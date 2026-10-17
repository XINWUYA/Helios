#include "Pch.h"
#include "AnimationClip.h"
#include "SceneCommon.h"
#include <tinyxml2.h>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "Helios/Common/Utils.h"

namespace Helios
{
	namespace
	{
		constexpr const char* kValueTypeNames[] = { "Float", "Vec3", "Vec4", "Quat" };
		constexpr const char* kInterpNames[] = { "Step", "Linear", "Cubic" };

		/* 每种值类型占几个分量；写 XML 与读 XML 都按这个数 */
		int ComponentCount(TrackValueType type)
		{
			switch (type)
			{
			case TrackValueType::Float: return 1;
			case TrackValueType::Vec3:  return 3;
			case TrackValueType::Vec4:  return 4;
			case TrackValueType::Quat:  return 4;
			}
			return 4;
		}

		TrackValueType ParseValueType(const char* text)
		{
			if (text != nullptr)
			{
				for (size_t i = 0; i < sizeof(kValueTypeNames) / sizeof(kValueTypeNames[0]); ++i)
				{
					if (std::strcmp(text, kValueTypeNames[i]) == 0)
						return static_cast<TrackValueType>(i);
				}
			}
			return TrackValueType::Vec3;
		}

		KeyInterp ParseInterp(const char* text)
		{
			if (text != nullptr)
			{
				for (size_t i = 0; i < sizeof(kInterpNames) / sizeof(kInterpNames[0]); ++i)
				{
					if (std::strcmp(text, kInterpNames[i]) == 0)
						return static_cast<KeyInterp>(i);
				}
			}
			return KeyInterp::Linear;
		}

		glm::vec4 ParseValue(const char* text, int component_count)
		{
			glm::vec4 value(0.0f);
			if (text == nullptr || *text == '\0')
				return value;

			/* 按类型只读实际分量数：Float 只有一列，别拿 vec4 的口径硬拆 */
			const std::vector<std::string> tokens = Split(text, " ");
			const int count = std::min(component_count, static_cast<int>(tokens.size()));
			for (int index = 0; index < count; ++index)
				value[index] = std::strtof(tokens[index].c_str(), nullptr);
			return value;
		}

		std::string FormatValue(const glm::vec4& value, int component_count)
		{
			switch (component_count)
			{
			case 1: return ToString(value.x);
			case 3: return ToString(glm::vec3(value));
			default: return ToString(value);
			}
		}

		bool IsQuat(const AnimationTrack& track)
		{
			return track.ValueType == TrackValueType::Quat;
		}

		/* 四元数邻近半球对齐：slerp 与切线都要求两个四元数同向 */
		glm::vec4 AlignQuat(const glm::vec4& reference, const glm::vec4& value)
		{
			const float dot = reference.x * value.x + reference.y * value.y
				+ reference.z * value.z + reference.w * value.w;
			return (dot < 0.0f) ? glm::vec4(-value.x, -value.y, -value.z, -value.w) : value;
		}

		glm::vec4 InterpolateLinear(const AnimationTrack& track, const glm::vec4& from, const glm::vec4& to, float t)
		{
			if (!IsQuat(track))
				return from + (to - from) * t;

			/* 按 (x, y, z, w) 存取，转成 glm 的 (w, x, y, z) 再 slerp */
			const glm::quat q0(from.w, from.x, from.y, from.z);
			const glm::quat q1(to.w, to.x, to.y, to.z);
			const glm::quat result = glm::slerp(q0, q1, t);
			return glm::vec4(result.x, result.y, result.z, result.w);
		}

		float Hermite(float p0, float p1, float m0, float m1, float t)
		{
			const float t2 = t * t;
			const float t3 = t2 * t;
			return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0
				+ (t3 - 2.0f * t2 + t) * m0
				+ (-2.0f * t3 + 3.0f * t2) * p1
				+ (t3 - t2) * m1;
		}

