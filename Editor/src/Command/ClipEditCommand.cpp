#include "Pch.h"
#include "ClipEditCommand.h"

namespace Helios
{
	ClipEditCommand::ClipEditCommand(SharedPtr<AnimationClip> clip,
		std::vector<AnimationTrack> before_tracks, std::vector<AnimationTrack> after_tracks,
		std::vector<AnimationEvent> before_events, std::vector<AnimationEvent> after_events,
		float before_duration, float after_duration, const char* label)
		: m_Clip(std::move(clip)),
		  m_BeforeTracks(std::move(before_tracks)),
		  m_AfterTracks(std::move(after_tracks)),
		  m_BeforeEvents(std::move(before_events)),
		  m_AfterEvents(std::move(after_events)),
		  m_BeforeDuration(before_duration),
		  m_AfterDuration(after_duration),
		  m_Label(label != nullptr ? label : "Edit Animation")
	{
	}

	void ClipEditCommand::Do()
	{
		Apply(m_AfterTracks, m_AfterEvents, m_AfterDuration);
	}

	void ClipEditCommand::Undo()
	{
		Apply(m_BeforeTracks, m_BeforeEvents, m_BeforeDuration);
	}

	void ClipEditCommand::Apply(const std::vector<AnimationTrack>& tracks,
		const std::vector<AnimationEvent>& events, float duration)
	{
		if (m_Clip == nullptr)
			return;

		m_Clip->GetTracks() = tracks;
		m_Clip->GetEvents() = events;
		m_Clip->SetDuration(duration);
		m_Clip->Touch();

		/* 落盘以便下次打开仍在；内存里的对象地址不变，正在播的动画下一帧就能采到新数据 */
		if (!m_Clip->GetPath().empty())
			m_Clip->Serializer(m_Clip->GetPath());
	}
}
