#pragma once
#include <string>
#include <vector>
#include "Helios/Command/Command.h"
#include "Helios/Common/Common.h"
#include "Helios/Scene/AnimationClip.h"

namespace Helios
{
	/* 动画片段的编辑命令：整份轨道表换入换出，改完立刻写回 .anim。
	 * 片段是资产不是场景内容，所以不算"场景改动"（撤销历史照进同一条栈）。 */
	class ClipEditCommand final : public ICommand
	{
	public:
		ClipEditCommand(SharedPtr<AnimationClip> clip,
			std::vector<AnimationTrack> before_tracks, std::vector<AnimationTrack> after_tracks,
			std::vector<AnimationEvent> before_events, std::vector<AnimationEvent> after_events,
			float before_duration, float after_duration, const char* label);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }
		[[nodiscard]] bool AffectsSceneDocument() const override { return false; }

	private:
		void Apply(const std::vector<AnimationTrack>& tracks, const std::vector<AnimationEvent>& events,
			float duration);

		SharedPtr<AnimationClip> m_Clip;
		std::vector<AnimationTrack> m_BeforeTracks;
		std::vector<AnimationTrack> m_AfterTracks;
		std::vector<AnimationEvent> m_BeforeEvents;
		std::vector<AnimationEvent> m_AfterEvents;
		float m_BeforeDuration{ 0.0f };
		float m_AfterDuration{ 0.0f };
		std::string m_Label;
	};
}
