#include "Pch.h"
#include "AnimationBinding.h"
#include "Scene.h"
#include "Entity.h"
#include "Components.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include <glm/gtx/quaternion.hpp>
#include <cmath>

namespace Helios
{
	namespace AnimationBinding
	{
		namespace
		{
			entt::entity FindChildByName(Scene& scene, entt::entity parent, const std::string& name)
			{
				entt::entity found = entt::null;
				const auto view = scene.GetRegistry().view<ParentComponent, NameComponent>();
				for (const entt::entity candidate : view)
				{
					const auto& link = view.get<ParentComponent>(candidate);
					const auto& label = view.get<NameComponent>(candidate);
					if (link.m_Parent != parent || label.m_Name != name)
						continue;

					/* 同名取 ID 最小的那个，结果不随遍历顺序漂 */
					if (found == entt::null || entt::to_integral(candidate) < entt::to_integral(found))
						found = candidate;
				}
				return found;
			}
		}

		entt::entity ResolvePath(Scene& scene, entt::entity root, const std::string& path)
		{
			if (path.empty())
				return root;

			entt::entity current = root;
			size_t start = 0;
			while (current != entt::null)
			{
				const size_t slash = path.find('/', start);
				const std::string segment = path.substr(start,
					slash == std::string::npos ? std::string::npos : slash - start);

				if (!segment.empty())
					current = FindChildByName(scene, current, segment);

				if (slash == std::string::npos)
					break;
				start = slash + 1;
			}
			return current;
		}

		const ComponentDesc* FindComponent(const std::string& name)
		{
			for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
			{
				if (desc.Name != nullptr && name == desc.Name)
					return &desc;
			}
			return nullptr;
		}

		const FieldDesc* FindField(const ComponentDesc& desc, const std::string& name)
		{
			for (const FieldDesc& field : desc.Fields)
			{
				if (field.Name != nullptr && name == field.Name)
					return &field;
			}
			return nullptr;
		}

		bool IsVisibilityTrack(const AnimationTrack& track)
		{
			return track.Component == "Visibility" && track.Property == "Visible";
		}

		bool ReadVisibility(Scene& scene, entt::entity entity)
		{
			if (entity == entt::null || !scene.IsEntityValid(entity))
				return true;

			const auto* visibility = scene.GetRegistry().try_get<VisibilityComponent>(entity);
			return visibility == nullptr || visibility->m_Visible;
		}

		void WriteVisibility(Scene& scene, entt::entity entity, bool visible)
		{
			if (entity == entt::null || !scene.IsEntityValid(entity))
				return;

			entt::registry& registry = scene.GetRegistry();
			if (visible)
			{
				if (registry.all_of<VisibilityComponent>(entity))
					registry.remove<VisibilityComponent>(entity);
				return;
			}

			VisibilityComponent& visibility = registry.all_of<VisibilityComponent>(entity)
				? registry.get<VisibilityComponent>(entity)
				: registry.emplace<VisibilityComponent>(entity, false);
			visibility.m_Visible = false;
		}

		bool ResolveTarget(Scene& scene, entt::entity animator, const AnimationTrack& track, Target& out)
		{
			out = {};
			out.Entity = ResolvePath(scene, animator, track.Path);
			if (out.Entity == entt::null)
				return false;

			/* 可见性没有可查的组件与字段：实体在就算解析成功 */
			if (IsVisibilityTrack(track))
			{
				out.Visibility = true;
				return true;
			}

			out.Component = FindComponent(track.Component);
			if (out.Component == nullptr || out.Component->Has == nullptr || out.Component->GetPtr == nullptr)
				return false;

			Entity entity{ out.Entity, scene.shared_from_this() };
			if (!out.Component->Has(entity))
				return false;

			out.Field = FindField(*out.Component, track.Property);
			return out.Field != nullptr && out.Field->ValueSize != 0;
		}

		void* GetComponentPtr(Scene& scene, const Target& target)
		{
			if (target.Component == nullptr || target.Component->GetPtr == nullptr)
				return nullptr;
			if (!scene.IsEntityValid(target.Entity))
				return nullptr;

			Entity entity{ target.Entity, scene.shared_from_this() };
			if (target.Component->Has == nullptr || !target.Component->Has(entity))
				return nullptr;
			return target.Component->GetPtr(entity);
		}

		bool ReadValue(const FieldDesc& field, const void* component, TrackValueType value_type, glm::vec4& out_value)
		{
			switch (field.Type)
			{
			case FieldType::Float:
			{
				float value = 0.0f;
				if (field.Get != nullptr)
					field.Get(component, &value);
				else
					std::memcpy(&value, static_cast<const uint8_t*>(component) + field.Offset, sizeof(float));
				out_value = glm::vec4(value, 0.0f, 0.0f, 0.0f);
				return true;
			}
			case FieldType::Int:
			case FieldType::Enum:
			{
				int value = 0;
				if (field.Get != nullptr)
					field.Get(component, &value);
				else
					std::memcpy(&value, static_cast<const uint8_t*>(component) + field.Offset, sizeof(int));
				out_value = glm::vec4(static_cast<float>(value), 0.0f, 0.0f, 0.0f);
				return true;
			}
			case FieldType::Bool:
			{
				bool value = false;
				if (field.Get != nullptr)
					field.Get(component, &value);
				else
					std::memcpy(&value, static_cast<const uint8_t*>(component) + field.Offset, sizeof(bool));
				out_value = glm::vec4(value ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
				return true;
			}
			case FieldType::Vec3:
			{
				glm::vec3 value(0.0f);
				if (field.Get != nullptr)
					field.Get(component, &value);
				else
					std::memcpy(&value, static_cast<const uint8_t*>(component) + field.Offset, sizeof(glm::vec3));

				if (value_type == TrackValueType::Quat)
				{
					const glm::quat rotation(value);
					out_value = glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w);
				}
				else
				{
					out_value = glm::vec4(value, 0.0f);
				}
				return true;
			}
			case FieldType::Vec4:
			case FieldType::Color:
			{
				glm::vec4 value(0.0f);
				if (field.Get != nullptr)
					field.Get(component, &value);
				else
					std::memcpy(&value, static_cast<const uint8_t*>(component) + field.Offset, sizeof(glm::vec4));
				out_value = value;
				return true;
			}
			default:
				return false;
			}
		}

		bool WriteValue(const FieldDesc& field, void* component, TrackValueType value_type, const glm::vec4& value)
		{
			const auto write = [&field, component](const void* data)
			{
				if (field.Set != nullptr)
					field.Set(component, data);
				else
					std::memcpy(static_cast<uint8_t*>(component) + field.Offset, data, field.ValueSize);
			};

			switch (field.Type)
			{
			case FieldType::Float:
			{
				const float scalar = value.x;
				write(&scalar);
				return true;
			}
			case FieldType::Int:
			case FieldType::Enum:
			{
				const int scalar = static_cast<int>(std::lround(value.x));
				write(&scalar);
				return true;
			}
			case FieldType::Bool:
			{
				const bool scalar = value.x != 0.0f;
				write(&scalar);
				return true;
			}
			case FieldType::Vec3:
			{
				glm::vec3 target(value);
				if (value_type == TrackValueType::Quat)
					target = glm::eulerAngles(glm::quat(value.w, value.x, value.y, value.z));
				write(&target);
				return true;
			}
			case FieldType::Vec4:
			case FieldType::Color:
			{
				const glm::vec4 target = value;
				write(&target);
				return true;
			}
			default:
				return false;
			}
		}
	}
}