		/* 自动切线（Catmull-Rom 斜率，单位是"值 / 秒"）：缺切线时用它顶上。
		 * 四元数的 q 与 -q 是同一个旋转，切线必须表达在**当前关键帧**的半球里：
		 * 两侧邻居都先对齐到它。只对齐邻居之间是不够的 —— 邻居和当前帧不同半球时
		 * 算出来的斜率是反的，贝塞尔段会来回抖（旋转角度在中途倒退再前进）。 */
		glm::vec4 AutoTangentImpl(const std::vector<AnimationKey>& keys, size_t index)
		{
			const size_t last = keys.size() - 1;
			const AnimationKey& previous = keys[index > 0 ? index - 1 : index];
			const AnimationKey& next = keys[index < last ? index + 1 : index];
			const float span = next.Time - previous.Time;
			if (span <= 1e-6f)
				return glm::vec4(0.0f);

			const glm::vec4& reference = keys[index].Value;
			const glm::vec4 previous_value = AlignQuat(reference, previous.Value);
			const glm::vec4 next_value = AlignQuat(reference, next.Value);
			return (next_value - previous_value) / span;
		}

		glm::quat QuatOf(const glm::vec4& value)
		{
			return glm::quat(value.w, value.x, value.y, value.z);
		}

		glm::vec4 Vec4Of(const glm::quat& value)
		{
			return glm::vec4(value.x, value.y, value.z, value.w);
		}

		/* 半球对齐：slerp 与控制点都要求两个四元数同向 */
		glm::quat AlignTo(const glm::quat& reference, const glm::quat& value)
		{
			const float dot = reference.x * value.x + reference.y * value.y
				+ reference.z * value.z + reference.w * value.w;
			return (dot < 0.0f) ? glm::quat(-value.w, -value.x, -value.y, -value.z) : value;
		}

		glm::quat QuatLog(const glm::quat& q)
		{
			const glm::vec3 v(q.x, q.y, q.z);
			const float length = glm::length(v);
			if (length < 1e-6f)
				return glm::quat(0.0f, 0.0f, 0.0f, 0.0f);

			const float angle = std::atan2(length, q.w);
			return glm::quat(0.0f, v * (angle / length));
		}

		glm::quat QuatExp(const glm::quat& q)
		{
			const glm::vec3 v(q.x, q.y, q.z);
			const float angle = glm::length(v);
			if (angle < 1e-6f)
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

			const float scale = std::sin(angle) / angle;
			return glm::quat(std::cos(angle), v * scale);
		}

		/* squad 的控制点：b_i = q_i * exp(-0.25*(log(q_i^-1 q_{i-1}) + log(q_i^-1 q_{i+1})))，
		 * 邻居先对齐到 q_i 的半球；端点上只有一侧，用自己顶替 */
		glm::quat SquadControl(const std::vector<AnimationKey>& keys, size_t index, const glm::quat& value)
		{
			const glm::quat q = value;
			const glm::quat previous = (index > 0)
				? AlignTo(q, QuatOf(keys[index - 1].Value)) : q;
			const glm::quat next = (index + 1 < keys.size())
				? AlignTo(q, QuatOf(keys[index + 1].Value)) : q;

			const glm::quat inverse = glm::conjugate(q);
			const glm::quat log_previous = QuatLog(inverse * previous);
			const glm::quat log_next = QuatLog(inverse * next);
			const glm::quat average(0.0f,
				(log_previous.x + log_next.x) * -0.25f,
				(log_previous.y + log_next.y) * -0.25f,
				(log_previous.z + log_next.z) * -0.25f);
			return q * QuatExp(average);
		}

		/* 四元数的三次插值用 squad：两层 slerp 夹一条平滑路径，角速度比"分量 Hermite 再归一化"匀 */
		glm::vec4 InterpolateSquad(const std::vector<AnimationKey>& keys, size_t from_index,
			const AnimationKey& from, const AnimationKey& to, float t)
		{
			const glm::quat q0 = QuatOf(from.Value);
			const glm::quat q1 = AlignTo(q0, QuatOf(to.Value));
			const glm::quat c0 = SquadControl(keys, from_index, q0);
			const glm::quat c1 = SquadControl(keys, from_index + 1, q1);

			const glm::quat along = glm::slerp(q0, q1, t);
			const glm::quat across = glm::slerp(c0, c1, t);
			const float blend = 2.0f * t * (1.0f - t);
			return Vec4Of(glm::normalize(glm::slerp(along, across, blend)));
		}

