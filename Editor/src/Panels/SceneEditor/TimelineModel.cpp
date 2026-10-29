#include "Pch.h"
#include "TimelineModel.h"
#include "Helios/Scene/AnimationBinding.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/Scene.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Helios
{
	namespace TimelineModel
	{
		namespace
		{
			/* 属性枚举：Animator 自己 + 整棵子树（深度上限兜住畸形层级） */
			void CollectSubtree(Scene& scene, entt::entity root, std::vector<entt::entity>& out)
			{
				out.push_back(root);
				std::vector<entt::entity> pending{ root };
				size_t guard = 0;

				while (!pending.empty() && guard++ < 4096)
				{
					const entt::entity parent = pending.back();
					pending.pop_back();

					const auto view = scene.GetRegistry().view<ParentComponent>();
					for (const entt::entity candidate : view)
					{
						if (view.get<ParentComponent>(candidate).m_Parent != parent)
							continue;
						out.push_back(candidate);
						pending.push_back(candidate);
					}
				}
			}

			std::string EntityName(Scene& scene, entt::entity entity)
			{
				if (const auto* name = scene.GetRegistry().try_get<NameComponent>(entity))
					return name->m_Name;
				return "Entity";
			}

			/* 从根往下的名字路径；根自己返回空串 */
			std::string BuildPath(Scene& scene, entt::entity root, entt::entity entity)
			{
				if (entity == root)
					return {};

				std::vector<std::string> segments;
				entt::entity current = entity;
				for (size_t depth = 0; current != entt::null && current != root && depth < 64; ++depth)
				{
					segments.push_back(EntityName(scene, current));
					current = scene.GetParent(current);
				}

				if (current != root)
					return {};

				std::reverse(segments.begin(), segments.end());
				std::string path;
				for (const std::string& segment : segments)
					path += (path.empty() ? "" : "/") + segment;
				return path;
			}

			bool IsAnimatableField(const FieldDesc& field)
			{
				if (field.Name == nullptr || field.ValueSize == 0)
					return false;

				switch (field.Type)
				{
				case FieldType::Float:
				case FieldType::Int:
				case FieldType::Enum:
				case FieldType::Bool:
				case FieldType::Vec3:
				case FieldType::Vec4:
				case FieldType::Color:
					return true;
				default:
					return false;
				}
			}
		}

		int FindKey(const AnimationTrack& track, float time, float tolerance)
		{
			int best = -1;
			float best_delta = tolerance;

			for (int index = 0; index < static_cast<int>(track.Keys.size()); ++index)
			{
				const float delta = std::fabs(track.Keys[index].Time - time);
				if (delta <= best_delta)
				{
					best = index;
					best_delta = delta;
				}
			}
			return best;
		}

		int InsertOrUpdateKey(AnimationTrack& track, float time, const glm::vec4& value, KeyInterp interp)
		{
			const int existing = FindKey(track, time);
			if (existing >= 0)
			{
				track.Keys[existing].Value = value;
				track.Interp = interp;
				return existing;
			}

			AnimationKey key;
			key.Time = time;
			key.Value = value;
			key.HasTangent = false;

			track.Interp = interp;
			track.Keys.emplace_back(key);
			std::sort(track.Keys.begin(), track.Keys.end(),
				[](const AnimationKey& lhs, const AnimationKey& rhs) { return lhs.Time < rhs.Time; });

			return FindKey(track, time);
		}

		bool RemoveKey(AnimationTrack& track, int index)
		{
			if (index < 0 || index >= static_cast<int>(track.Keys.size()))
				return false;

			track.Keys.erase(track.Keys.begin() + index);
			return true;
		}

		bool MoveKey(AnimationTrack& track, int index, float new_time, int& out_new_index)
		{
			if (index < 0 || index >= static_cast<int>(track.Keys.size()))
				return false;

			new_time = std::max(new_time, 0.0f);

			/* 挪到别的帧上就并过去（位置取目标帧，值保留被拖的那个） */
			const int occupant = FindKey(track, new_time);
			if (occupant >= 0 && occupant != index)
			{
				track.Keys[occupant].Value = track.Keys[index].Value;
				track.Keys.erase(track.Keys.begin() + index);
				out_new_index = occupant - (occupant > index ? 1 : 0);
				return true;
			}

			AnimationKey key = track.Keys[index];
			key.Time = new_time;
			track.Keys.erase(track.Keys.begin() + index);
			track.Keys.emplace_back(key);
			std::sort(track.Keys.begin(), track.Keys.end(),
				[](const AnimationKey& lhs, const AnimationKey& rhs) { return lhs.Time < rhs.Time; });

			out_new_index = FindKey(track, new_time);
			return out_new_index >= 0;
		}

		bool SetKeyValue(AnimationTrack& track, int index, const glm::vec4& value)
		{
			if (index < 0 || index >= static_cast<int>(track.Keys.size()))
				return false;

			track.Keys[index].Value = value;
			return true;
		}

		void RemoveKeys(std::vector<AnimationTrack>& tracks, const std::vector<KeyRef>& keys)
		{
			std::unordered_map<int, std::vector<int>> by_track;
			for (const KeyRef& ref : keys)
			{
				if (ref.Track < 0 || ref.Track >= static_cast<int>(tracks.size()))
					continue;
				if (ref.Key < 0 || ref.Key >= static_cast<int>(tracks[ref.Track].Keys.size()))
					continue;
				by_track[ref.Track].push_back(ref.Key);
			}

			for (auto& [track_index, indices] : by_track)
			{
				std::sort(indices.begin(), indices.end(), std::greater<int>());
				indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
				for (const int index : indices)
					tracks[track_index].Keys.erase(tracks[track_index].Keys.begin() + index);
			}
		}

		void MoveKeys(std::vector<AnimationTrack>& tracks, const std::vector<AnimationTrack>& before,
			const std::vector<KeyRef>& selection, float delta, std::vector<KeyRef>& out_selection)
		{
			out_selection.clear();
			if (std::fabs(delta) < 1e-6f)
			{
				out_selection = selection;
				return;
			}

			/* 先按轨道收集被选中的原始时间：后面重建时按时间认帧 */
			std::unordered_map<int, std::vector<float>> selected_times;
			for (const KeyRef& ref : selection)
			{
				if (ref.Track < 0 || ref.Track >= static_cast<int>(before.size()))
					continue;
				const auto& keys = before[ref.Track].Keys;
				if (ref.Key < 0 || ref.Key >= static_cast<int>(keys.size()))
					continue;
				selected_times[ref.Track].push_back(keys[ref.Key].Time);
			}

			for (const auto& [track_index, times] : selected_times)
			{
				if (track_index >= static_cast<int>(tracks.size()))
					continue;

				struct Staged
				{
					AnimationKey Key;
					bool Selected{ false };
				};

				std::vector<Staged> staged;
				for (const AnimationKey& key : before[track_index].Keys)
				{
					bool selected = false;
					for (const float time : times)
					{
						if (std::fabs(key.Time - time) <= 1e-4f)
						{
							selected = true;
							break;
						}
					}

					Staged item;
					item.Key = key;
					item.Selected = selected;
					if (selected)
						item.Key.Time = std::max(0.0f, key.Time + delta);
					staged.push_back(item);
				}

				/* 时间升序；同一时刻被选中的排前面（合并时值优先保留被拖的） */
				std::stable_sort(staged.begin(), staged.end(),
					[](const Staged& lhs, const Staged& rhs)
					{
						if (std::fabs(lhs.Key.Time - rhs.Key.Time) > 1e-4f)
							return lhs.Key.Time < rhs.Key.Time;
						return lhs.Selected && !rhs.Selected;
					});

				std::vector<AnimationKey> rebuilt;
				for (const Staged& item : staged)
				{
					if (!rebuilt.empty() && std::fabs(rebuilt.back().Time - item.Key.Time) <= 1e-4f)
					{
						if (item.Selected)
							rebuilt.back() = item.Key;
						continue;
					}
					rebuilt.push_back(item.Key);
				}

				tracks[track_index].Keys = std::move(rebuilt);

				/* 回填新下标（落点重合被并掉的帧自然消失） */
				for (const Staged& item : staged)
				{
					if (!item.Selected)
						continue;

					for (int index = 0; index < static_cast<int>(tracks[track_index].Keys.size()); ++index)
					{
						if (std::fabs(tracks[track_index].Keys[index].Time - item.Key.Time) > 1e-4f)
							continue;

						const KeyRef ref{ track_index, index };
						const bool exists = std::any_of(out_selection.begin(), out_selection.end(),
							[&ref](const KeyRef& other) { return other.Track == ref.Track && other.Key == ref.Key; });
						if (!exists)
							out_selection.push_back(ref);
						break;
					}
				}
			}
		}

		TrackValueType ValueTypeForField(const FieldDesc& field)
		{
			switch (field.Type)
			{
			case FieldType::Vec3:  return TrackValueType::Vec3;
			case FieldType::Vec4:
			case FieldType::Color: return TrackValueType::Vec4;
			default:               return TrackValueType::Float;
			}
		}

		bool ValuesEqual(TrackValueType value_type, const glm::vec4& lhs, const glm::vec4& rhs, float tolerance)
		{
			if (value_type == TrackValueType::Quat)
			{
				/* 四元数用夹角判：1e-5 约 0.26 度，小角度旋转也要能录进去 */
				const float dot = lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
				return std::fabs(dot) >= 1.0f - 1e-5f;
			}

			return std::fabs(lhs.x - rhs.x) <= tolerance
				&& std::fabs(lhs.y - rhs.y) <= tolerance
				&& std::fabs(lhs.z - rhs.z) <= tolerance
				&& std::fabs(lhs.w - rhs.w) <= tolerance;
		}

		bool ReadCurrentValue(Scene& scene, entt::entity animator, const AnimationTrack& track, glm::vec4& out_value)
		{
			/* 可见性不是组件字段：直接读实体自己的显隐 */
			if (AnimationBinding::IsVisibilityTrack(track))
			{
				AnimationBinding::Target visibility_target;
				if (!AnimationBinding::ResolveTarget(scene, animator, track, visibility_target))
					return false;

				const float visible = AnimationBinding::ReadVisibility(scene, visibility_target.Entity) ? 1.0f : 0.0f;
				out_value = glm::vec4(visible, 0.0f, 0.0f, 0.0f);
				return true;
			}

			AnimationBinding::Target target;
			if (!AnimationBinding::ResolveTarget(scene, animator, track, target))
				return false;

			void* component = AnimationBinding::GetComponentPtr(scene, target);
			if (component == nullptr || target.Field == nullptr)
				return false;

			return AnimationBinding::ReadValue(*target.Field, component, track.ValueType, out_value);
		}

		bool RecordTrack(Scene& scene, entt::entity animator, AnimationTrack& track, float time)
		{
			glm::vec4 current;
			if (!ReadCurrentValue(scene, animator, track, current))
				return false;

			/* 没有关键帧时采样失败，直接按当前值落一帧 */
			glm::vec4 sampled;
			if (AnimationClip::Sample(track, time, sampled)
				&& ValuesEqual(track.ValueType, current, sampled))
				return false;

			InsertOrUpdateKey(track, time, current, track.Interp);
			return true;
		}

		std::vector<PropertyEntry> CollectAnimatableProperties(Scene& scene, entt::entity animator)
		{
			std::vector<PropertyEntry> entries;
			if (animator == entt::null || !scene.IsEntityValid(animator))
				return entries;

			std::vector<entt::entity> entities;
			CollectSubtree(scene, animator, entities);

			for (const entt::entity entity : entities)
			{
				const std::string path = BuildPath(scene, animator, entity);
				const std::string name = EntityName(scene, entity);
				Entity entity_handle{ entity, scene.shared_from_this() };

				for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
				{
					if (desc.Name == nullptr || desc.Has == nullptr || !desc.Has(entity_handle))
						continue;

					/* 可见性走下面的固定条目：组件是「有则隐」的，缺席时也该能加轨道 */
					if (std::strcmp(desc.Name, "Visibility") == 0)
						continue;

					for (const FieldDesc& field : desc.Fields)
					{
						if (!IsAnimatableField(field))
							continue;

						PropertyEntry entry;
						entry.Path = path;
						entry.EntityName = name;
						entry.Component = desc.Name;
						entry.Property = field.Name;

						/* 旋转存的是欧拉角，轨道按四元数存，插值才不会绕远路 */
						entry.ValueType = (entry.Component == "Transform" && entry.Property == "Rotation")
							? TrackValueType::Quat
							: ValueTypeForField(field);
						entries.emplace_back(std::move(entry));
					}
				}

				/* 每个节点都有一条可见性轨道：值是 0/1，缺省按阶梯插值 */
				PropertyEntry visibility;
				visibility.Path = path;
				visibility.EntityName = name;
				visibility.Component = "Visibility";
				visibility.Property = "Visible";
				visibility.ValueType = TrackValueType::Float;
				entries.emplace_back(std::move(visibility));
			}

			std::sort(entries.begin(), entries.end(),
				[](const PropertyEntry& lhs, const PropertyEntry& rhs)
				{
					if (lhs.Path != rhs.Path) return lhs.Path < rhs.Path;
					if (lhs.Component != rhs.Component) return lhs.Component < rhs.Component;
					return lhs.Property < rhs.Property;
				});
			return entries;
		}

		std::string MakeTrackLabel(const AnimationTrack& track, const std::string& animator_name)
		{
			const std::string& owner = track.Path.empty() ? animator_name : track.Path;
			return owner + " · " + track.Component + "." + track.Property;
		}
	}
}
