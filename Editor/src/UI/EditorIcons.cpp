#include "Pch.h"
#include "EditorIcons.h"
#include "Svg.h"
#include "UI/EditorTheme.h"
#include "Helios/Scene/SceneCommon.h"
#include <magic_enum.hpp>
#include <tinyxml2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace Helios::Icons
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		/* Token 颜色 → ImU32（alpha 由调用方给，图标要跟着禁用态一起变淡） */
		inline ImU32 InkColor(const ImVec4& color, uint8_t alpha)
		{
			return IM_COL32(static_cast<int>(color.x * 255.0f + 0.5f),
				static_cast<int>(color.y * 255.0f + 0.5f),
				static_cast<int>(color.z * 255.0f + 0.5f), alpha);
		}

		/* SVG 资产里写的十六进制只是颜色槽的标识（#655fd0 = 强调色槽、#27a88a = 副色槽），
		 * 实际落色由 SvgInkColor 决定：两个彩色槽都跟主题走，其余固定 —— 换主题不用动资产。 */
		enum class SvgInk : uint8_t
		{
			None,
			Navy,
			Accent,
			PaleAccent,
			Alt,
			PaleAlt,
			Red,
			/* 红色淡底：删除图标的桶身 */
			PaleRed,
			Gray,
			/* 挖空底：浅色画布上是纯白，深色画布上压到面板底色，用来遮住下面的线 */
			White,
		};

		struct SvgStyle
		{
			SvgInk Stroke = SvgInk::Navy;
			SvgInk Fill = SvgInk::None;
			float StrokeWidth = 1.8f;
			float DashOn = 0.0f;
			float DashOff = 0.0f;
			bool RoundCap = false;
			bool RoundJoin = false;
		};

		struct SvgShape
		{
			enum class Type : uint8_t { Path, Circle, Rect };
			Type Kind = Type::Path;
			SvgStyle Style;
			std::vector<Svg::Contour> Contours;
			float X = 0.0f;
			float Y = 0.0f;
			float Width = 0.0f;
			float Height = 0.0f;
			float Radius = 0.0f;
		};

		struct SvgIcon
		{
			std::vector<SvgShape> Shapes;
		};

		/* 两个彩色槽都跟主题走：深色画布用 Accent / AccentTint、Alt / AltTint，
		 * 亮底（强调色按钮上的图标）用 OnLight / Pale 那两对 */
		inline ImU32 SvgInkColor(SvgInk ink, uint8_t alpha, bool dark_canvas)
		{
			if (dark_canvas)
			{
				switch (ink)
				{
				case SvgInk::Navy:        return IM_COL32(211, 222, 241, alpha);
				case SvgInk::Accent:      return InkColor(EditorTheme::Token::Accent, alpha);
				case SvgInk::PaleAccent:  return InkColor(EditorTheme::Token::AccentTint, alpha);
				case SvgInk::Alt:         return InkColor(EditorTheme::Token::AccentAlt, alpha);
				case SvgInk::PaleAlt:     return InkColor(EditorTheme::Token::AccentAltTint, alpha);
				case SvgInk::Red:         return IM_COL32(255, 132, 126, alpha);
				case SvgInk::PaleRed:     return IM_COL32(71, 37, 35, alpha);
				case SvgInk::Gray:        return IM_COL32(174, 187, 208, alpha);
				case SvgInk::White:       return IM_COL32(29, 38, 49, alpha);
				default:                  return IM_COL32(0, 0, 0, 0);
				}
			}

			switch (ink)
			{
			case SvgInk::Navy:        return IM_COL32(41, 53, 80, alpha);   /* #293550 */
			case SvgInk::Accent:      return InkColor(EditorTheme::Token::AccentOnLight, alpha);
			case SvgInk::PaleAccent:  return InkColor(EditorTheme::Token::AccentPale, alpha);
			case SvgInk::Alt:         return InkColor(EditorTheme::Token::AccentAltDeep, alpha);
			case SvgInk::PaleAlt:     return InkColor(EditorTheme::Token::AccentAltPale, alpha);
			case SvgInk::Red:         return IM_COL32(213, 92, 85, alpha);  /* #D55C55 */
			case SvgInk::PaleRed:     return IM_COL32(252, 235, 238, alpha);/* #FCEBEE */
			case SvgInk::Gray:        return IM_COL32(170, 178, 193, alpha);/* #AAB2C1 */
			case SvgInk::White:       return IM_COL32(255, 255, 255, alpha);
			default:                  return IM_COL32(0, 0, 0, 0);
			}
		}

		inline SvgInk ParseSvgInk(const char* value, SvgInk fallback)
		{
			if (value == nullptr)
				return fallback;
			if (std::strcmp(value, "none") == 0)
				return SvgInk::None;
			if (std::strcmp(value, "#293550") == 0)
				return SvgInk::Navy;
			if (std::strcmp(value, "#655fd0") == 0)
				return SvgInk::Accent;
			if (std::strcmp(value, "#27a88a") == 0)
				return SvgInk::Alt;
			if (std::strcmp(value, "#e1f5ee") == 0)
				return SvgInk::PaleAlt;
			if (std::strcmp(value, "#eef0ff") == 0)
				return SvgInk::PaleAccent;
			if (std::strcmp(value, "#d55c55") == 0)
				return SvgInk::Red;
			if (std::strcmp(value, "#fcebee") == 0)
				return SvgInk::PaleRed;
			if (std::strcmp(value, "#aab2c1") == 0)
				return SvgInk::Gray;
			if (std::strcmp(value, "#fff") == 0 || std::strcmp(value, "#ffffff") == 0)
				return SvgInk::White;
			return fallback;
		}

		inline float ParseSvgNumber(const char* value, float fallback)
		{
			return value != nullptr ? std::strtof(value, nullptr) : fallback;
		}

		SvgStyle InheritSvgStyle(const tinyxml2::XMLElement* element, SvgStyle style)
		{
			style.Stroke = ParseSvgInk(element->Attribute("stroke"), style.Stroke);
			style.Fill = ParseSvgInk(element->Attribute("fill"), style.Fill);
			style.StrokeWidth = ParseSvgNumber(element->Attribute("stroke-width"), style.StrokeWidth);
			style.RoundCap = element->Attribute("stroke-linecap") != nullptr
				? std::strcmp(element->Attribute("stroke-linecap"), "round") == 0 : style.RoundCap;
			style.RoundJoin = element->Attribute("stroke-linejoin") != nullptr
				? std::strcmp(element->Attribute("stroke-linejoin"), "round") == 0 : style.RoundJoin;
			if (const char* dash = element->Attribute("stroke-dasharray"))
			{
				style.DashOn = std::strtof(dash, nullptr);
				const char* separator = dash;
				while (*separator != '\0' && *separator != ',' && *separator != ' ' && *separator != '\t')
					++separator;
				while (*separator == ',' || *separator == ' ' || *separator == '\t')
					++separator;
				style.DashOff = std::strtof(separator, nullptr);
			}
			return style;
		}


		void ParseSvgElement(const tinyxml2::XMLElement* element, const SvgStyle& inherited, SvgIcon& icon)
		{
			for (const tinyxml2::XMLElement* child = element->FirstChildElement(); child != nullptr; child = child->NextSiblingElement())
			{
				const char* tag = child->Name();
				const SvgStyle style = InheritSvgStyle(child, inherited);
				if (std::strcmp(tag, "g") == 0)
				{
					ParseSvgElement(child, style, icon);
					continue;
				}

				SvgShape shape;
				shape.Style = style;
				if (std::strcmp(tag, "path") == 0)
				{
					shape.Kind = SvgShape::Type::Path;
					const char* data = child->Attribute("d");
					if (data != nullptr)
						shape.Contours = Svg::ParsePath(data);
				}
				else if (std::strcmp(tag, "circle") == 0)
				{
					shape.Kind = SvgShape::Type::Circle;
					shape.X = child->FloatAttribute("cx");
					shape.Y = child->FloatAttribute("cy");
					shape.Radius = child->FloatAttribute("r");
				}
				else if (std::strcmp(tag, "rect") == 0)
				{
					shape.Kind = SvgShape::Type::Rect;
					shape.X = child->FloatAttribute("x");
					shape.Y = child->FloatAttribute("y");
					shape.Width = child->FloatAttribute("width");
					shape.Height = child->FloatAttribute("height");
					shape.Radius = child->FloatAttribute("rx");
				}
				else
					continue;
				icon.Shapes.push_back(std::move(shape));
			}
		}

		std::array<SvgIcon, static_cast<size_t>(Id::COUNT)> s_SvgIcons;
		std::array<float, static_cast<size_t>(Id::COUNT)> s_OpticalScales;
		std::once_flag s_SvgIconsInit;

		/* 按枚举名从 EditorRes/icons 目录逐个加载 SVG，光学尺寸读 SVG 根元素上的
		 * data-optical-scale；加载不到的图标用回退画法兜底。 */
		void InitializeSvgIcons()
		{
			std::fill(s_OpticalScales.begin(), s_OpticalScales.end(), 1.0f);

			const std::filesystem::path directory = ResolveAssetPath(PathFromUtf8("EditorRes/icons"));
			if (!std::filesystem::is_directory(directory))
			{
				CORE_LOG_ERROR("Editor icons: directory not found: {}", PathToUtf8(directory));
				return;
			}

			size_t loaded = 0;
			size_t failed = 0;
			for (size_t i = 1; i < static_cast<size_t>(Id::COUNT); ++i)
			{
				const std::string file_name = std::string(magic_enum::enum_name(static_cast<Id>(i))) + ".svg";
				const std::filesystem::path file = directory / PathFromUtf8(file_name);

				if (!std::filesystem::exists(file))
				{
					CORE_LOG_WARN("Editor icon missing: {}", file_name);
					++failed;
					continue;
				}

				tinyxml2::XMLDocument document;
				if (document.LoadFile(PathToUtf8(file).c_str()) != tinyxml2::XML_SUCCESS || document.RootElement() == nullptr)
				{
					CORE_LOG_WARN("Editor icon parse failed: {}", file_name);
					++failed;
					continue;
				}

				const tinyxml2::XMLElement* root = document.RootElement();
				if (const char* scale = root->Attribute("data-optical-scale"))
					s_OpticalScales[i] = std::strtof(scale, nullptr);
				ParseSvgElement(root, SvgStyle{}, s_SvgIcons[i]);
				++loaded;
			}

			if (failed > 0)
				CORE_LOG_WARN("Editor icons: {} loaded, {} missing or failed (EditorRes/icons)", loaded, failed);
			else
				CORE_LOG_INFO("Editor icons: {} loaded from EditorRes/icons", loaded);
		}

		float SvgStrokeWidth(const SvgStyle& style, float unit, bool dark_canvas)
		{
			const float scaled = style.StrokeWidth * unit * (dark_canvas ? 1.12f : 1.0f);
			return dark_canvas ? std::max(1.35f, scaled) : scaled;
		}

		float SvgDarkOpticalBoost(float size)
		{
			/* 阈值与区间按设计像素、经 Scaled 折算（size 是修复后的物理尺寸） */
			if (size <= EditorTheme::Scaled(16.0f))
				return 1.30f;
			if (size <= EditorTheme::Scaled(24.0f))
				return 1.30f - (size - EditorTheme::Scaled(16.0f)) * (0.16f / EditorTheme::Scaled(8.0f));
			if (size <= EditorTheme::Scaled(32.0f))
				return 1.14f - (size - EditorTheme::Scaled(24.0f)) * (0.14f / EditorTheme::Scaled(8.0f));
			return 1.0f;
		}

		void DrawSvgContour(ImDrawList* draw_list, const Svg::Contour& contour, const SvgStyle& style,
		                    const ImVec2& center, float unit, uint8_t alpha, bool dark_canvas)
		{
			if (contour.Points.size() < 2)
				return;
			std::vector<ImVec2> points;
			points.reserve(contour.Points.size());
			for (const ImVec2& point : contour.Points)
				points.emplace_back(center.x + (point.x - 16.0f) * unit, center.y + (point.y - 16.0f) * unit);

			/* 图标源的填充面常不带 Z：有填充就按闭合画（描边也跟着闭合） */
			const bool closed = contour.Closed || style.Fill != SvgInk::None;
			if (style.Fill != SvgInk::None)
				Svg::FillPolygon(draw_list, points, SvgInkColor(style.Fill, alpha, dark_canvas));

			if (style.Stroke != SvgInk::None && style.StrokeWidth > 0.0f)
				Svg::StrokePolyline(draw_list, points, closed,
					SvgInkColor(style.Stroke, alpha, dark_canvas), SvgStrokeWidth(style, unit, dark_canvas),
					style.DashOn * unit, style.DashOff * unit, style.RoundCap, style.RoundJoin);
		}

		void DrawSvgIcon(ImDrawList* draw_list, Id id, const ImVec2& center, float size, ImU32 input_color)
		{
			std::call_once(s_SvgIconsInit, InitializeSvgIcons);
			const size_t index = static_cast<size_t>(id);
			if (index >= s_SvgIcons.size())
				return;
			const uint8_t alpha = static_cast<uint8_t>((input_color >> IM_COL32_A_SHIFT) & 0xFF);
			const uint8_t red = static_cast<uint8_t>((input_color >> IM_COL32_R_SHIFT) & 0xFF);
			const uint8_t green = static_cast<uint8_t>((input_color >> IM_COL32_G_SHIFT) & 0xFF);
			const uint8_t blue = static_cast<uint8_t>((input_color >> IM_COL32_B_SHIFT) & 0xFF);
			const bool dark_canvas = red * 0.2126f + green * 0.7152f + blue * 0.0722f > 128.0f;
			const float rendered_size = size * (dark_canvas ? SvgDarkOpticalBoost(size) : 1.0f);
			const float unit = rendered_size / 32.0f;
			for (const SvgShape& shape : s_SvgIcons[index].Shapes)
			{
				if (shape.Kind == SvgShape::Type::Path)
				{
					for (const Svg::Contour& contour : shape.Contours)
						DrawSvgContour(draw_list, contour, shape.Style, center, unit, alpha, dark_canvas);
					continue;
				}
				const ImVec2 min(center.x + (shape.X - 16.0f) * unit, center.y + (shape.Y - 16.0f) * unit);
				if (shape.Kind == SvgShape::Type::Circle)
				{
					const float radius = shape.Radius * unit;
					if (shape.Style.Fill != SvgInk::None)
						draw_list->AddCircleFilled(min, radius, SvgInkColor(shape.Style.Fill, alpha, dark_canvas));
					if (shape.Style.Stroke != SvgInk::None)
					{
						const float stroke_width = SvgStrokeWidth(shape.Style, unit, dark_canvas);
						if (shape.Style.DashOn > 0.0f)
						{
							const float circumference = 2.0f * kPi * radius;
							const float dash = shape.Style.DashOn * unit;
							const float cycle = (shape.Style.DashOn + shape.Style.DashOff) * unit;
							for (float offset = 0.0f; offset < circumference; offset += cycle)
							{
								const float a0 = offset / radius;
								const float a1 = std::min(offset + dash, circumference) / radius;
								draw_list->PathArcTo(min, radius, a0, a1, 4);
								draw_list->PathStroke(SvgInkColor(shape.Style.Stroke, alpha, dark_canvas), 0, stroke_width);
							}
						}
						else
							draw_list->AddCircle(min, radius, SvgInkColor(shape.Style.Stroke, alpha, dark_canvas), 0,
								stroke_width);
					}
				}
				else
				{
					const ImVec2 max(min.x + shape.Width * unit, min.y + shape.Height * unit);
					if (shape.Style.Fill != SvgInk::None)
						draw_list->AddRectFilled(min, max, SvgInkColor(shape.Style.Fill, alpha, dark_canvas), shape.Radius * unit);
					if (shape.Style.Stroke != SvgInk::None)
						draw_list->AddRect(min, max, SvgInkColor(shape.Style.Stroke, alpha, dark_canvas), shape.Radius * unit,
							0, SvgStrokeWidth(shape.Style, unit, dark_canvas));
				}
			}
		}
	}

	bool HasSvg(Id id)
	{
		const auto index = static_cast<size_t>(id);
		if (index >= static_cast<size_t>(Id::COUNT))
			return false;

		std::call_once(s_SvgIconsInit, InitializeSvgIcons);
		return !s_SvgIcons[index].Shapes.empty();
	}

	void Draw(ImDrawList* draw_list, Id id, const ImVec2& center, float size, ImU32 color,
		bool apply_optical_scale)
	{
		if (draw_list == nullptr || size <= 0.0f)
			return;

		const auto index = static_cast<size_t>(id);
		if (index >= static_cast<size_t>(Id::COUNT))
			return;

		std::call_once(s_SvgIconsInit, InitializeSvgIcons);
		const float optical_size = apply_optical_scale ? size * s_OpticalScales[index] : size;
		/* 缺图（文件名写错或解析失败，启动时有警告）：画个方框占位，一眼能看出来 */
		if (s_SvgIcons[index].Shapes.empty())
		{
			const float half = optical_size * 0.35f;
			draw_list->AddRect(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
				color, 0.0f, 0, std::max(1.0f, optical_size * 0.06f));
			return;
		}

		DrawSvgIcon(draw_list, id, center, optical_size, color);
	}

	void BeginSearchInput()
	{
		BeginLeadingIcon();
	}

	void EndSearchInput()
	{
		/* 输入框的矩形当场可取（它的"当前窗口"就是输入框所在的窗口） */
		DrawLeadingIcon(Id::Search, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		EndLeadingIcon();
	}

	/* 前置图标（输入框 / 下拉框左端那个小图标）的摆法：图标中心距控件左端多远，
	 * 以及控件要为此额外留出的横向内边距（留宽一点，文字才不会贴着图标）。
	 * 搜索框与类型筛选共用这一组数 —— 两个控件的图标位置才对得齐。 */
	inline float kLeadingIconX() { return EditorTheme::Scaled(11.0f); }
	inline float kLeadingIconPad() { return EditorTheme::Scaled(20.0f); }

	inline float LeadingIconSize()
	{
		return std::min(ImGui::GetFontSize(), EditorTheme::Scaled(14.0f));
	}

	/* 预留图标占的横向空间（包住控件，之后用 Pop 还原）。
	 * 撑大的是 `FramePadding`，而 `BeginComboPopup` 会把弹层的横向内边距取成它 ——
	 * 带弹层的控件（下拉框）别用这一对，会连带把弹层撑开；输入框这类没有弹层的才用它。 */
	void BeginLeadingIcon()
	{
		const ImVec2 frame_padding = ImGui::GetStyle().FramePadding;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
			ImVec2(frame_padding.x + kLeadingIconPad(), frame_padding.y));
	}

	/* 把图标画在控件左端中部：用弱化色（跟搜索框的放大镜同一个色，都是"这东西是干嘛的"的提示）。
	 * 矩形由调用方给（不用 GetItemRect*）：下拉框弹层打开时当前窗口已经切到弹层了。 */
	void DrawLeadingIcon(Id id, const ImVec2& frame_min, const ImVec2& frame_max)
	{
		Draw(ImGui::GetWindowDrawList(), id,
			ImVec2(frame_min.x + kLeadingIconX(), (frame_min.y + frame_max.y) * 0.5f),
			LeadingIconSize(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
	}

	void EndLeadingIcon()
	{
		ImGui::PopStyleVar();
	}

	float LeadingIconSpace()
	{
		return kLeadingIconPad();
	}

	bool IconButton(Id id, const ImVec2& size, bool checked, const char* tooltip)
	{
		ImGui::PushID(static_cast<int>(id));

		/* 扁平无边框：默认近透明，悬停/按下由中性灰提供，checked 用强调色浸染。
		 * 主题给所有 framed 控件设了 1px 边框（输入框需要），图标按钮这里要去掉。
		 * 灰阶比主题的控件档各提一档：扁平按钮的底色是它唯一的状态反馈，控件档偏暗。 */
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Button,
			checked ? EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.34f) : EditorTheme::Token::Clear);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
			checked ? EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.46f)
			        : EditorTheme::WithAlpha(EditorTheme::Token::Neutral6, 0.85f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,
			checked ? EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.58f)
			        : EditorTheme::WithAlpha(EditorTheme::Token::Neutral7, 0.70f));

		const bool clicked = ImGui::Button("##icon", size);

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar();

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		const float icon_size = ImMin(size.x, size.y) * 0.66f;

		/* GetColorU32 会把 Style.Alpha 乘进去，禁用态（调用方压低 Alpha）自动变淡 */
		const ImU32 color = ImGui::GetColorU32(checked ? EditorTheme::Token::AccentHover : EditorTheme::Token::Text);

		Draw(ImGui::GetWindowDrawList(), id, center, icon_size, color);

		if (tooltip != nullptr && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);

		ImGui::PopID();
		return clicked;
	}

	int IconToggleGroup(const char* id, const Id* icons, const char* const* tooltips,
		int count, int selected, const ImVec2& size)
	{
		if (count <= 0 || icons == nullptr)
			return -1;

		const ImGuiStyle& style = ImGui::GetStyle();
		const float seg_width = size.x / static_cast<float>(count);
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 end(pos.x + size.x, pos.y + size.y);
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		/* 底槽：控件底色 + 一档边框，与属性面板的分段开关同一套画法 */
		draw_list->AddRectFilled(pos, end, ImGui::GetColorU32(EditorTheme::Token::Neutral4), style.FrameRounding);
		draw_list->AddRect(pos, end, ImGui::GetColorU32(EditorTheme::Token::Border),
			style.FrameRounding, 0, EditorTheme::Scaled(1.0f));

		/* 选中块：往里收一点，强调色淡填充 + 描边 */
		const float inset = EditorTheme::Scaled(2.0f);
		const float block_rounding = std::max(EditorTheme::Scaled(1.0f), style.FrameRounding - EditorTheme::Scaled(1.0f));
		if (selected >= 0 && selected < count)
		{
			const float block_x = pos.x + seg_width * static_cast<float>(selected);
			const ImVec2 block_min(block_x + inset, pos.y + inset);
			const ImVec2 block_max(block_x + seg_width - inset, pos.y + size.y - inset);
			draw_list->AddRectFilled(block_min, block_max,
				ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.17f)), block_rounding);
			draw_list->AddRect(block_min, block_max,
				ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.65f)),
				block_rounding, 0, EditorTheme::Scaled(1.0f));
		}

		int clicked = -1;
		ImGui::PushID(id);
		for (int index = 0; index < count; ++index)
		{
			ImGui::PushID(index);
			ImGui::InvisibleButton("##segment", ImVec2(seg_width, size.y));
			const bool hovered = ImGui::IsItemHovered();
			if (ImGui::IsItemClicked())
				clicked = index;
			if (hovered && tooltips != nullptr && tooltips[index] != nullptr)
				ImGui::SetTooltip("%s", tooltips[index]);
			ImGui::PopID();

			/* 未选中格的悬停提亮：只在没被选中块盖住的地方画 */
			const float seg_x = pos.x + seg_width * static_cast<float>(index);
			if (hovered && index != selected)
			{
				draw_list->AddRectFilled(ImVec2(seg_x, pos.y), ImVec2(seg_x + seg_width, pos.y + size.y),
					ImGui::GetColorU32(EditorTheme::Token::Neutral5), style.FrameRounding);
			}

			const float icon_size = std::min(seg_width, size.y) * 0.66f;
			const ImU32 color = ImGui::GetColorU32(index == selected || hovered
				? EditorTheme::Token::Text : EditorTheme::Token::TextDim);
			Draw(draw_list, icons[index],
				ImVec2(seg_x + seg_width * 0.5f, pos.y + size.y * 0.5f), icon_size, color);

			if (index + 1 < count)
				ImGui::SameLine(0.0f, 0.0f);
		}
		ImGui::PopID();

		return clicked;
	}
}