		glm::vec4 InterpolateCubic(const AnimationTrack& track, const std::vector<AnimationKey>& keys,
			size_t from_index, const AnimationKey& from, const AnimationKey& to, float t)
		{
			/* 自动切线的四元数段走 squad；手拖过切线（显式切线）的仍按分量 Hermite */
			if (IsQuat(track) && !from.HasTangent && !to.HasTangent)
				return InterpolateSquad(keys, from_index, from, to, t);

			const float span = to.Time - from.Time;
			glm::vec4 tangent_from = from.OutTangent;
			glm::vec4 tangent_to = to.InTangent;
			if (!from.HasTangent)
				tangent_from = AutoTangentImpl(keys, from_index);
			if (!to.HasTangent)
				tangent_to = AutoTangentImpl(keys, from_index + 1);

			/* 旋转先把两端对齐到同一半球，再逐分量插值，避免绕远路 */
			glm::vec4 to_value = to.Value;
			if (IsQuat(track))
			{
				const float dot = from.Value.x * to.Value.x + from.Value.y * to.Value.y
					+ from.Value.z * to.Value.z + from.Value.w * to.Value.w;
				if (dot < 0.0f)
				{
					to_value = -to.Value;
					tangent_to = -tangent_to;
				}
			}

			glm::vec4 value;
			for (int component = 0; component < 4; ++component)
			{
				value[component] = Hermite(from.Value[component], to_value[component],
					tangent_from[component] * span, tangent_to[component] * span, t);
			}

			if (IsQuat(track))
			{
				const glm::quat result(value.w, value.x, value.y, value.z);
				const glm::quat normalized = glm::normalize(result);
				return glm::vec4(normalized.x, normalized.y, normalized.z, normalized.w);
			}
			return value;
		}
	}

	glm::vec4 AnimationClip::ComputeAutoTangent(const std::vector<AnimationKey>& keys, size_t index)
	{
		return AutoTangentImpl(keys, index);
	}

	bool AnimationClip::Sample(const AnimationTrack& track, float time, glm::vec4& out_value)
	{
		const std::vector<AnimationKey>& keys = track.Keys;
		if (keys.empty())
			return false;

		if (keys.size() == 1 || time <= keys.front().Time)
		{
			out_value = keys.front().Value;
			return true;
		}
		if (time >= keys.back().Time)
		{
			out_value = keys.back().Value;
			return true;
		}

		/* 找段用二分：曲线视图一帧要采几百个点，线性扫关键帧会直接拖慢 */
		const auto segment = std::upper_bound(keys.begin(), keys.end(), time,
			[](float value, const AnimationKey& key) { return value < key.Time; });
		size_t next_index = static_cast<size_t>(segment - keys.begin());
		next_index = std::min(std::max<size_t>(next_index, 1), keys.size() - 1);

		const size_t from_index = next_index - 1;
		const AnimationKey& from = keys[from_index];
		const AnimationKey& to = keys[next_index];
		const float span = to.Time - from.Time;
		const float t = (span > 1e-6f) ? (time - from.Time) / span : 0.0f;

		/* 段的插值方式取"出发帧"的设置，没设置就跟轨道 */
		const KeyInterp segment_interp = from.HasInterp ? from.Interp : track.Interp;
		switch (segment_interp)
		{
		case KeyInterp::Step:
			out_value = from.Value;
			return true;
		case KeyInterp::Linear:
			out_value = InterpolateLinear(track, from.Value, AlignQuat(from.Value, to.Value), t);
			return true;
		case KeyInterp::Cubic:
			out_value = InterpolateCubic(track, keys, from_index, from, to, t);
			return true;
		}
		return false;
	}

