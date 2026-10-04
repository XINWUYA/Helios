#include "Pch.h"
#include "EditorIcons.h"
#include "Helios/ImGui/EditorTheme.h"
#include <algorithm>
#include <cmath>

namespace Helios::Icons
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		/* 统一描边语言：所有矢量图标共用同一线宽比例，视觉重量一致 */
		inline float StrokeWidth(float size)
		{
			return std::max(1.0f, size * 0.088f);
		}

		/* 归一化坐标 -> 屏幕坐标。
		 * 约定：图标在 [0,1]² 内作图，(0,0) 为左上、(1,1) 为右下；
		 * 实际留白由调用方传入的 size 控制。 */
		struct Canvas
		{
			ImVec2 Center;
			float  Size;

			[[nodiscard]] ImVec2 At(float x, float y) const
			{
				return ImVec2(Center.x + (x - 0.5f) * Size, Center.y + (y - 0.5f) * Size);
			}
			[[nodiscard]] float Len(float v) const { return v * Size; }
		};

		/* 由一串归一化点连成折线并描边 */
		template <int N>
		void StrokePolyline(ImDrawList* draw_list, const Canvas& canvas, const float (&points)[N][2],
		                    float thickness, ImU32 color, bool closed)
		{
			for (int i = 0; i < N; ++i)
				draw_list->PathLineTo(canvas.At(points[i][0], points[i][1]));

			draw_list->PathStroke(color, closed ? ImDrawFlags_Closed : 0, thickness);
		}

		/* ---- 文件 ---- */

		void DrawNewScene(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 空白文档 + 右上折角 */
			static constexpr float kPage[5][2] = {
				{ 0.22f, 0.09f }, { 0.60f, 0.09f }, { 0.80f, 0.29f }, { 0.80f, 0.91f }, { 0.22f, 0.91f }
			};
			StrokePolyline(dl, c, kPage, t, color, true);

			dl->AddLine(c.At(0.60f, 0.09f), c.At(0.60f, 0.29f), color, t);
			dl->AddLine(c.At(0.60f, 0.29f), c.At(0.80f, 0.29f), color, t);
		}

		void DrawOpenScene(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			/* 文件夹 */
			static constexpr float kFolder[6][2] = {
				{ 0.10f, 0.85f }, { 0.10f, 0.19f }, { 0.39f, 0.19f },
				{ 0.49f, 0.34f }, { 0.90f, 0.34f }, { 0.90f, 0.85f }
			};
			StrokePolyline(dl, c, kFolder, StrokeWidth(size), color, true);
		}

		void DrawSave(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 盘体（左上切角）+ 上方滑盖 + 下方标签 */
			static constexpr float kShell[5][2] = {
				{ 0.15f, 0.15f }, { 0.68f, 0.15f }, { 0.85f, 0.32f }, { 0.85f, 0.85f }, { 0.15f, 0.85f }
			};
			StrokePolyline(dl, c, kShell, t, color, true);

			dl->AddRect(c.At(0.33f, 0.15f), c.At(0.67f, 0.40f), color, 0.0f, 0, t);
			dl->AddRect(c.At(0.28f, 0.55f), c.At(0.72f, 0.85f), color, 0.0f, 0, t);
		}

		/* ---- 编辑历史 ----
		 * 半圆弧 + 尾线 + 实心箭头；mirror 为真时左右镜像，得到「重做」 */
		void DrawHistoryArrow(ImDrawList* dl, const Canvas& c, float t, ImU32 color, bool mirror)
		{
			const float flip = mirror ? -1.0f : 1.0f;
			const auto at = [&](float x, float y) { return c.At(0.5f + flip * (x - 0.5f), y); };

			dl->PathArcTo(c.At(0.50f, 0.58f), c.Len(0.27f), kPi, 2.0f * kPi, 28);
			dl->PathLineTo(at(0.77f, 0.86f));
			dl->PathStroke(color, 0, t);

			dl->AddTriangleFilled(at(0.23f, 0.91f), at(0.10f, 0.58f), at(0.36f, 0.58f), color);
		}

		void DrawUndo(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			DrawHistoryArrow(dl, Canvas{ center, size }, StrokeWidth(size), color, false);
		}

		void DrawRedo(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			DrawHistoryArrow(dl, Canvas{ center, size }, StrokeWidth(size), color, true);
		}

		/* ---- 变换 ---- */

		void DrawTranslate(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddLine(c.At(0.50f, 0.26f), c.At(0.50f, 0.74f), color, t);
			dl->AddLine(c.At(0.26f, 0.50f), c.At(0.74f, 0.50f), color, t);

			dl->AddTriangleFilled(c.At(0.50f, 0.09f), c.At(0.39f, 0.28f), c.At(0.61f, 0.28f), color);
			dl->AddTriangleFilled(c.At(0.50f, 0.91f), c.At(0.39f, 0.72f), c.At(0.61f, 0.72f), color);
			dl->AddTriangleFilled(c.At(0.09f, 0.50f), c.At(0.28f, 0.39f), c.At(0.28f, 0.61f), color);
			dl->AddTriangleFilled(c.At(0.91f, 0.50f), c.At(0.72f, 0.39f), c.At(0.72f, 0.61f), color);
		}

		void DrawRotate(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const ImVec2 origin = c.At(0.50f, 0.53f);
			const float radius = c.Len(0.30f);
			/* 顶部留口，其余扫过一圈 */
			const float a0 = -0.30f * kPi;
			const float a1 = 1.28f * kPi;

			dl->PathArcTo(origin, radius, a0, a1, 40);
			dl->PathStroke(color, 0, StrokeWidth(size));

			/* 末端沿切向的箭头 */
			const ImVec2 dir(std::cos(a1), std::sin(a1));   /* 半径方向 */
			const ImVec2 end(origin.x + radius * dir.x, origin.y + radius * dir.y);
			const ImVec2 tangent(-dir.y, dir.x);            /* 切向（角度增大方向） */
			const float head = c.Len(0.17f);
			const float half = c.Len(0.115f);

			dl->AddTriangleFilled(
				ImVec2(end.x + tangent.x * head, end.y + tangent.y * head),
				ImVec2(end.x + dir.x * half, end.y + dir.y * half),
				ImVec2(end.x - dir.x * half, end.y - dir.y * half),
				color);
		}

		void DrawScale(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 由小到大的两个方框，用角点连线表达「缩放」 */
			dl->AddRect(c.At(0.10f, 0.64f), c.At(0.36f, 0.90f), color, 0.0f, 0, t);
			dl->AddRect(c.At(0.44f, 0.10f), c.At(0.90f, 0.56f), color, 0.0f, 0, t);
			dl->AddLine(c.At(0.36f, 0.64f), c.At(0.44f, 0.56f), color, t);
		}

		/* ---- 运行 ---- */

		void DrawPlay(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			/* 三角形的包围盒居中（略偏右一点，补偿实心三角的视觉重心） */
			dl->AddTriangleFilled(c.At(0.24f, 0.14f), c.At(0.24f, 0.86f), c.At(0.80f, 0.50f), color);
		}

		void DrawStop(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			dl->AddRectFilled(c.At(0.20f, 0.20f), c.At(0.80f, 0.80f), color, c.Len(0.16f));
		}

		/* ---- 通用 ---- */

		void DrawMenu(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			for (int i = 0; i < 3; ++i)
			{
				const float y = 0.28f + 0.22f * static_cast<float>(i);
				dl->AddLine(c.At(0.20f, y), c.At(0.80f, y), color, t);
			}
		}

		void DrawAdd(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddLine(c.At(0.50f, 0.16f), c.At(0.50f, 0.84f), color, t);
			dl->AddLine(c.At(0.16f, 0.50f), c.At(0.84f, 0.50f), color, t);
		}

		void DrawReturn(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			dl->AddLine(c.At(0.82f, 0.50f), c.At(0.30f, 0.50f), color, StrokeWidth(size));
			dl->AddTriangleFilled(c.At(0.09f, 0.50f), c.At(0.37f, 0.32f), c.At(0.37f, 0.68f), color);
		}

		void DrawFilter(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			static constexpr float kFunnel[7][2] = {
				{ 0.12f, 0.18f }, { 0.88f, 0.18f }, { 0.57f, 0.54f },
				{ 0.57f, 0.89f }, { 0.43f, 0.78f }, { 0.43f, 0.54f }, { 0.12f, 0.18f }
			};
			StrokePolyline(dl, c, kFunnel, StrokeWidth(size), color, false);
		}

		void DrawVisible(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->PathLineTo(c.At(0.08f, 0.50f));
			dl->PathBezierCubicCurveTo(c.At(0.30f, 0.16f), c.At(0.70f, 0.16f), c.At(0.92f, 0.50f), 24);
			dl->PathBezierCubicCurveTo(c.At(0.70f, 0.84f), c.At(0.30f, 0.84f), c.At(0.08f, 0.50f), 24);
			dl->PathStroke(color, 0, t);

			dl->AddCircle(c.At(0.50f, 0.50f), c.Len(0.145f), color, 0, t);
		}

		/* 元数据表：Id 与表项一一对应。PngPath 与 Draw 二选一。 */
		constexpr IconDesc s_Icons[] = {
			{ "None",         nullptr,                             nullptr },
			{ "NewScene",     nullptr,                             &DrawNewScene },
			{ "OpenScene",    nullptr,                             &DrawOpenScene },
			{ "Save",         nullptr,                             &DrawSave },
			{ "Undo",         nullptr,                             &DrawUndo },
			{ "Redo",         nullptr,                             &DrawRedo },
			{ "Translate",    nullptr,                             &DrawTranslate },
			{ "Rotate",       nullptr,                             &DrawRotate },
			{ "Scale",        nullptr,                             &DrawScale },
			{ "Play",         nullptr,                             &DrawPlay },
			{ "Stop",         nullptr,                             &DrawStop },
			{ "Menu",         nullptr,                             &DrawMenu },
			{ "Add",          nullptr,                             &DrawAdd },
			{ "Return",       nullptr,                             &DrawReturn },
			{ "Filter",       nullptr,                             &DrawFilter },
			{ "Visible",      nullptr,                             &DrawVisible },
			{ "Directory",    "EditorRes/icons/directory.png",     nullptr },
			{ "File",         "EditorRes/icons/file.png",          nullptr },
			{ "FileScene",    "EditorRes/icons/file_scn.png",      nullptr },
			{ "FileMtlGraph", "EditorRes/icons/file_mtlgraph.png", nullptr },
		};

		static_assert(sizeof(s_Icons) / sizeof(s_Icons[0]) == static_cast<size_t>(Id::COUNT),
			"Icons 元数据表与 Id 枚举数量不一致，请同步新增项");
	}

	const IconDesc& Get(Id id)
	{
		const auto index = static_cast<size_t>(id);
		return s_Icons[index < static_cast<size_t>(Id::COUNT) ? index : 0];
	}

	void Draw(ImDrawList* draw_list, Id id, const ImVec2& center, float size, ImU32 color)
	{
		if (draw_list == nullptr || size <= 0.0f)
			return;

		const auto index = static_cast<size_t>(id);
		if (index >= static_cast<size_t>(Id::COUNT))
			return;

		const IconDesc& desc = s_Icons[index];
		if (desc.Draw != nullptr)
			desc.Draw(draw_list, center, size, color);
	}

	std::shared_ptr<DeviceTexture> GetTexture(Id id)
	{
		/* 集中缓存：同一图标全局只加载一次 */
		static std::shared_ptr<DeviceTexture> s_Cache[static_cast<size_t>(Id::COUNT)];

		const auto index = static_cast<size_t>(id);
		if (index >= static_cast<size_t>(Id::COUNT))
			return nullptr;

		auto& slot = s_Cache[index];
		const char* png_path = s_Icons[index].PngPath;
		if (!slot && png_path != nullptr)
			slot = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(png_path));

		return slot;
	}

	bool IconButton(Id id, const ImVec2& size, bool checked, const char* tooltip)
	{
		ImGui::PushID(static_cast<int>(id));

		/* 扁平无边框：默认近透明，悬停/按下由中性灰提供，checked 用强调色浸染。
		 * 主题给所有 framed 控件设了 1px 边框（输入框需要），图标按钮这里要去掉。 */
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Button,
			checked ? EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.32f) : EditorTheme::Token::Clear);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
			checked ? EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.44f)
			        : EditorTheme::WithAlpha(EditorTheme::Token::Neutral5, 0.85f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,
			checked ? EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.58f) : EditorTheme::Token::Neutral6);

		const bool clicked = ImGui::Button("##icon", size);

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar();

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		const float icon_size = ImMin(size.x, size.y) * 0.62f;

		/* GetColorU32 会把 Style.Alpha 乘进去，禁用态（调用方压低 Alpha）自动变淡 */
		const ImU32 color = ImGui::GetColorU32(checked ? EditorTheme::Token::AccentHover : EditorTheme::Token::Text);

		const IconDesc& desc = Get(id);
		if (desc.Draw != nullptr)
		{
			Draw(ImGui::GetWindowDrawList(), id, center, icon_size, color);
		}
		else if (const auto texture = GetTexture(id))
		{
			/* 位图图标：贴图原点在左下，UV 上下翻转 */
			ImGui::GetWindowDrawList()->AddImage((ImTextureID)texture.get(),
				ImVec2(center.x - icon_size * 0.5f, center.y - icon_size * 0.5f),
				ImVec2(center.x + icon_size * 0.5f, center.y + icon_size * 0.5f),
				ImVec2(0, 1), ImVec2(1, 0), color);
		}

		if (tooltip != nullptr && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);

		ImGui::PopID();
		return clicked;
	}
}
