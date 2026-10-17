#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <entt/entt.hpp>

namespace Helios
{
	class Scene;
	class AnimationClip;
	struct AnimatorComponent;
	struct AnimationTrack;
	enum class TrackValueType : uint8_t;
	struct ComponentDesc;
	struct FieldDesc;

	/* 场景级的动画播放器：每个 AnimatorComponent 一个会话。
	 * 播放时把采样值写进组件字段，播放前对碰到的字段做快照，停止时还原 —— 场景数据保持原样。 */
	class AnimationRuntime
	{
	public:
		AnimationRuntime() = default;
		~AnimationRuntime() = default;

		/* 每帧推进（由 Scene::OnUpdate 在变换推送之前调用） */
		void Tick(Scene& scene, float delta_time);

		/* 停止全部播放并还原（保存场景 / 清空场景前调用） */
		void StopAll(Scene& scene);

		/* 动画事件回调：播到事件点时按名字通知（音效 / 特效 / 逻辑由使用方解释） */
		using EventHandler = std::function<void(entt::entity animator, const std::string& name)>;
		void SetEventHandler(EventHandler handler) { m_EventHandler = std::move(handler); }

		/* 丢弃所有会话，不写回（实体已经销毁时用） */
		void Clear() { m_Sessions.clear(); }

		[[nodiscard]] bool IsAnyPlaying() const;

	private:
		/* 一条绑定好的轨道：目标实体、组件与字段、播放前的值 */
		struct BoundTrack
		{
			size_t TrackIndex{ 0 };              /* 在 clip 轨道表里的下标（重新采样时按它取） */
			entt::entity Entity{ entt::null };
			const ComponentDesc* Component{ nullptr };
			const FieldDesc* Field{ nullptr };
			std::vector<uint8_t> Snapshot;
			/* 可见性轨道：没有组件与字段，快照记的是实体自己的显隐 */
			bool Visibility{ false };
			bool VisibilitySnapshot{ true };
		};

		struct Session
		{
			entt::entity Animator{ entt::null };
			const AnimationClip* Clip{ nullptr };
			bool Active{ false };
			bool Begun{ false };
			/* 上一帧是不是在播放：用来认「播到头停下再按播放」这件事 */
			bool WasPlaying{ false };
			float Time{ 0.0f };
			/* 事件判定用的上一帧时间：播放时按 (上一帧, 这一帧] 触发 */
			float PreviousTime{ 0.0f };
			bool HasPreviousTime{ false };
			/* 过渡：正在淡出的旧片段，自己一份时间与绑定（结束后空） */
			const AnimationClip* FadingClip{ nullptr };
			std::vector<BoundTrack> FadingTracks;
			float FadingTime{ 0.0f };
			float FadeDuration{ 0.0f };
			float FadeElapsed{ 0.0f };
			/* 定格时只在时间或片段内容变了才重新采样：手改属性不该被每帧覆盖 */
			bool Applied{ false };
			float AppliedTime{ 0.0f };
			uint32_t AppliedRevision{ 0xFFFFFFFFu };
			std::vector<BoundTrack> Tracks;
		};

		/* 绑定 clip 的全部轨道；解析不到的轨道跳过（其余照播） */
		void BindSession(Scene& scene, Session& session);
		/* 接住 AnimatorComponent::Play 的请求：旧绑定整份挪进淡出层，再重绑新片段 */
		void ApplyPendingClip(Scene& scene, Session& session, AnimatorComponent& component);
		/* 两层里同一个目标（实体 + 组件 + 字段） */
		[[nodiscard]] static const BoundTrack* FindBound(const std::vector<BoundTrack>& tracks,
			const BoundTrack& key);
		/* 把一条绑定的值写进场景（可见性单独走三元判断） */
		void WriteBound(Scene& scene, const BoundTrack& bound, TrackValueType type, const glm::vec4& value);
		/* 还原一条绑定在播放前的快照 */
		void RestoreBound(Scene& scene, const BoundTrack& bound);
		/* 还原快照并清空绑定 */
		void StopSession(Scene& scene, Session& session);

		[[nodiscard]] Session* FindSession(entt::entity animator);

		std::vector<Session> m_Sessions{};
		EventHandler m_EventHandler{};
	};
}