	size_t AnimationClip::AddEvent(AnimationEvent event)
	{
		/* 时间升序：同时间的新事件排在已有事件后面，下标才稳定 */
		const auto position = std::upper_bound(m_Events.begin(), m_Events.end(), event.Time,
			[](float time, const AnimationEvent& existing) { return time < existing.Time; });
		const size_t index = static_cast<size_t>(position - m_Events.begin());
		m_Events.insert(position, std::move(event));
		return index;
	}

	bool AnimationClip::RemoveEvent(size_t index)
	{
		if (index >= m_Events.size())
			return false;
		m_Events.erase(m_Events.begin() + static_cast<std::ptrdiff_t>(index));
		return true;
	}

	void AnimationClip::RefreshDuration()
	{
		float last_time = 0.0f;
		for (const AnimationTrack& track : m_Tracks)
		{
			if (!track.Keys.empty())
				last_time = std::max(last_time, track.Keys.back().Time);
		}
		m_Duration = std::max(m_Duration, last_time);
	}

	void AnimationClip::Serializer(const std::string& path) const
	{
		std::filesystem::path clip_path;
		if (!TryPathFromUtf8(path, clip_path))
		{
			CORE_LOG_ERROR("Invalid UTF-8 animation path.");
			return;
		}

		auto clip_file = std::unique_ptr<FILE, decltype(&std::fclose)>(
			OpenUtf8File(clip_path, "wb"), &std::fclose);
		if (!clip_file)
		{
			CORE_LOG_ERROR("Failed to open animation file for writing: {}.", path);
			return;
		}

		auto* document = new tinyxml2::XMLDocument();
		document->InsertEndChild(document->NewDeclaration());
		auto* root = document->NewElement("Animation");
		document->InsertEndChild(root);

		root->SetAttribute("Duration", m_Duration);
		root->SetAttribute("Loop", m_Loop);

		for (const AnimationTrack& track : m_Tracks)
		{
			auto* track_element = root->InsertNewChildElement("Track");
			if (!track.Path.empty())
				track_element->SetAttribute("Path", track.Path.c_str());
			track_element->SetAttribute("Component", track.Component.c_str());
			track_element->SetAttribute("Property", track.Property.c_str());
			track_element->SetAttribute("Type", kValueTypeNames[static_cast<int>(track.ValueType)]);
			track_element->SetAttribute("Interp", kInterpNames[static_cast<int>(track.Interp)]);
			if (!track.Enabled)
				track_element->SetAttribute("Enabled", false);

			const int component_count = ComponentCount(track.ValueType);
			for (const AnimationKey& key : track.Keys)
			{
				auto* key_element = track_element->InsertNewChildElement("Key");
				key_element->SetAttribute("Time", key.Time);
				key_element->SetAttribute("Value", FormatValue(key.Value, component_count).c_str());
				if (key.HasTangent)
				{
					key_element->SetAttribute("In", FormatValue(key.InTangent, component_count).c_str());
					key_element->SetAttribute("Out", FormatValue(key.OutTangent, component_count).c_str());
				}
				if (key.HasInterp)
					key_element->SetAttribute("Interp", kInterpNames[static_cast<int>(key.Interp)]);
			}
		}

		for (const AnimationEvent& event : m_Events)
		{
			auto* event_element = root->InsertNewChildElement("Event");
			event_element->SetAttribute("Time", event.Time);
			event_element->SetAttribute("Name", event.Name.c_str());
		}

		if (document->SaveFile(clip_file.get()) != tinyxml2::XML_SUCCESS)
			CORE_LOG_ERROR("Failed to write animation file: {}.", path);
		delete document;
	}

