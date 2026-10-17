#include "Pch.h"
#include "AnimationRuntime.h"
#include "AnimationBinding.h"
#include "AnimationClip.h"
#include "Components.h"
#include "Entity.h"
#include "Scene.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Helios
{
	namespace
	{
		/* 过渡混合：向量线性、四元数 slerp（分量直接插值会变短再归一化，角速度不匀） */
		glm::vec4 BlendTrackValue(TrackValueType type, const glm::vec4& from, const glm::vec4& to, float weight)
		{
			if (type != TrackValueType::Quat)
				return from + (to - from) * weight;

			const glm::quat lhs(from.w, from.x, from.y, from.z);
			const glm::quat rhs(to.w, to.x, to.y, to.z);
			const glm::quat mixed = glm::slerp(lhs, rhs, weight);
			return glm::vec4(mixed.x, mixed.y, mixed.z, mixed.w);
		}

		/* 快照 / 还原按原始字节走：访问器字段交给 Get/Set，偏移字段直接 memcpy */
		void ReadFieldBytes(const FieldDesc& field, const void* component, void* out_value)
		{
			if (field.Get != nullptr)
			{
				field.Get(component, out_value);
				return;
			}
			std::memcpy(out_value, static_cast<const uint8_t*>(component) + field.Offset, field.ValueSize);
		}

		void WriteFieldBytes(const FieldDesc& field, void* component, const void* value)
		{
			if (field.Set != nullptr)
			{
				field.Set(component, value);
				return;
			}
			std::memcpy(static_cast<uint8_t*>(component) + field.Offset, value, field.ValueSize);
		}
	}

	void AnimationRuntime::Tick(Scene& scene, float delta_time)
	{
		PROFILE_FUNCTION();

		entt::registry& registry = scene.GetRegistry();

		/* 会话清理：实体没了或组件被移除，先还原再丢 */
		for (size_t index = 0; index < m_Sessions.size();)
		{
			Session& session = m_Sessions[index];
			const bool valid = registry.valid(session.Animator) && registry.all_of<AnimatorComponent>(session.Animator);
			if (valid)
			{
				++index;
				continue;
			}

			if (session.Active)
				StopSession(scene, session);
			m_Sessions.erase(m_Sessions.begin() + static_cast<std::ptrdiff_t>(index));
		}

		/* 按实体 ID 升序推进：多个播放器写同一个字段时结果稳定 */
		std::vector<entt::entity> animators;
		for (const entt::entity entity : registry.view<AnimatorComponent>())
			animators.push_back(entity);
		std::sort(animators.begin(), animators.end(),
			[](entt::entity lhs, entt::entity rhs)
			{
				return entt::to_integral(lhs) < entt::to_integral(rhs);
			});

		for (const entt::entity entity : animators)
		{
			AnimatorComponent& component = registry.get<AnimatorComponent>(entity);

			Session* session = FindSession(entity);
			if (session == nullptr)
			{
				m_Sessions.emplace_back();
				session = &m_Sessions.back();
				session->Animator = entity;
			}

			/* Play(clip, fade) 的请求：过渡场景下旧绑定整份让给淡出层 */
			if (component.m_PendingClip != nullptr)
				ApplyPendingClip(scene, *session, component);

			/* 换 clip：先把旧姿势还回去，再按新 clip 重新开始 */
			if (session->Clip != component.m_Clip.get())
			{
				if (session->Active)
					StopSession(scene, *session);
				session->Clip = component.m_Clip.get();
				session->Begun = false;
				session->Time = 0.0f;
				session->Applied = false;
			}

			if (component.m_Clip == nullptr)
			{
				if (session->Active)
					StopSession(scene, *session);
				component.m_Playing = false;
				component.m_Scrubbing = false;
				component.m_Time = 0.0f;
				continue;
			}

			/* 首次见到这个播放器：自动播放只认一次，之后的停止是用户的决定 */
			if (!session->Begun)
			{
				session->Begun = true;
				session->Time = 0.0f;
				if (component.m_AutoPlay)
					component.m_Playing = true;
			}

			/* 播放与拖播放头都要有姿势：前者推进时间，后者按 UI 给的时间定格 */
			const bool wants_pose = component.m_Playing || component.m_Scrubbing;
			if (wants_pose && !session->Active)
			{
				BindSession(scene, *session);
				session->Active = true;
				session->Time = component.m_Playing ? 0.0f : component.m_Time;
				/* 从头播：上一帧时间给 -1，0 秒处的事件也能触发一次；拖播放头则从当前时间接着算 */
				session->PreviousTime = component.m_Playing ? -1.0f : session->Time;
				session->HasPreviousTime = true;
			}
			else if (!wants_pose && session->Active)
			{
				StopSession(scene, *session);
			}

			if (!session->Active)
			{
				component.m_Time = session->Time;
				session->WasPlaying = false;
				continue;
			}

			const float duration = std::max(component.m_Clip->GetDuration(), 0.0f);

			/* 不循环、已经播到末帧，又刚按下播放：从头再来一遍。
			 * 否则时间被钳在末帧，按播放什么也不会发生（预览面板的 Preview 想干的就是这件事） */
			if (!component.m_Loop && component.m_Playing && !session->WasPlaying
				&& duration > 1e-5f && session->Time >= duration - 1e-4f)
			{
				session->Time = 0.0f;
			}

			if (component.m_Playing)
			{
				session->Time += delta_time * component.m_Speed;
				if (duration > 1e-5f)
				{
					if (component.m_Loop)
					{
						session->Time = std::fmod(session->Time, duration);
						if (session->Time < 0.0f)
							session->Time += duration;
					}
					else
					{
						session->Time = std::min(std::max(session->Time, 0.0f), duration);
					}
				}
				else
				{
					session->Time = 0.0f;
				}
			}
			else
			{
				session->Time = (duration > 1e-5f) ? std::min(std::max(component.m_Time, 0.0f), duration) : 0.0f;
			}

			/* 事件：只在播放推进时触发；跨循环回绕拆成两段，起点那一侧含 0 秒 */
			if (component.m_Playing && m_EventHandler != nullptr && session->HasPreviousTime)
			{
				const float from = session->PreviousTime;
				const float to = session->Time;
				const auto fire_range = [&](float range_from, float range_to)
				{
					for (const AnimationEvent& event : component.m_Clip->GetEvents())
					{
						if (event.Time > range_from && event.Time <= range_to)
							m_EventHandler(entity, event.Name);
					}
				};

				if (to >= from || duration <= 1e-5f)
					fire_range(from, to);
				else
				{
					fire_range(from, duration);
					fire_range(-1.0f, to);
				}
			}
			session->PreviousTime = session->Time;

			/* 淡出层：时间照常推进（跟着播放/循环规则），权重线性走完 */
			const bool fading = (session->FadingClip != nullptr && session->FadeDuration > 1e-5f);
			if (fading)
			{
				session->FadeElapsed += delta_time;
				session->FadingTime += delta_time * component.m_Speed;

				const float fading_duration = std::max(session->FadingClip->GetDuration(), 0.0f);
				if (fading_duration > 1e-5f)
				{
					if (component.m_Loop)
					{
						session->FadingTime = std::fmod(session->FadingTime, fading_duration);
						if (session->FadingTime < 0.0f)
							session->FadingTime += fading_duration;
					}
					else
					{
						session->FadingTime = std::min(std::max(session->FadingTime, 0.0f), fading_duration);
					}
				}
				else
				{
					session->FadingTime = 0.0f;
				}
			}
			const float fade_weight = fading
				? std::clamp(session->FadeElapsed / session->FadeDuration, 0.0f, 1.0f) : 1.0f;

			/* 定格时只在"时间变了"或"片段内容变了"才重写组件：
			 * 每帧重采样会把手改的属性覆盖掉，也没必要 */
			const uint32_t clip_revision = component.m_Clip->GetRevision();
			const bool needs_sample = component.m_Playing || fading
				|| !session->Applied
				|| std::fabs(session->Time - session->AppliedTime) > 1e-6f
				|| session->AppliedRevision != clip_revision;

			if (needs_sample)
			{
				const std::vector<AnimationTrack>& tracks = component.m_Clip->GetTracks();
				for (BoundTrack& bound : session->Tracks)
				{
					if (bound.TrackIndex >= tracks.size())
						continue;
					if (!bound.Visibility && (bound.Component == nullptr || bound.Field == nullptr))
						continue;
					if (!registry.valid(bound.Entity))
						continue;

					const AnimationTrack& track = tracks[bound.TrackIndex];
					if (!track.Enabled)
						continue;

					glm::vec4 sample;
					if (!component.m_Clip->Sample(track, session->Time, sample))
						continue;

					/* 过渡中：同一个目标在淡出层里也有值时按权重混合 */
					if (fading)
					{
						const BoundTrack* fading_bound = FindBound(session->FadingTracks, bound);
						const std::vector<AnimationTrack>& fading_tracks = session->FadingClip->GetTracks();
						if (fading_bound != nullptr && fading_bound->TrackIndex < fading_tracks.size())
						{
							const AnimationTrack& fading_track = fading_tracks[fading_bound->TrackIndex];
							glm::vec4 fading_sample;
							if (fading_track.Enabled
								&& session->FadingClip->Sample(fading_track, session->FadingTime, fading_sample))
							{
								sample = BlendTrackValue(track.ValueType, fading_sample, sample, fade_weight);
							}
						}
					}

					WriteBound(scene, bound, track.ValueType, sample);
				}

				/* 只被淡出层驱动的目标：过渡期间照旧写，权重走满就把播放前的值还回去 */
				if (fading)
				{
					const std::vector<AnimationTrack>& fading_tracks = session->FadingClip->GetTracks();
					for (BoundTrack& fading_bound : session->FadingTracks)
					{
						if (FindBound(session->Tracks, fading_bound) != nullptr)
							continue;
						if (fading_bound.TrackIndex >= fading_tracks.size() || !registry.valid(fading_bound.Entity))
							continue;

						const AnimationTrack& fading_track = fading_tracks[fading_bound.TrackIndex];
						if (!fading_track.Enabled)
							continue;

						if (fade_weight >= 1.0f)
						{
							RestoreBound(scene, fading_bound);
							continue;
						}

						glm::vec4 fading_sample;
						if (session->FadingClip->Sample(fading_track, session->FadingTime, fading_sample))
							WriteBound(scene, fading_bound, fading_track.ValueType, fading_sample);
					}
				}

				session->Applied = true;
				session->AppliedTime = session->Time;
				session->AppliedRevision = clip_revision;
			}

			/* 过渡走完：淡出层收摊（只被它驱动的目标上面已经还原） */
			if (fading && session->FadeElapsed >= session->FadeDuration)
			{
				session->FadingClip = nullptr;
				session->FadingTracks.clear();
				session->FadeDuration = 0.0f;
				session->FadeElapsed = 0.0f;
			}

			component.m_Time = session->Time;
			session->WasPlaying = component.m_Playing;
		}
	}

	void AnimationRuntime::BindSession(Scene& scene, Session& session)
	{
		session.Tracks.clear();

		if (session.Clip == nullptr)
			return;

		const std::vector<AnimationTrack>& tracks = session.Clip->GetTracks();
		for (size_t index = 0; index < tracks.size(); ++index)
		{
			const AnimationTrack& track = tracks[index];

			AnimationBinding::Target target;
			if (!AnimationBinding::ResolveTarget(scene, session.Animator, track, target))
			{
				CORE_LOG_WARN("Animation track target not found: '{}' ({}.{}).",
					track.Path, track.Component, track.Property);
				continue;
			}

			BoundTrack bound;
			bound.TrackIndex = index;
			bound.Entity = target.Entity;

			if (target.Visibility)
			{
				bound.Visibility = true;
				bound.VisibilitySnapshot = AnimationBinding::ReadVisibility(scene, target.Entity);
				session.Tracks.emplace_back(std::move(bound));
				continue;
			}

			void* component = AnimationBinding::GetComponentPtr(scene, target);
			if (component == nullptr)
				continue;

			bound.Component = target.Component;
			bound.Field = target.Field;
			bound.Snapshot.resize(target.Field->ValueSize);
			ReadFieldBytes(*target.Field, component, bound.Snapshot.data());
			session.Tracks.emplace_back(std::move(bound));
		}
	}

	void AnimationRuntime::ApplyPendingClip(Scene& scene, Session& session, AnimatorComponent& component)
	{
		SharedPtr<AnimationClip> next = std::move(component.m_PendingClip);
		const float fade = std::max(component.m_PendingFade, 0.0f);
		component.m_PendingClip = nullptr;
		component.m_PendingFade = 0.0f;

		if (next.get() == component.m_Clip.get())
			return;

		/* 过渡：旧片段连绑定带时间整份挪进淡出层；新片段直接换绑，不还原旧姿势 */
		if (session.Active && fade > 1e-4f && session.Clip != nullptr && session.FadingClip == nullptr)
		{
			session.FadingClip = session.Clip;
			session.FadingTracks = std::move(session.Tracks);
			session.FadingTime = session.Time;
			session.FadeDuration = fade;
			session.FadeElapsed = 0.0f;

			component.m_Clip = std::move(next);
			session.Clip = component.m_Clip.get();
			session.Begun = true;
			session.Time = 0.0f;
			session.Applied = false;
			session.HasPreviousTime = false;
			BindSession(scene, session);

			/* 两边都有的目标沿用淡出层的快照：停止时还原的是播放前那份，不是半路的样子 */
			for (BoundTrack& bound : session.Tracks)
			{
				if (const BoundTrack* fading_bound = FindBound(session.FadingTracks, bound))
				{
					if (fading_bound->Snapshot.size() == bound.Snapshot.size())
						bound.Snapshot = fading_bound->Snapshot;
				}
			}
			return;
		}

		/* 没有过渡（或当前没在播）：照常换，下面的换片段判断会先还原旧姿势 */
		component.m_Clip = std::move(next);
	}

	const AnimationRuntime::BoundTrack* AnimationRuntime::FindBound(const std::vector<BoundTrack>& tracks,
		const BoundTrack& key)
	{
		for (const BoundTrack& track : tracks)
		{
			if (track.Entity == key.Entity && track.Component == key.Component
				&& track.Field == key.Field && track.Visibility == key.Visibility)
				return &track;
		}
		return nullptr;
	}

	void AnimationRuntime::WriteBound(Scene& scene, const BoundTrack& bound, TrackValueType type,
		const glm::vec4& value)
	{
		if (bound.Visibility)
		{
			AnimationBinding::WriteVisibility(scene, bound.Entity, value.x > 0.5f);
			return;
		}

		if (bound.Component == nullptr || bound.Field == nullptr || !scene.IsEntityValid(bound.Entity))
			return;

		Entity target{ bound.Entity, scene.shared_from_this() };
		if (bound.Component->Has == nullptr || !bound.Component->Has(target))
			return;

		void* component = (bound.Component->GetPtr != nullptr) ? bound.Component->GetPtr(target) : nullptr;
		if (component == nullptr)
			return;

		AnimationBinding::WriteValue(*bound.Field, component, type, value);
	}

	void AnimationRuntime::RestoreBound(Scene& scene, const BoundTrack& bound)
	{
		if (!scene.IsEntityValid(bound.Entity))
			return;

		if (bound.Visibility)
		{
			AnimationBinding::WriteVisibility(scene, bound.Entity, bound.VisibilitySnapshot);
			return;
		}

		if (bound.Component == nullptr || bound.Field == nullptr || bound.Snapshot.empty())
			return;

		Entity target{ bound.Entity, scene.shared_from_this() };
		if (bound.Component->Has == nullptr || !bound.Component->Has(target))
			return;

		void* component = (bound.Component->GetPtr != nullptr) ? bound.Component->GetPtr(target) : nullptr;
		if (component == nullptr)
			return;

		WriteFieldBytes(*bound.Field, component, bound.Snapshot.data());
	}

	void AnimationRuntime::StopSession(Scene& scene, Session& session)
	{
		const auto& registry = scene.GetRegistry();

		/* 过渡中的旧片段：把它驱动的目标也还原回播放前的样子 */
		for (const BoundTrack& bound : session.FadingTracks)
			RestoreBound(scene, bound);
		session.FadingTracks.clear();
		session.FadingClip = nullptr;
		session.FadeDuration = 0.0f;
		session.FadeElapsed = 0.0f;

		for (const BoundTrack& bound : session.Tracks)
		{
			if (!registry.valid(bound.Entity))
				continue;

			if (bound.Visibility)
			{
				AnimationBinding::WriteVisibility(scene, bound.Entity, bound.VisibilitySnapshot);
				continue;
			}

			if (bound.Component == nullptr || bound.Field == nullptr)
				continue;

			Entity entity{ bound.Entity, scene.shared_from_this() };
			if (bound.Component->Has == nullptr || !bound.Component->Has(entity))
				continue;

			void* component = (bound.Component->GetPtr != nullptr) ? bound.Component->GetPtr(entity) : nullptr;
			if (component == nullptr)
				continue;

			WriteFieldBytes(*bound.Field, component, bound.Snapshot.data());
		}

		session.Tracks.clear();
		session.Active = false;
		session.Applied = false;
		session.HasPreviousTime = false;
	}

	void AnimationRuntime::StopAll(Scene& scene)
	{
		entt::registry& registry = scene.GetRegistry();

		for (Session& session : m_Sessions)
		{
			if (!session.Active)
				continue;

			StopSession(scene, session);
			if (registry.valid(session.Animator) && registry.all_of<AnimatorComponent>(session.Animator))
			{
				AnimatorComponent& component = registry.get<AnimatorComponent>(session.Animator);
				component.m_Playing = false;
				component.m_Scrubbing = false;
				component.m_Time = 0.0f;
			}
		}
		m_Sessions.clear();
	}

	bool AnimationRuntime::IsAnyPlaying() const
	{
		for (const Session& session : m_Sessions)
		{
			if (session.Active)
				return true;
		}
		return false;
	}

	AnimationRuntime::Session* AnimationRuntime::FindSession(entt::entity animator)
	{
		for (Session& session : m_Sessions)
		{
			if (session.Animator == animator)
				return &session;
		}
		return nullptr;
	}
}
