#pragma once
#include <array>
#include <cmath>
#include <imgui.h>
#include "EditorUI/EditorTheme.h"
#include "Helios/Scene/TerrainField.h"

namespace Helios::TerrainBrushPreview
{
	enum class State { Hover, Painting, Paused };

	inline void Draw(ImDrawList* draw, const TerrainField& field, const glm::vec2& center,
		const glm::vec2& radius, const glm::mat4& view_projection, const ImVec2& viewport_min,
		const ImVec2& viewport_size, State state)
	{
		if (draw == nullptr || field.IsEmpty() || radius.x <= 0.0f || radius.y <= 0.0f
			|| viewport_size.x <= 0.0f || viewport_size.y <= 0.0f)
			return;

		const auto project = [&](const glm::vec2& local, ImVec2& screen)
		{
			float height = 0.0f;
			if (!field.SampleHeightLocal(local, height))
				return false;
			const glm::vec4 clip = view_projection * glm::vec4(field.ToWorld(local, height), 1.0f);
			if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z)
				|| !std::isfinite(clip.w) || clip.w <= 1e-4f || clip.z < 0.0f || clip.z > clip.w)
				return false;
			screen = ImVec2(viewport_min.x + (clip.x / clip.w * 0.5f + 0.5f) * viewport_size.x,
				viewport_min.y + (0.5f - clip.y / clip.w * 0.5f) * viewport_size.y);
			return std::isfinite(screen.x) && std::isfinite(screen.y);
		};

		constexpr int kSegments = 128;
		std::array<ImVec2, kSegments> ring{};
		std::array<bool, kSegments> valid{};
		int gap = -1;
		for (int index = 0; index < kSegments; ++index)
		{
			const float angle = 6.2831853f * static_cast<float>(index) / kSegments;
			valid[index] = project(center + glm::vec2(std::cos(angle), std::sin(angle)) * radius, ring[index]);
			if (!valid[index])
				gap = index;
		}

		const bool paused = state == State::Paused;
		const bool painting = state == State::Painting;
		const ImU32 backing = IM_COL32(15, 20, 28, paused ? 100 : 210);
		const ImU32 outline = painting ? ImGui::GetColorU32(EditorTheme::Token::AccentHover)
			: IM_COL32(242, 247, 252, paused ? 120 : 245);
		const float thickness = EditorTheme::Scaled(painting ? 2.0f : 1.5f);
		const auto stroke = [&](const ImVec2* points, int count, bool closed)
		{
			if (count < 2)
				return;
			const ImDrawFlags flags = closed ? ImDrawFlags_Closed : ImDrawFlags_None;
			draw->AddPolyline(points, count, backing, flags, thickness + EditorTheme::Scaled(2.0f));
			draw->AddPolyline(points, count, outline, flags, thickness);
		};

		draw->PushClipRect(viewport_min, ImVec2(viewport_min.x + viewport_size.x, viewport_min.y + viewport_size.y), true);
		if (gap < 0)
			stroke(ring.data(), kSegments, true);
		else
		{
			/* 从断点后起排，地形边缘与近平面两侧不跨空白连线。 */
			std::array<ImVec2, kSegments> arc{};
			int count = 0;
			for (int step = 1; step <= kSegments; ++step)
			{
				const int index = (gap + step) % kSegments;
				if (valid[index])
					arc[count++] = ring[index];
				else
				{
					stroke(arc.data(), count, false);
					count = 0;
				}
			}
		}

		ImVec2 hit;
		if (project(center, hit))
		{
			const float inner = EditorTheme::Scaled(4.0f);
			const float outer = EditorTheme::Scaled(8.0f);
			const ImVec2 marks[][2] = {
				{ ImVec2(hit.x - outer, hit.y), ImVec2(hit.x - inner, hit.y) },
				{ ImVec2(hit.x + inner, hit.y), ImVec2(hit.x + outer, hit.y) },
				{ ImVec2(hit.x, hit.y - outer), ImVec2(hit.x, hit.y - inner) },
				{ ImVec2(hit.x, hit.y + inner), ImVec2(hit.x, hit.y + outer) },
			};
			for (const auto& mark : marks)
				stroke(mark, 2, false);
			draw->AddCircleFilled(hit, EditorTheme::Scaled(2.5f), backing, 12);
			draw->AddCircleFilled(hit, EditorTheme::Scaled(1.25f), outline, 12);
		}
		draw->PopClipRect();
	}
}