	bool AnimationClip::Deserializer(const std::string& path)
	{
		m_Path = ABSOLUTE_PATH(path);

		std::filesystem::path clip_path;
		if (!TryPathFromUtf8(m_Path, clip_path))
		{
			CORE_LOG_ERROR("Invalid UTF-8 animation path.");
			return false;
		}

		auto clip_file = std::unique_ptr<FILE, decltype(&std::fclose)>(
			OpenUtf8File(clip_path, "rb"), &std::fclose);
		if (!clip_file)
		{
			CORE_LOG_ERROR("Failed to open animation file: {}.", m_Path);
			return false;
		}

		tinyxml2::XMLDocument document;
		if (document.LoadFile(clip_file.get()) != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_ERROR("Failed to parse animation file: {}.", m_Path);
			return false;
		}

		const tinyxml2::XMLElement* root = document.FirstChildElement("Animation");
		if (root == nullptr)
		{
			CORE_LOG_ERROR("Missing <Animation> root in animation file: {}.", m_Path);
			return false;
		}

		m_Tracks.clear();
		m_Duration = root->FloatAttribute("Duration", 0.0f);
		m_Loop = root->BoolAttribute("Loop", true);

		m_Events.clear();
		for (const tinyxml2::XMLElement* event_element = root->FirstChildElement("Event");
			event_element != nullptr; event_element = event_element->NextSiblingElement("Event"))
		{
			AnimationEvent event;
			event.Time = event_element->FloatAttribute("Time", 0.0f);
			if (const char* name = event_element->Attribute("Name"); name != nullptr)
				event.Name = name;
			m_Events.emplace_back(std::move(event));
		}

		for (const tinyxml2::XMLElement* track_element = root->FirstChildElement("Track");
			track_element != nullptr; track_element = track_element->NextSiblingElement("Track"))
		{
			AnimationTrack track;
			track.Path = track_element->Attribute("Path") != nullptr ? track_element->Attribute("Path") : "";
			track.Component = track_element->Attribute("Component") != nullptr ? track_element->Attribute("Component") : "";
			track.Property = track_element->Attribute("Property") != nullptr ? track_element->Attribute("Property") : "";
			track.ValueType = ParseValueType(track_element->Attribute("Type"));
			track.Interp = ParseInterp(track_element->Attribute("Interp"));
			track.Enabled = track_element->BoolAttribute("Enabled", true);

			if (track.Component.empty() || track.Property.empty())
			{
				CORE_LOG_WARN("Skipped animation track without component or property: {}.", m_Path);
				continue;
			}

			const int component_count = ComponentCount(track.ValueType);
			for (const tinyxml2::XMLElement* key_element = track_element->FirstChildElement("Key");
				key_element != nullptr; key_element = key_element->NextSiblingElement("Key"))
			{
				AnimationKey key;
				key.Time = key_element->FloatAttribute("Time", 0.0f);
				key.Value = ParseValue(key_element->Attribute("Value"), component_count);
				if (const char* in_text = key_element->Attribute("In"); in_text != nullptr)
				{
					key.InTangent = ParseValue(in_text, component_count);
					key.HasTangent = true;
				}
				if (const char* out_text = key_element->Attribute("Out"); out_text != nullptr)
				{
					key.OutTangent = ParseValue(out_text, component_count);
					key.HasTangent = true;
				}
				if (const char* interp_text = key_element->Attribute("Interp"); interp_text != nullptr)
				{
					for (int index = 0; index < 3; ++index)
					{
						if (std::strcmp(interp_text, kInterpNames[index]) == 0)
						{
							key.Interp = static_cast<KeyInterp>(index);
							key.HasInterp = true;
							break;
						}
					}
				}
				track.Keys.emplace_back(key);
			}

			if (track.Keys.empty())
				continue;

			/* 时间乱序的文件也照读：排一次序，采样就不用假设输入有序 */
			std::sort(track.Keys.begin(), track.Keys.end(),
				[](const AnimationKey& lhs, const AnimationKey& rhs) { return lhs.Time < rhs.Time; });
			m_Tracks.emplace_back(std::move(track));
		}

		RefreshDuration();
		return true;
	}

	SharedPtr<AnimationClip> AnimationClip::Create(const std::string& path)
	{
		auto clip = CreateSharedPtr<AnimationClip>();
		if (!clip->Deserializer(path))
			return nullptr;
		return clip;
	}
}
