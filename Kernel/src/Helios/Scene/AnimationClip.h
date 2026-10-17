#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Helios/Common/Common.h"

namespace Helios
{
	/* 轨道值的语义类型：决定采样怎么插值、写回组件时怎么转换。
	 * Quat 专给旋转用（组件里存的是欧拉角，写完要转一道）。 */
	enum class TrackValueType : uint8_t
	{
		Float,
		Vec3,
		Vec4,
		Quat,
	};

	/* 关键帧插值：Step 只取前一个值；Linear 线性；Cubic 用切线做 Hermite（缺切线时按相邻帧自动求） */
	enum class KeyInterp : uint8_t
	{
		Step,
		Linear,
		Cubic,
	};

	/* 动画事件：播到该时间点时回调一次。名字交给使用方解释（音效 / 特效 / 逻辑），
	 * 动画系统只负责"到点通知"。 */
	struct AnimationEvent
	{
		float       Time{ 0.0f };
		std::string Name;
	};

	struct AnimationKey
	{
		float     Time{ 0.0f };
		glm::vec4 Value{ 0.0f };
		glm::vec4 InTangent{ 0.0f };
		glm::vec4 OutTangent{ 0.0f };
		bool      HasTangent{ false };

		/* 这一帧到下一帧的插值方式：HasInterp 为假时跟轨道走（旧文件没有这个属性） */
		KeyInterp Interp{ KeyInterp::Linear };
		bool      HasInterp{ false };
	};

	/* 一条轨道：目标 = 相对 Animator 实体的层级路径 + 组件名 + 字段名。
	 * 字段名和 ComponentRegistry 里的注册名一致，读写都走那张表（含访问器字段）。 */
	struct AnimationTrack
	{
		std::string    Path;       /* 空 = Animator 自己；"Arm/Hand" = 逐级子节点 */
		std::string    Component;  /* "Transform" / "Light" / "Camera" */
		std::string    Property;   /* "Position" / "Intensity" / "Fov" */
		TrackValueType ValueType{ TrackValueType::Vec3 };
		KeyInterp      Interp{ KeyInterp::Linear };
		/* 轨道开关：关掉的轨道不参与播放与录制（时间轴行首图标切换） */
		bool           Enabled{ true };
		std::vector<AnimationKey> Keys;
	};

	/* 动画片段资产（.anim）。只放数据与采样，播放与写值在 AnimationRuntime 里。 */
	class AnimationClip
	{
	public:
		AnimationClip() = default;
		~AnimationClip() = default;

		[[nodiscard]] const std::string& GetPath() const { return m_Path; }
		void SetPath(std::string path) { m_Path = std::move(path); }

		[[nodiscard]] float GetDuration() const { return m_Duration; }
		void SetDuration(float duration) { m_Duration = duration; }
		[[nodiscard]] bool IsLoop() const { return m_Loop; }
		void SetLoop(bool loop) { m_Loop = loop; }

		[[nodiscard]] const std::vector<AnimationTrack>& GetTracks() const { return m_Tracks; }
		std::vector<AnimationTrack>& GetTracks() { return m_Tracks; }
		void AddTrack(AnimationTrack track) { m_Tracks.emplace_back(std::move(track)); }

		/* 事件表（按时间升序）：播放器到点回调，编辑器在标尺上打点 */
		[[nodiscard]] const std::vector<AnimationEvent>& GetEvents() const { return m_Events; }
		std::vector<AnimationEvent>& GetEvents() { return m_Events; }
		/* 加一条事件并保持时间升序，返回它在新表里的下标 */
		size_t AddEvent(AnimationEvent event);
		bool RemoveEvent(size_t index);

		/* 内容版本号：编辑器改完轨道调 Touch()，播放器据此判断"定格要不要重新采样" */
		[[nodiscard]] uint32_t GetRevision() const { return m_Revision; }
		void Touch() { ++m_Revision; }

		/* 采样一条轨道；没有关键帧返回 false（值不动）。只读 track，故做成静态 */
		[[nodiscard]] static bool Sample(const AnimationTrack& track, float time, glm::vec4& out_value);

		/* 自动切线（相邻帧中心差分，单位是"值 / 秒"）；曲线编辑器画手柄也用它 */
		[[nodiscard]] static glm::vec4 ComputeAutoTangent(const std::vector<AnimationKey>& keys, size_t index);

		/* 关键帧列表变化后重算时长：取所有轨道最后一帧的最大时间（外部显式设置的更长的值保留） */
		void RefreshDuration();

		void Serializer(const std::string& path) const;
		bool Deserializer(const std::string& path);

		static SharedPtr<AnimationClip> Create(const std::string& path);

	private:
		std::string m_Path{};
		float       m_Duration{ 0.0f };
		bool        m_Loop{ true };
		std::vector<AnimationTrack> m_Tracks{};
		std::vector<AnimationEvent> m_Events{};
		uint32_t m_Revision{ 0 };
	};
}
