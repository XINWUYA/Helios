#include "Pch.h"
#include "FrameGraphPanel.h"
#include "EditorIcons.h"
#include "Helios/ImGui/EditorTheme.h"
#include "PanelRegistry.h"
#include "Helios/Common/Utils.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include "Helios/Renderer/FrameGraph/RenderPassNode.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace Helios
{
	namespace
	{
		/* 布局常量（图空间单位；Pass 行布局：每个 Pass 占一行、行内横排它的输出资源） */
		constexpr float kLaneGap = 16.0f;          /* 行与行之间的纵向间隔 */
		constexpr float kLanePadding = 10.0f;      /* 行内容的内边距（Pass 卡与资源卡的纵向留白） */
		constexpr float kNodeGap = 12.0f;          /* 行内卡片之间的横向间隔 */
		constexpr float kTitleGap = 8.0f;          /* 标题带（名字独占的首行）→ 资源内容行的间隔 */
		constexpr float kPassContentMinHeight = 40.0f; /* 资源内容行最小高（无资源的行也有体量） */
		constexpr float kCanvasPadding = 30.0f;    /* Fit 时画布四周的留白 */
		/* 资源卡内快照缩略图的包围盒边长（图空间；按纹理宽高比内接）。
		 * 取 88：常见信息行（如 "64 x 64  RGBA8"）不会把卡撑得比图更宽，贴图
		 * 两侧的窄边距因此得以保持 */
		constexpr float kResourceImageBox = 88.0f;
		/* 快照图两侧的留白（图空间，每侧）：比标题文字的留白小 —— 图尽量充满卡 */
		constexpr float kImageSidePadding = 6.0f;
		/* 信息行（"宽 x 高  格式"）字号相对正文的比例：小一号让信息行不至于把卡撑宽，
		 * 贴图两侧因此更贴边（宽度计算与绘制两处必须同用这一个系数） */
		constexpr float kInfoTextScale = 0.85f;
		constexpr float kHoverImageBox = 200.0f;  /* 悬停放大图的包围盒边长（屏幕像素） */
		/* 节点类型图标的方形包围盒边长（图空间；屏幕上再随缩放 clamp ——
		 * Pass 的叠层菱形板与资源的节点卡共用同一条尺寸口径） */
		constexpr float kNodeIconBox = 10.0f;
		/* 跨行连线的左侧总线（rail，图空间）：Pass 卡左缘之外的纵向车道，连线的
		 * 下行段全部走这里 —— 中间不再穿过任何行 / 卡片（archify 式"总线"走线，
		 * 换乘点都在行与行之间的空档里） */
		constexpr float kRailInset = 14.0f;   /* Pass 卡左缘（x=0）→ 第一条总线 */
		constexpr float kRailSpacing = 14.0f; /* 总线之间的横向间距 */
		/* 缩放范围与拖拽判定阈值（屏幕像素） */
		constexpr float kMinZoom = 0.25f;
		constexpr float kMaxZoom = 2.5f;
		constexpr float kDragSlop = 4.0f;

		/* 按纹理宽高比把尺寸内接到边长 box 的正方框内 */
		ImVec2 FitBox(float box, float aspect)
		{
			if (aspect >= 1.0f)
				return ImVec2(box, box / aspect);
			return ImVec2(box * aspect, box);
		}

		/* ---- 渲染阶段标识色（按节点名关键字匹配）----
		 * 同一阶段的 Pass 和资源共用色相（Shadow / GBuffer / Lighting / Sky / Axis / Debug）：深色同相底 +
		 * 饱和彩色描边（描边才是类别的主要载体）；Pass 亮一档 / 资源暗一档。 */
		struct StageFill
		{
			const char* Keyword;   /* 节点名包含即命中（按注册顺序，先到先得） */
			const char* Display;   /* 悬停提示 / 图例里显示的名字 */
			ImVec4 Stroke;         /* 饱和标识色：节点描边（实线 / 虚线） */
			ImVec4 Fill;           /* 深色同相底：Pass 节点填充（资源节点 = 同色暗版） */
		};

		const StageFill kStageFills[] =
		{
			/*        关键字       显示名       描边（饱和标识色）                            填充（深色同相底） */
			{ "Shadow",   "Shadow",   ImVec4(155.0f / 255.0f, 138.0f / 255.0f, 224.0f / 255.0f, 1.0f),
			                          ImVec4(46.0f / 255.0f, 38.0f / 255.0f, 60.0f / 255.0f, 1.0f) }, /* 紫 */
			{ "GBuffer",  "GBuffer",  ImVec4(63.0f / 255.0f, 191.0f / 255.0f, 176.0f / 255.0f, 1.0f),
			                          ImVec4(30.0f / 255.0f, 54.0f / 255.0f, 58.0f / 255.0f, 1.0f) }, /* 青 */
			{ "Lighting", "Lighting", ImVec4(224.0f / 255.0f, 160.0f / 255.0f, 74.0f / 255.0f, 1.0f),
			                          ImVec4(58.0f / 255.0f, 46.0f / 255.0f, 38.0f / 255.0f, 1.0f) }, /* 琥珀 */
			{ "Sky",      "Sky",      ImVec4(91.0f / 255.0f, 141.0f / 255.0f, 239.0f / 255.0f, 1.0f),
			                          ImVec4(30.0f / 255.0f, 44.0f / 255.0f, 72.0f / 255.0f, 1.0f) }, /* 蓝 */
			{ "Axis",     "Axis",     ImVec4(111.0f / 255.0f, 191.0f / 255.0f, 95.0f / 255.0f, 1.0f),
			                          ImVec4(37.0f / 255.0f, 57.0f / 255.0f, 47.0f / 255.0f, 1.0f) }, /* 绿 */
			{ "Debug",    "Debug",    ImVec4(138.0f / 255.0f, 148.0f / 255.0f, 166.0f / 255.0f, 1.0f),
			                          ImVec4(52.0f / 255.0f, 49.0f / 255.0f, 54.0f / 255.0f, 1.0f) }, /* 中灰 */
		};

		/* 名字命中的阶段（无命中返回 nullptr = 节点用默认卡片 / 面板色与 Border 描边） */
		const StageFill* FindStageFill(const std::string& name)
		{
			for (const StageFill& stage : kStageFills)
			{
				if (name.find(stage.Keyword) != std::string::npos)
					return &stage;
			}
			return nullptr;
		}

		/* 资源节点的阶段色：同色相暗版（×0.8，与 CardBg → Neutral2 的层级比一致，
		 * 保持"动作亮 / 数据暗"） */
		ImVec4 ResourceFillOf(const ImVec4& pass_fill)
		{
			return ImVec4(pass_fill.x * 0.8f, pass_fill.y * 0.8f, pass_fill.z * 0.8f, 1.0f);
		}

		/* 描边提亮（悬停）：×1.35 截顶（阶段色描边不能像 Border 一样换成灰色，
		 * 否则悬停时丢掉阶段身份） */
		ImVec4 BrightenedStroke(const ImVec4& stroke)
		{
			return ImVec4(std::min(stroke.x * 1.35f, 1.0f), std::min(stroke.y * 1.35f, 1.0f),
				std::min(stroke.z * 1.35f, 1.0f), stroke.w);
		}

		/* 把文本按可用宽度截断（超出以省略号收尾）。悬停网格里的名字可能比单元宽，
		 * 不截断会把同一行的后续单元挤走。 */
		void DrawClippedCaption(const std::string& text, float width)
		{
			if (ImGui::CalcTextSize(text.c_str()).x <= width)
			{
				ImGui::TextUnformatted(text.c_str());
				return;
			}

			std::string clipped = text;
			while (!clipped.empty() && ImGui::CalcTextSize((clipped + "...").c_str()).x > width)
				clipped.pop_back();
			clipped += "...";
			ImGui::TextUnformatted(clipped.c_str());
		}

		/* 快照纹理的采样 UV：离屏纹理按引擎约定底行在前 → V 翻转显示（(0,1)/(1,0)）；
		 * 默认目标快照（TopDown）顶行在前 → 不翻转（(0,0)/(1,1)） */
		void SnapshotImageUVs(const FrameGraphCapture::Output& output, ImVec2& uv0, ImVec2& uv1)
		{
			uv0 = output.TopDown ? ImVec2(0.0f, 0.0f) : ImVec2(0.0f, 1.0f);
			uv1 = output.TopDown ? ImVec2(1.0f, 1.0f) : ImVec2(1.0f, 0.0f);
		}

		/* 精确坐标的线段（PathLineTo + PathStroke）。别用 AddLine：它给端点各加 (0.5, 0.5) 的对齐偏移
		 * （细线时代的手段）—— 粗线相对精确几何（折线 / 箭头 / 卡片）会整体偏半像素，看起来"箭头和线
		 * 不同心"。跟 AddOrthogonalPath / AddArrowHead 保持同一坐标口径。 */
		void AddExactLine(ImDrawList* draw_list, const ImVec2& from, const ImVec2& to,
			ImU32 color, float thickness)
		{
			draw_list->PathLineTo(from);
			draw_list->PathLineTo(to);
			draw_list->PathStroke(color, ImDrawFlags_None, thickness);
		}

		/* 虚线矩形：资源节点的描边语言（数据容器 = 直角 + 虚线；Pass = 圆角 + 实线）。
		 * 四边各自从角起笔，dash / gap 长度由调用方按缩放传（保持节奏一致）。 */
		void AddDashedRect(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max,
			ImU32 color, float thickness, float dash, float gap)
		{
			const ImVec2 corners[4] = { min, ImVec2(max.x, min.y), max, ImVec2(min.x, max.y) };
			const float step = dash + gap;

			for (int i = 0; i < 4; ++i)
			{
				const ImVec2 from = corners[i];
				const ImVec2 to = corners[(i + 1) % 4];

				const float delta_x = to.x - from.x;
				const float delta_y = to.y - from.y;
				const float length = std::sqrt(delta_x * delta_x + delta_y * delta_y);
				if (length <= 0.0f)
					continue;

				const float dir_x = delta_x / length;
				const float dir_y = delta_y / length;
				for (float offset = 0.0f; offset < length; offset += step)
				{
					const float dash_end = std::min(offset + dash, length);
					AddExactLine(draw_list,
						ImVec2(from.x + dir_x * offset, from.y + dir_y * offset),
						ImVec2(from.x + dir_x * dash_end, from.y + dir_y * dash_end),
						color, thickness);
				}
			}
		}

		/* 虚线线段（同名资源关联线用）：与资源卡描边同一套 dash / gap 节奏 ——
		 * 虚线在本面板的语言里 = "数据容器 / 同一资源的关联"（无箭头、中性色） */
		void AddDashedLine(ImDrawList* draw_list, const ImVec2& from, const ImVec2& to,
			ImU32 color, float thickness, float dash, float gap)
		{
			const float delta_x = to.x - from.x;
			const float delta_y = to.y - from.y;
			const float length = std::sqrt(delta_x * delta_x + delta_y * delta_y);
			if (length <= 0.0f)
				return;

			const float dir_x = delta_x / length;
			const float dir_y = delta_y / length;
			const float step = dash + gap;
			for (float offset = 0.0f; offset < length; offset += step)
			{
				const float dash_end = std::min(offset + dash, length);
				AddExactLine(draw_list,
					ImVec2(from.x + dir_x * offset, from.y + dir_y * offset),
					ImVec2(from.x + dir_x * dash_end, from.y + dir_y * dash_end),
					color, thickness);
			}
		}

		/* 直角折线：轴对齐的线段一笔画完，转弯处是纯直角（无倒角）。每个顶点补一枚半径 = 半线宽的
		 * 实心圆（等价 SVG 的 round linejoin / linecap）—— 拐角锐利、线头圆润，接缝没有豁口。 */
		void AddOrthogonalPath(ImDrawList* draw_list, const ImVec2* points, int point_count,
			ImU32 color, float thickness)
		{
			if (point_count < 2)
				return;

			draw_list->AddPolyline(points, point_count, color, ImDrawFlags_None, thickness);
			for (int i = 0; i < point_count; ++i)
				draw_list->AddCircleFilled(points[i], thickness * 0.5f, color, 8);
		}

		/* 箭头头部：实心三角、长宽比 10:7（按线宽缩放），尖端咬合在目标点上、底边垂直于行进方向。
		 * 尺寸由调用方按缩放给（太长会吃掉折线的转角段 —— 入口段的可用长度 = kRailInset）。 */
		void AddArrowHead(ImDrawList* draw_list, const ImVec2& tip,
			float dir_x, float dir_y, float length, ImU32 color)
		{
			const float half_width = length * 0.35f; /* 10 : 7 */
			const ImVec2 base(tip.x - dir_x * length, tip.y - dir_y * length);
			const ImVec2 normal(-dir_y, dir_x);
			const ImVec2 wing_a(base.x + normal.x * half_width, base.y + normal.y * half_width);
			const ImVec2 wing_b(base.x - normal.x * half_width, base.y - normal.y * half_width);
			draw_list->AddTriangleFilled(tip, wing_a, wing_b, color);
		}

		/* 箭头头部（开放式雪佛龙 —— 只给 Pass 图标内部的小箭头用：图标对齐的是
		 * 游戏引擎图标集的描边语言；依赖连线用 archify 同款实心三角）。
		 * dir = 行进方向（单位向量）、arm = 斜臂长（= 视觉半宽） */
		void AddChevronHead(ImDrawList* draw_list, const ImVec2& tip,
			float dir_x, float dir_y, float arm, float thickness, ImU32 color)
		{
			const ImVec2 wing_a(tip.x - dir_x * arm - dir_y * arm, tip.y - dir_y * arm + dir_x * arm);
			const ImVec2 wing_b(tip.x - dir_x * arm + dir_y * arm, tip.y - dir_y * arm - dir_x * arm);
			draw_list->AddLine(tip, wing_a, color, thickness);
			draw_list->AddLine(tip, wing_b, color, thickness);

			/* 圆头线帽（与折线同一手法）：三点各补一枚小圆 */
			const float cap = thickness * 0.5f;
			draw_list->AddCircleFilled(tip, cap, color, 8);
			draw_list->AddCircleFilled(wing_a, cap, color, 8);
			draw_list->AddCircleFilled(wing_b, cap, color, 8);
		}

		/* ---- 节点类型图标 ----
		 * Pass = 三层堆叠菱形板 + 向下箭头；资源 = 圆角卡片 + 端口 + 内部立方体。单色渲染、靠透明度
		 * 分层；小盒（< 12px）自动简化（Pass 减成两层板、资源去掉端口），低倍下也还认得出轮廓。 */

		void AddPassIconGlyph(ImDrawList* draw_list, const ImVec2& box_top_left, float box, ImU32 color)
		{
			const ImVec4 base = ImGui::ColorConvertU32ToFloat4(color);
			const auto tint = [&](float alpha)
			{ return ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, base.w * alpha)); };

			const float stroke = std::max(0.9f, box * 0.055f);

			/* 等距菱形板：中心 (cx, cy)、半宽 hw、半高 hh（先填后描） */
			const auto add_plate = [&](float cx, float cy, float hw, float hh, bool filled)
			{
				const ImVec2 points[4] = {
					ImVec2(cx, cy - hh), ImVec2(cx + hw, cy), ImVec2(cx, cy + hh), ImVec2(cx - hw, cy) };
				if (filled)
					draw_list->AddConvexPolyFilled(points, 4, tint(0.30f));
				draw_list->AddPolyline(points, 4, tint(0.95f), ImDrawFlags_Closed, stroke);
			};

			const bool full = box >= 12.0f;
			if (full)
			{
				/* 完整三层（原图比例：板宽 0.5×盒、三层等分 0.25×盒、板心 x = 0.375 盒） */
				const float hw = box * 0.25f;
				const float hh = box * 0.125f;
				const float cx = box_top_left.x + box * 0.375f;
				add_plate(cx, box_top_left.y + box * 0.1875f, hw, hh, true);
				add_plate(cx, box_top_left.y + box * 0.4688f, hw, hh, false);
				add_plate(cx, box_top_left.y + box * 0.75f, hw, hh, false);
			}
			else
			{
				/* 小盒：两层实心板（顶板亮、底板暗）—— 8px 下描边轮廓会糊成
				 * 一团，实心形状才读得出来；两层之间的空隙把"层叠"表达出来 */
				const auto add_solid_plate = [&](float cx, float cy, float hw, float hh, float alpha)
				{
					const ImVec2 points[4] = {
						ImVec2(cx, cy - hh), ImVec2(cx + hw, cy), ImVec2(cx, cy + hh), ImVec2(cx - hw, cy) };
					draw_list->AddConvexPolyFilled(points, 4, tint(alpha));
				};
				const float hw = box * 0.30f;
				const float hh = box * 0.20f;
				const float cx = box_top_left.x + box * 0.36f;
				add_solid_plate(cx, box_top_left.y + box * 0.25f, hw, hh, 0.95f);
				add_solid_plate(cx, box_top_left.y + box * 0.73f, hw, hh, 0.38f);
			}

			/* 右侧向下箭头（直线 + 雪佛龙头）：与依赖连线同一语言 */
			const float arrow_x = box_top_left.x + box * 0.86f;
			const float arrow_top = box_top_left.y + box * (full ? 0.14f : 0.20f);
			const float arrow_bottom = box_top_left.y + box * (full ? 0.68f : 0.74f);
			draw_list->AddLine(ImVec2(arrow_x, arrow_top), ImVec2(arrow_x, arrow_bottom),
				tint(0.95f), stroke);
			AddChevronHead(draw_list, ImVec2(arrow_x, arrow_bottom), 0.0f, 1.0f,
				std::max(1.3f, box * 0.13f), stroke, tint(0.95f));
		}

		void AddResourceIconGlyph(ImDrawList* draw_list, const ImVec2& box_top_left, float box, ImU32 color)
		{
			const ImVec4 base = ImGui::ColorConvertU32ToFloat4(color);
			const auto tint = [&](float alpha)
			{ return ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, base.w * alpha)); };

			const float stroke = std::max(0.9f, box * 0.055f);
			const bool full = box >= 12.0f;

			/* 圆角卡：淡底 + 描边 */
			const ImVec2 card_min(box_top_left.x + box * 0.16f, box_top_left.y + box * 0.18f);
			const ImVec2 card_max(box_top_left.x + box * 0.84f, box_top_left.y + box * 0.82f);
			draw_list->AddRectFilled(card_min, card_max, tint(0.10f), box * 0.085f);
			draw_list->AddRect(card_min, card_max, tint(0.95f), box * 0.085f, 0, stroke);

			if (full)
			{
				/* 连接端口：左×2（y = 0.375 / 0.625 盒）、右×1（y = 0.5 盒） */
				draw_list->AddLine(ImVec2(box_top_left.x + box * 0.03f, box_top_left.y + box * 0.375f),
					card_min, tint(0.95f), stroke);
				draw_list->AddLine(ImVec2(box_top_left.x + box * 0.03f, box_top_left.y + box * 0.625f),
					ImVec2(card_min.x, box_top_left.y + box * 0.625f), tint(0.95f), stroke);
				draw_list->AddLine(ImVec2(card_max.x, box_top_left.y + box * 0.5f),
					ImVec2(box_top_left.x + box * 0.97f, box_top_left.y + box * 0.5f), tint(0.95f), stroke);
			}

			/* 内部立方体（六边形宝石；full 变体再画三条可见棱） */
			const ImVec2 gem[6] = {
				ImVec2(box_top_left.x + box * 0.50f, box_top_left.y + box * 0.30f),
				ImVec2(box_top_left.x + box * 0.66f, box_top_left.y + box * 0.40f),
				ImVec2(box_top_left.x + box * 0.66f, box_top_left.y + box * 0.60f),
				ImVec2(box_top_left.x + box * 0.50f, box_top_left.y + box * 0.70f),
				ImVec2(box_top_left.x + box * 0.34f, box_top_left.y + box * 0.60f),
				ImVec2(box_top_left.x + box * 0.34f, box_top_left.y + box * 0.40f),
			};
			if (full)
			{
				draw_list->AddConvexPolyFilled(gem, 6, tint(0.28f));
				draw_list->AddPolyline(gem, 6, tint(0.95f), ImDrawFlags_Closed, stroke);
				/* 立方体的可见棱：顶 V（左上棱 / 右上棱）+ 前竖棱 */
				draw_list->AddLine(gem[5], ImVec2(box_top_left.x + box * 0.50f, box_top_left.y + box * 0.50f),
					tint(0.95f), stroke);
				draw_list->AddLine(ImVec2(box_top_left.x + box * 0.50f, box_top_left.y + box * 0.50f), gem[1],
					tint(0.95f), stroke);
				draw_list->AddLine(ImVec2(box_top_left.x + box * 0.50f, box_top_left.y + box * 0.50f), gem[3],
					tint(0.95f), stroke);
			}
			else
			{
				/* 小盒：宝石实心提亮、不描边（8px 下描边 + 填充会糊成一块；
				 * 实心小宝石 = 卡内的一枚亮点，"容器 + 载荷"的意象仍成立） */
				draw_list->AddConvexPolyFilled(gem, 6, tint(0.85f));
			}
		}
	}

	void FrameGraphPanel::OnImGuiRenderer(const std::vector<CameraEntry>& cameras, bool& visible)
	{
		/* 抓取请求默认清零：只有本帧真实画出来、且选中了相机的窗口才会重新置位 */
		m_CaptureRequested = false;

		/* 窗口隐藏时（View 菜单取消 / 标题栏关闭）就不提交 Begin —— 本仓 ImGui 的 p_open 只管
		 * "把关闭按钮写回 false"，继续提交的话已关闭的窗口照样显示；不提交就是隐藏（停靠 / 位置
		 * 状态都保留），恢复入口在 View 菜单。 */
		if (!visible)
		{
			m_LastNodeRects.clear();
			m_LastOutputRects.clear();
			m_LastLaneRects.clear();
			m_LastIconRects.clear();
			return;
		}

		/* 首次出现给一个适合看图的默认尺寸（拖过之后以 ini 记录为准） */
		ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_FirstUseEver);

		const bool drawn = ImGui::Begin(Panel::kFrameGraph, &visible);

		if (drawn)
		{
			/* 解析当前相机：按指针匹配（渲染图与相机同生命周期，跨帧稳定）；选中不在
			 * 本帧列表（相机被删除 / 首次打开）→ 回落到首个条目，并清掉旧图上的选中 */
			FrameGraph* active_graph = nullptr;
			for (const CameraEntry& camera : cameras)
			{
				if (camera.Graph != nullptr && camera.Graph == m_SelectedGraph)
				{
					active_graph = camera.Graph;
					break;
				}
			}
			if (active_graph == nullptr && !cameras.empty())
			{
				m_SelectedGraph = cameras.front().Graph;
				m_SelectedPassName.clear();
				RequestFit();
				active_graph = m_SelectedGraph;
			}

			m_CaptureRequested = !ImGui::GetCurrentWindow()->SkipItems && m_CaptureEnabled && active_graph != nullptr;

			/* 面板标题由页签承担（"Frame Graph"），面板内不重复标题行 */

			if (active_graph != nullptr)
			{
				/* 工具行（芯片组风格）：相机下拉 │ 抓取 / 剔除切换芯片、适配视图 │
				 * 右端搜索框 + 图规模统计。控件同高、组间一条细竖线分隔 ——
				 * 与面板的控件语言一致（不用原生复选框 / 小按钮混排） */
				const ImGuiStyle& toolbar_style = ImGui::GetStyle();
				const float toolbar_height = ImGui::GetFrameHeight();

				/* 切换芯片：自绘的圆角开关（左端状态圆点 + 文字）—— 激活 = 强调色
				 * 淡底 + 实心强调色圆点，未激活 = 控件底色 + 暗色圆点 */
				const auto toggle_chip = [&](const char* label, bool* value)
				{
					const ImVec2 label_size = ImGui::CalcTextSize(label);
					const float dot_radius = 3.0f;
					const float width = toolbar_style.FramePadding.x * 2.0f + dot_radius * 2.0f
						+ 6.0f + label_size.x;
					const ImVec2 pos = ImGui::GetCursorScreenPos();
					const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, toolbar_height));
					const bool chip_hovered = ImGui::IsItemHovered();
					const bool active = *value;

					ImDrawList* chip_draw_list = ImGui::GetWindowDrawList();
					chip_draw_list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + toolbar_height),
						ImGui::GetColorU32(active
							? EditorTheme::WithAlpha(EditorTheme::Token::Accent, chip_hovered ? 0.26f : 0.17f)
							: (chip_hovered ? EditorTheme::Token::Neutral5 : EditorTheme::Token::Neutral4)),
						toolbar_style.FrameRounding);
					chip_draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + toolbar_height),
						ImGui::GetColorU32(active
							? EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.65f)
							: EditorTheme::Token::Border),
						toolbar_style.FrameRounding, 0, 1.0f);

					const ImVec2 dot_center(pos.x + toolbar_style.FramePadding.x + dot_radius,
						pos.y + toolbar_height * 0.5f);
					chip_draw_list->AddCircleFilled(dot_center, dot_radius,
						ImGui::GetColorU32(active ? EditorTheme::Token::Accent
							: EditorTheme::WithAlpha(EditorTheme::Token::TextDim, 0.85f)));
					chip_draw_list->AddText(
						ImVec2(dot_center.x + dot_radius + 6.0f,
							pos.y + (toolbar_height - label_size.y) * 0.5f),
						ImGui::GetColorU32(active ? EditorTheme::Token::Text : EditorTheme::Token::TextDim),
						label);

					if (clicked)
						*value = !*value;
				};

				/* 图标动作芯片：Fit 这类"按一下"的动作 —— 外观与切换芯片同族
				 * （同高 / 同底 / 悬停提亮、1px 描边），内容换成矢量图标
				 * （编辑器统一图标集，单一数据源；悬停提示交代含义）。 */
				const auto action_icon_chip = [&](Icons::Id icon_id, const char* tooltip) -> bool
				{
					const float width = toolbar_height;
					const ImVec2 pos = ImGui::GetCursorScreenPos();
					ImGui::PushID(static_cast<int>(icon_id));
					const bool clicked = ImGui::InvisibleButton("##action_icon", ImVec2(width, toolbar_height));
					const bool chip_hovered = ImGui::IsItemHovered();
					ImGui::PopID();

					ImDrawList* chip_draw_list = ImGui::GetWindowDrawList();
					chip_draw_list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + toolbar_height),
						ImGui::GetColorU32(chip_hovered ? EditorTheme::Token::Neutral5
							: EditorTheme::Token::Neutral4), toolbar_style.FrameRounding);
					chip_draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + toolbar_height),
						ImGui::GetColorU32(EditorTheme::Token::Border), toolbar_style.FrameRounding, 0, 1.0f);
					Icons::Draw(chip_draw_list, icon_id,
						ImVec2(pos.x + width * 0.5f, pos.y + toolbar_height * 0.5f),
						toolbar_height * 0.64f,
						ImGui::GetColorU32(chip_hovered ? EditorTheme::Token::Text : EditorTheme::Token::TextLabel));
					if (tooltip != nullptr && chip_hovered)
						ImGui::SetTooltip("%s", tooltip);
					return clicked;
				};

				/* 组间分隔：一条 1px 细竖线（上下各留 3px） */
				const auto group_divider = [&]()
				{
					ImGui::SameLine(0.0f, 10.0f);
					const ImVec2 pos = ImGui::GetCursorScreenPos();
					ImGui::Dummy(ImVec2(1.0f, toolbar_height));
					ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + 3.0f),
						ImVec2(pos.x, pos.y + toolbar_height - 3.0f),
						ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Border, 0.8f)), 1.0f);
					ImGui::SameLine(0.0f, 10.0f);
				};

				/* 组 1：相机选择 */
				const char* selected_name = "(none)";
				for (const CameraEntry& camera : cameras)
				{
					if (camera.Graph == m_SelectedGraph)
					{
						selected_name = camera.Name.c_str();
						break;
					}
				}

				{
					const char* label = "Camera";
					const ImVec2 label_size = ImGui::CalcTextSize(label);
					ImGui::GetWindowDrawList()->AddText(
						ImVec2(ImGui::GetCursorScreenPos().x,
							ImGui::GetCursorScreenPos().y + (toolbar_height - label_size.y) * 0.5f),
						ImGui::GetColorU32(EditorTheme::Token::TextDim), label);
					ImGui::Dummy(ImVec2(label_size.x, toolbar_height));
				}
				ImGui::SameLine(0.0f, 6.0f);

				ImGui::SetNextItemWidth(170.0f);
				if (ImGui::BeginCombo("##FrameGraphCamera", selected_name))
				{
					for (size_t i = 0; i < cameras.size(); ++i)
					{
						if (cameras[i].Graph == nullptr)
							continue;

						ImGui::PushID(static_cast<int>(i));
						const bool selected = (cameras[i].Graph == m_SelectedGraph);
						if (ImGui::Selectable(cameras[i].Name.c_str(), selected) && !selected)
						{
							/* 换相机 = 换整张图：清掉旧图上的聚焦 / 选中的卡并重新适配视图 */
							m_SelectedGraph = cameras[i].Graph;
							m_SelectedPassName.clear();
							m_Focus = FocusRef{};
							RequestFit();
						}
						if (selected)
							ImGui::SetItemDefaultFocus();
						ImGui::PopID();
					}
					ImGui::EndCombo();
				}

				group_divider();

				/* 组 2：抓取开关 / 剔除显示 / 适配视图 */
				toggle_chip("Capture", &m_CaptureEnabled);
				ImGui::SameLine(0.0f, 6.0f);
				toggle_chip("Show Culled", &m_ShowCulled);
				ImGui::SameLine(0.0f, 6.0f);
				if (action_icon_chip(Icons::Id::FitView, "Fit to view"))
					RequestFit();

				size_t pass_count = 0;
				size_t resource_count = 0;
				for (const auto& node : active_graph->GetDependencyGraph().GetNodes())
				{
					if (!m_ShowCulled && !node->IsValid())
						continue;

					if (DynamicPtrCast<RenderPassNode>(node) != nullptr)
						++pass_count;
					else
						++resource_count;
				}

				/* 右端：搜索框（放大镜前置）+ 图规模统计，二者作为一个块贴行右缘 */
				const std::string stats_text = std::to_string(pass_count) + (pass_count == 1 ? " pass \xc2\xb7 " : " passes \xc2\xb7 ")
					+ std::to_string(resource_count) + (resource_count == 1 ? " resource" : " resources")
					+ (m_CaptureEnabled ? "" : " \xc2\xb7 capture paused");
				const float stats_width = ImGui::CalcTextSize(stats_text.c_str()).x;
				const float left_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
				const float content_max = ImGui::GetContentRegionMax().x;
				/* 搜索框宽度自适应：优先 170，行内空间不足时压缩到 90（统计不被挤出面板） */
				const float search_width = std::clamp(
					content_max - left_end - toolbar_style.ItemSpacing.x * 2.0f - stats_width,
					90.0f, 170.0f);
				const float right_width = search_width + toolbar_style.ItemSpacing.x + stats_width;
				const float right_start = std::max(left_end + toolbar_style.ItemSpacing.x,
					content_max - right_width);
				ImGui::SameLine(right_start);

				Icons::BeginSearchInput();
				ImGui::SetNextItemWidth(search_width);
				if (ImGui::InputTextWithHint("##FrameGraphFind", "Find pass / resource",
					m_SearchBuffer, sizeof(m_SearchBuffer)))
					m_SearchFilter = m_SearchBuffer;
				Icons::EndSearchInput();

				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(EditorTheme::Token::TextDim, "%s", stats_text.c_str());

				ImGui::Dummy(ImVec2(0.0f, 2.0f));

				/* 依赖图占满剩余高度 */
				DrawGraphCanvas(*active_graph, ImGui::GetContentRegionAvail().y);
			}
			else
			{
				/* 没有可选相机（理论少见）：给一行提示，不画图 */
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextUnformatted("No camera to visualize.");
				ImGui::PopStyleColor();
				m_LastNodeRects.clear();
				m_LastOutputRects.clear();
				m_LastLaneRects.clear();
				m_LastIconRects.clear();
			}
		}
		else
		{
			/* 窗口不可见：不画内容，但节点矩形要清掉（自检读到的是上一帧真画过的布局） */
			m_LastNodeRects.clear();
			m_LastOutputRects.clear();
			m_LastLaneRects.clear();
			m_LastIconRects.clear();
		}

		ImGui::End();
	}

	/* 阶段标识色查询（自检 / 自动化验证用）：名字命中阶段关键字时返回该阶段的
	 * 饱和标识色（节点描边用的那个）；无命中返回 nullptr（节点用默认卡片 /
	 * 面板色与 Border 描边） */
	const ImVec4* FrameGraphPanel::GetStageColor(const std::string& node_name)
	{
		const StageFill* stage = FindStageFill(node_name);
		return stage != nullptr ? &stage->Stroke : nullptr;
	}

	/* 搜索过滤（程序化入口）：同步驻留缓冲，UI 输入框下一帧显示同一文字 */
	void FrameGraphPanel::SetSearchFilter(const std::string& filter)
	{
		m_SearchFilter = filter;
		std::snprintf(m_SearchBuffer, sizeof(m_SearchBuffer), "%s", filter.c_str());
	}

	/* 依赖图画布：Pass 行布局 —— 每个 Pass 一行（行头 = 整行背景区域，名字独占首行；输出资源卡
	 * 叠在区域内，缩略图 = 执行完当刻的快照）。Pass 和输出之间不画线（包含即语义）；资源只保留
	 * 跨 Pass 的连线：相邻行直连、其余绕左侧总线。同名资源副本跨行列对齐（虚线关联）。 */
	void FrameGraphPanel::DrawGraphCanvas(FrameGraph& frame_graph, float canvas_height)
	{
		ImGui::BeginChild("##FrameGraphCanvas", ImVec2(0.0f, canvas_height), false,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		const ImVec2 canvas_min = ImGui::GetWindowPos();
		const ImVec2 canvas_size = ImGui::GetWindowSize();
		const ImVec2 canvas_max(canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y);
		const ImVec2 canvas_center(canvas_min.x + canvas_size.x * 0.5f, canvas_min.y + canvas_size.y * 0.5f);

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		/* 画布 = 工作区最深底（Neutral0）：节点（N2 / N3）都在它之上"浮"出来 */
		draw_list->AddRectFilled(canvas_min, canvas_max, ImGui::GetColorU32(EditorTheme::Token::Neutral0));

		/* 行背景 / 图标矩形每帧在绘制循环里记录（自检用；卡片矩形在函数末尾统一记） */
		m_LastLaneRects.clear();
		m_LastIconRects.clear();

		/* 工程点阵网格（archify 式底纹）：锚在图空间原点、随平移 / 缩放呼吸，
		 * 屏幕间距钳在 [14, 60] 防止过密或过疏；节点与连线都叠在它之上 */
		{
			const float spacing = std::clamp(34.0f * m_Zoom, 14.0f, 60.0f);
			const float dot_radius = std::max(0.6f, 1.0f * m_Zoom);
			const ImU32 dot_color = ImGui::GetColorU32(
				EditorTheme::WithAlpha(EditorTheme::Token::Neutral8, 0.26f));

			const float anchor_x = canvas_center.x + m_Pan.x;
			const float anchor_y = canvas_center.y + m_Pan.y;
			const auto first_aligned = [](float from, float anchor, float step)
			{
				return anchor + std::ceil((from - anchor) / step) * step;
			};

			for (float y = first_aligned(canvas_min.y, anchor_y, spacing); y < canvas_max.y; y += spacing)
			{
				for (float x = first_aligned(canvas_min.x, anchor_x, spacing); x < canvas_max.x; x += spacing)
					draw_list->AddRectFilled(ImVec2(x - dot_radius, y - dot_radius),
						ImVec2(x + dot_radius, y + dot_radius), dot_color);
			}
		}

		const float title_height = ImGui::GetFontSize() + 8.0f;
		/* 快照图下方"宽 x 高  格式"信息行的高度（图空间） */
		const float info_line_height = ImGui::GetFontSize() + 2.0f;

		/* ---- 收集行：每个生效 Pass 一行 —— 行头 = 整行背景区域（无缩略图），输出资源卡叠在区域内
		 * 横排；行序 = Pass 的拓扑深度（生产者在上、消费者在下，最长路径松弛）。 ---- */
		struct NodeView
		{
			std::string Name;
			std::string LaneName;   /* 所属行的 Pass 名（自检记录用） */
			bool IsPass{ false };   /* Pass 背景区域（行头与背景合并）or 区域内的资源卡 */
			bool IsCulled{ false };
			size_t LaneIndex{ 0 };
			const std::vector<FrameGraphCapture::Output>* Outputs{ nullptr }; /* Pass 背景区域：全部输出（xN 徽标 / 悬停网格） */
			const FrameGraphCapture::Output* Image{ nullptr };                /* 资源卡内显示的缩略图 */
			ImVec2 ImageSize{ 0.0f, 0.0f };      /* 图空间尺寸（按宽高比内接） */
			ImVec2 ImageScreenMin{};             /* 绘制时填充（悬停放大 / 自检用） */
			ImVec2 ImageScreenMax{};
			std::string InfoText;                /* 图下方信息行（"宽 x 高  格式"；无图时为空） */
			float InfoTextWidth{ 0.0f };
			float Width{ 0.0f };
			float Height{ 0.0f };
			float X{ 0.0f };      /* 图空间中心 */
			float Y{ 0.0f };
			ImVec2 ScreenMin{};
			ImVec2 ScreenMax{};
		};

		struct LaneView
		{
			std::string PassName;
			const StageFill* Stage{ nullptr };
			bool IsCulled{ false };
			const std::vector<FrameGraphCapture::Output>* Outputs{ nullptr };
			size_t HeaderIndex{ 0 };              /* nodes 里的背景区域索引 */
			std::vector<size_t> ResourceIndices;  /* nodes 里的资源卡索引 */
			float Y{ 0.0f };                      /* 行背景顶（图空间） */
			float Height{ 0.0f };                 /* 行背景高（含内边距） */
			float Width{ 0.0f };                  /* 行内容宽（标题列 + 资源卡） */
			float ContentCenterY{ 0.0f };         /* 行内容垂直中心 */
		};

		const DependencyGraph& graph = frame_graph.GetDependencyGraph();
		const FrameGraphCapture& capture = frame_graph.GetCapture();
		std::vector<NodeView> nodes;
		std::vector<LaneView> lanes;

		/* 图节点索引 → 名字 / 类型 / 生效性（连接解析与显示判定用） */
		struct GraphNodeRef
		{
			std::string Name;
			bool IsPass{ false };
			bool IsValid{ false };
		};
		std::unordered_map<uint32_t, GraphNodeRef> node_refs;
		for (const auto& node : graph.GetNodes())
		{
			node_refs[node->Index] = GraphNodeRef{ node->DebugName,
				DynamicPtrCast<RenderPassNode>(node) != nullptr, node->IsValid() };
		}

		/* 连接解析：输出连接（Pass → 资源）记生产者、输入连接（资源 → Pass）记消费者 */
		std::unordered_map<std::string, std::vector<std::string>> producers_of;
		std::unordered_map<std::string, std::vector<std::string>> consumers_of;
		for (const auto& connection : graph.GetConnections())
		{
			const auto from_iter = node_refs.find(connection->FromNodeIdx);
			const auto to_iter = node_refs.find(connection->ToNodeIdx);
			if (from_iter == node_refs.end() || to_iter == node_refs.end())
				continue;

			const GraphNodeRef& from = from_iter->second;
			const GraphNodeRef& to = to_iter->second;
			if (!from.IsPass && to.IsPass)
				consumers_of[from.Name].push_back(to.Name);
			else if (from.IsPass && !to.IsPass)
				producers_of[to.Name].push_back(from.Name);
		}

		/* 名字级邻接（聚焦链 / 悬停预览用）：pass → 输入资源（consumers_of 反转）、
		 * pass → 输出资源（producers_of 反转）、名字 → 是否 Pass（链遍历时区分两侧） */
		std::unordered_map<std::string, std::vector<std::string>> inputs_of;
		std::unordered_map<std::string, std::vector<std::string>> outputs_of;
		std::unordered_map<std::string, bool> name_is_pass;
		for (const auto& [node_index, ref] : node_refs)
			name_is_pass[ref.Name] = ref.IsPass;
		for (const auto& [resource_name, resource_consumers] : consumers_of)
		{
			for (const std::string& consumer : resource_consumers)
				inputs_of[consumer].push_back(resource_name);
		}
		for (const auto& [resource_name, resource_producers] : producers_of)
		{
			for (const std::string& producer : resource_producers)
				outputs_of[producer].push_back(resource_name);
		}

		/* ---- 行序：Pass 间拓扑深度（经资源的产出 / 消费链松弛；同深度保持图内顺序） ---- */
		std::vector<std::pair<std::string, bool>> visible_passes; /* (名字, 是否生效) */
		for (const auto& node : graph.GetNodes())
		{
			const GraphNodeRef& ref = node_refs[node->Index];
			if (ref.IsPass && (m_ShowCulled || ref.IsValid))
				visible_passes.emplace_back(ref.Name, ref.IsValid);
		}

		std::unordered_map<std::string, int> pass_depth;
		for (const auto& [pass_name, pass_valid] : visible_passes)
			pass_depth[pass_name] = 0;
		for (size_t iteration = 0; iteration < visible_passes.size(); ++iteration)
		{
			bool changed = false;
			for (const auto& [resource_name, producers] : producers_of)
			{
				const auto consumers_iter = consumers_of.find(resource_name);
				if (consumers_iter == consumers_of.end())
					continue;

				for (const std::string& producer : producers)
				{
					const auto producer_depth = pass_depth.find(producer);
					if (producer_depth == pass_depth.end())
						continue;

					for (const std::string& consumer : consumers_iter->second)
					{
						const auto consumer_depth = pass_depth.find(consumer);
						if (consumer_depth != pass_depth.end()
							&& consumer_depth->second < producer_depth->second + 1)
						{
							consumer_depth->second = producer_depth->second + 1;
							changed = true;
						}
					}
				}
			}
			if (!changed)
				break;
		}
		std::stable_sort(visible_passes.begin(), visible_passes.end(),
			[&](const auto& left, const auto& right)
			{ return pass_depth[left.first] < pass_depth[right.first]; });

		/* ---- 建卡：Pass 背景区域（行头与背景合并）+ 区域内的资源卡 ---- */
		for (const auto& [pass_name, pass_valid] : visible_passes)
		{
			LaneView lane;
			lane.PassName = pass_name;
			lane.Stage = FindStageFill(pass_name);
			lane.IsCulled = !pass_valid;
			lane.Outputs = capture.GetPassOutputs(pass_name);
			lane.HeaderIndex = nodes.size(); /* 先占卡槽，lanes.push_back 前固定 */

			NodeView header;
			header.Name = pass_name;
			header.LaneName = pass_name;
			header.IsPass = true;
			header.IsCulled = lane.IsCulled;
			header.Outputs = lane.Outputs;
			/* Pass 卡：行头和背景合并成的一张卡（占整行高，名字独占首行、资源内容在带子下面）。
			 * 宽度按标题带算（多输出加 xN 徽标区），高 = 标题带 + 内容行。 */
			header.Width = kLanePadding + kNodeIconBox + 6.0f
				+ ImGui::CalcTextSize(header.Name.c_str()).x + kLanePadding;
			if (lane.Outputs != nullptr && lane.Outputs->size() > 1)
			{
				const std::string count_label = "x" + std::to_string(lane.Outputs->size());
				header.Width += ImGui::CalcTextSize(count_label.c_str()).x + 6.0f;
			}
			header.Height = 0.0f; /* 行布局里取"标题带 + 内容行"的整行高 */
			header.LaneIndex = lanes.size();
			nodes.push_back(std::move(header));

			if (lane.Outputs != nullptr)
			{
				lane.ResourceIndices.reserve(lane.Outputs->size());
				for (const FrameGraphCapture::Output& output : *lane.Outputs)
				{
					const TextureDesc& desc = output.Preview->GetTextureDesc();
					const float aspect = desc.Height > 0
						? static_cast<float>(desc.Width) / static_cast<float>(desc.Height) : 1.0f;

					NodeView card;
					card.Name = output.Name;
					card.LaneName = pass_name;
					card.IsCulled = lane.IsCulled;
					card.LaneIndex = lanes.size();
					card.Image = &output;
					card.ImageSize = FitBox(kResourceImageBox, aspect);

					/* 贴图的尺寸与格式直接显示在卡上（图下方的信息行） */
					card.InfoText = std::to_string(desc.Width) + " x " + std::to_string(desc.Height)
						+ "  " + GetEnumName(desc.Format);
					card.InfoTextWidth = ImGui::CalcTextSize(card.InfoText.c_str()).x;

					/* 卡宽：标题 = 图标 + 间隙 + 名字（居中成组）；信息行与快照图
					 * 共用窄边距（贴图两侧贴边） */
					const float text_width = ImGui::CalcTextSize(card.Name.c_str()).x;
					float content_width = std::max(text_width + kNodeIconBox + 23.0f,
						card.InfoTextWidth * kInfoTextScale + kImageSidePadding * 2.0f);
					content_width = std::max(content_width, card.ImageSize.x + kImageSidePadding * 2.0f);
					card.Width = content_width;
					card.Height = title_height + card.ImageSize.y + info_line_height + kImageSidePadding;

					lane.ResourceIndices.push_back(nodes.size());
					nodes.push_back(std::move(card));
				}
			}

			lanes.push_back(std::move(lane));
		}

		/* ---- 行布局（图空间）：资源列对齐 —— 同名资源共享同一列（列按资源名首次出现分配、列宽取
		 * 最宽的副本），副本跨行严格对齐；标题带不占纵列，资源列从区域左内沿起排；背景区域统一宽度。 */
		std::vector<std::string> resource_columns;          /* 列序 → 资源名 */
		std::unordered_map<std::string, size_t> column_of;  /* 资源名 → 列号 */
		for (const LaneView& lane : lanes)
		{
			for (const size_t index : lane.ResourceIndices)
			{
				const std::string& name = nodes[index].Name;
				if (column_of.find(name) == column_of.end())
				{
					column_of[name] = resource_columns.size();
					resource_columns.push_back(name);
				}
			}
		}

		std::vector<float> column_width(resource_columns.size(), 0.0f);
		std::vector<float> column_center(resource_columns.size(), 0.0f);
		for (const LaneView& lane : lanes)
		{
			for (const size_t index : lane.ResourceIndices)
			{
				const size_t column = column_of[nodes[index].Name];
				column_width[column] = std::max(column_width[column], nodes[index].Width);
			}
		}
		{
			/* 第一列左缘 = 区域左内边（x = 0）：标题带之下就是资源内容行 */
			float x_cursor = 0.0f;
			for (size_t column = 0; column < resource_columns.size(); ++column)
			{
				column_center[column] = x_cursor + column_width[column] * 0.5f;
				x_cursor += column_width[column] + kNodeGap;
			}
		}

		for (LaneView& lane : lanes)
		{
			/* 卡片各自就位到自己的列（相同资源跨行对齐）；
			 * 行宽 = max(标题带宽度, 该行最右资源卡右缘) */
			float row_right = nodes[lane.HeaderIndex].Width;
			for (const size_t index : lane.ResourceIndices)
			{
				NodeView& card = nodes[index];
				const size_t column = column_of[card.Name];
				card.X = column_center[column];
				row_right = std::max(row_right, card.X + column_width[column] * 0.5f);
			}

			lane.Width = row_right;
		}

		/* ---- 同名资源副本链（被多个 Pass 写入的 RT）：相邻行的副本用虚线关联
		 * —— 副本已列对齐，关联线就是一条竖直虚线；语义 = 同一张 RT 的先后写入
		 * （与实线依赖箭头区分：无箭头、中性色、虚线 + 两端小圆点） */
		std::vector<std::pair<size_t, size_t>> same_rt_links; /* (上行副本, 下行副本) */
		{
			std::unordered_map<std::string, size_t> previous_copy;
			for (const LaneView& lane : lanes)
			{
				for (const size_t index : lane.ResourceIndices)
				{
					const std::string& name = nodes[index].Name;
					const auto iter = previous_copy.find(name);
					if (iter != previous_copy.end())
						same_rt_links.emplace_back(iter->second, index);
					previous_copy[name] = index;
				}
			}
		}

		float max_lane_width = 0.0f;
		float y_cursor = 0.0f;
		/* 行顶 → 资源内容行顶：标题带（行顶 5 + 标题行高）之下隔着 kTitleGap ——
		 * 名字独占首行，资源卡从标题带下方的一行起排 */
		const float content_top_offset = kLanePadding * 0.5f + title_height + kTitleGap;
		for (LaneView& lane : lanes)
		{
			/* 行高 = 标题带 + 资源内容行（卡高的最大者；没有资源的行留最小体量） */
			float content_height = kPassContentMinHeight;
			for (const size_t index : lane.ResourceIndices)
				content_height = std::max(content_height, nodes[index].Height);

			lane.Y = y_cursor;
			lane.Height = content_top_offset + content_height + kLanePadding;
			lane.ContentCenterY = y_cursor + content_top_offset + content_height * 0.5f;
			nodes[lane.HeaderIndex].Height = lane.Height; /* Pass 卡 = 整行高 */
			nodes[lane.HeaderIndex].Y = y_cursor + lane.Height * 0.5f;
			for (const size_t index : lane.ResourceIndices)
				nodes[index].Y = lane.ContentCenterY;

			max_lane_width = std::max(max_lane_width, lane.Width);
			y_cursor += lane.Height + kLaneGap;
		}

		/* Pass 节点 = 整行的背景区域（行头与背景合并的载体）：统一宽度
		 * （含左右内边距，所有行对齐）、整行高；输出资源卡叠在区域内部 */
		for (LaneView& lane : lanes)
		{
			NodeView& region = nodes[lane.HeaderIndex];
			region.X = max_lane_width * 0.5f;
			region.Width = max_lane_width + kLanePadding * 2.0f;
		}

		/* ---- 跨行连线规划：相邻直连 / 其余绕左侧总线 ----
		 * 语义：资源（最后写入的那个副本）→ 消费它的 Pass 区域。相邻直连 = 源卡底部竖直下行 →
		 * 目标区域顶边；其余绕总线（源卡底 → 总线列 → 目标区域入口）。总线在区域左缘之外、不穿卡片，
		 * 多条边在空档 / 总线 / 入口三处分槽、互不重叠。 */
		struct CrossEdge
		{
			size_t WriterIndex{ 0 };  /* 源资源卡（nodes 索引） */
			size_t TargetLane{ 0 };   /* 目标行（lanes 索引） */
			bool Direct{ false };     /* 相邻行：源卡底 → 目标区域顶（竖直直连） */
			float ExitX{ 0.0f };      /* 源卡底边的出发 x（图空间；总线走线用） */
			float ExitY{ 0.0f };      /* 源行下方空档里的左行高度（图空间） */
			float EntryY{ 0.0f };     /* 进入目标区域左缘的入口高度（图空间） */
			float RailX{ 0.0f };      /* 总线列（图空间） */
		};
		std::vector<CrossEdge> cross_edges;
		float rails_min_x = -kLanePadding; /* fit 包围盒要含总线（无连线时 = 行左缘） */
		{
			/* 资源名 → 最后写入副本（Sequence 最大者；其余副本不引出连线） */
			std::unordered_map<std::string, size_t> last_writer;
			for (const LaneView& lane : lanes)
			{
				for (const size_t index : lane.ResourceIndices)
				{
					const NodeView& card = nodes[index];
					if (card.Image == nullptr)
						continue;
					const auto iter = last_writer.find(card.Name);
					if (iter == last_writer.end()
						|| nodes[iter->second].Image->Sequence < card.Image->Sequence)
						last_writer[card.Name] = index;
				}
			}

			std::unordered_map<std::string, size_t> lane_of_pass;
			for (size_t l = 0; l < lanes.size(); ++l)
				lane_of_pass[lanes[l].PassName] = l;

			/* 按行内顺序收集（确定性的分槽与绘制顺序） */
			for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index)
			{
				for (const size_t index : lanes[lane_index].ResourceIndices)
				{
					const NodeView& card = nodes[index];
					const auto writer_iter = last_writer.find(card.Name);
					if (writer_iter == last_writer.end() || writer_iter->second != index)
						continue; /* 不是最后写入副本：不引出连线 */

					const auto consumers_iter = consumers_of.find(card.Name);
					if (consumers_iter == consumers_of.end())
						continue;

					for (const std::string& consumer : consumers_iter->second)
					{
						const auto target_iter = lane_of_pass.find(consumer);
						if (target_iter == lane_of_pass.end() || target_iter->second <= lane_index)
							continue; /* 同行 / 上方目标不画（次序上不可能出现在下方） */

						CrossEdge edge;
						edge.WriterIndex = index;
						edge.TargetLane = target_iter->second;
						cross_edges.push_back(edge);
					}
				}
			}

			std::stable_sort(cross_edges.begin(), cross_edges.end(),
				[&](const CrossEdge& a, const CrossEdge& b)
				{
					const NodeView& card_a = nodes[a.WriterIndex];
					const NodeView& card_b = nodes[b.WriterIndex];
					if (card_a.LaneIndex != card_b.LaneIndex)
						return card_a.LaneIndex < card_b.LaneIndex;
					if (a.TargetLane != b.TargetLane)
						return a.TargetLane < b.TargetLane;
					if (card_a.X != card_b.X)
						return card_a.X < card_b.X;
					return a.WriterIndex < b.WriterIndex;
				});

			/* 相邻行（目标 = 源的下一行）走直连：源卡底竖直下行、直接进
			 * 目标区域顶边 —— 不参与下面的总线分槽（那条路只有弯线才需要） */
			for (CrossEdge& edge : cross_edges)
				edge.Direct = (edge.TargetLane == nodes[edge.WriterIndex].LaneIndex + 1);

			std::vector<CrossEdge*> rail_edges;
			for (CrossEdge& edge : cross_edges)
			{
				if (!edge.Direct)
					rail_edges.push_back(&edge);
			}

			/* ① 源行空档分槽：同一条行内的出线各占一个高度（空档等分） */
			for (size_t begin = 0; begin < rail_edges.size();)
			{
				size_t end = begin;
				const size_t source_lane = nodes[rail_edges[begin]->WriterIndex].LaneIndex;
				while (end < rail_edges.size()
					&& nodes[rail_edges[end]->WriterIndex].LaneIndex == source_lane)
					++end;

				const float gap_top = lanes[source_lane].Y + lanes[source_lane].Height;
				const size_t count = end - begin;
				for (size_t k = 0; k < count; ++k)
				{
					rail_edges[begin + k]->ExitY = gap_top
						+ kLaneGap * (static_cast<float>(k + 1) / static_cast<float>(count + 1));
				}

				/* 同一张卡的出线口沿底边错开（下行段不重叠） */
				for (size_t card_begin = begin; card_begin < end;)
				{
					size_t card_end = card_begin;
					const size_t writer = rail_edges[card_begin]->WriterIndex;
					while (card_end < end && rail_edges[card_end]->WriterIndex == writer)
						++card_end;

					const size_t card_count = card_end - card_begin;
					const float center_x = nodes[writer].X;
					for (size_t k = 0; k < card_count; ++k)
					{
						rail_edges[card_begin + k]->ExitX = center_x
							+ (static_cast<float>(k) - (static_cast<float>(card_count) - 1.0f) * 0.5f) * 13.0f;
					}
					card_begin = card_end;
				}
				begin = end;
			}

			/* ② 目标卡入口分槽：每条（绕行）边在目标区域左缘上占一个高度、
			 * 各带一个箭头（不强制收束成一个）—— 区域是整行高的，纵向余量
			 * 足够，箭头彼此分开；入口按源序自上而下排，间距随区域高收敛 */
			{
				std::unordered_map<size_t, std::vector<CrossEdge*>> edges_by_target;
				for (CrossEdge* edge : rail_edges)
					edges_by_target[edge->TargetLane].push_back(edge);

				for (auto& [target_lane, targets] : edges_by_target)
				{
					const NodeView& card = nodes[lanes[target_lane].HeaderIndex];
					const size_t count = targets.size();
					const float available = std::max(card.Height - 24.0f, 12.0f);
					const float step = std::min(22.0f, available / static_cast<float>(count + 1));
					for (size_t t = 0; t < count; ++t)
					{
						targets[t]->EntryY = card.Y
							+ (static_cast<float>(t) - (static_cast<float>(count) - 1.0f) * 0.5f) * step;
					}
				}
			}

			/* ③ 总线分列：纵向区间（含端点的折角余量）互不重叠者共用一列，
			 * 否则往左再起一列 —— 总线上不会出现两条边叠在一起的段 */
			{
				constexpr float kRailMargin = 7.0f;
				std::vector<std::vector<std::pair<float, float>>> rails; /* 每列已占用的 [min, max] */
				for (CrossEdge* edge : rail_edges)
				{
					const float span_min = std::min(edge->ExitY, edge->EntryY) - kRailMargin;
					const float span_max = std::max(edge->ExitY, edge->EntryY) + kRailMargin;

					size_t rail_index = rails.size();
					for (size_t r = 0; r < rails.size(); ++r)
					{
						bool free = true;
						for (const auto& span : rails[r])
						{
							if (span_min < span.second && span_max > span.first)
							{
								free = false;
								break;
							}
						}
						if (free)
						{
							rail_index = r;
							break;
						}
					}
					if (rail_index == rails.size())
						rails.emplace_back();

					rails[rail_index].emplace_back(span_min, span_max);
					edge->RailX = -(kLanePadding + kRailInset)
						- static_cast<float>(rail_index) * kRailSpacing;
				}

				if (!rails.empty())
				{
					rails_min_x = -(kLanePadding + kRailInset)
						- static_cast<float>(rails.size() - 1) * kRailSpacing;
				}
			}
		}

		/* ---- 视图变换：首次进入（或点 Fit / 换相机之后）自动适配；屏幕 = 画布中心 + 平移 + 图·缩放 ---- */
		const auto compute_screen_rects = [&]()
		{
			for (NodeView& node : nodes)
			{
				node.ScreenMin = ImVec2(canvas_center.x + m_Pan.x + (node.X - node.Width * 0.5f) * m_Zoom,
					canvas_center.y + m_Pan.y + (node.Y - node.Height * 0.5f) * m_Zoom);
				node.ScreenMax = ImVec2(canvas_center.x + m_Pan.x + (node.X + node.Width * 0.5f) * m_Zoom,
					canvas_center.y + m_Pan.y + (node.Y + node.Height * 0.5f) * m_Zoom);
			}
		};

		/* 图空间 → 屏幕坐标（行背景 / 连线用；卡片另走 ScreenMin/Max） */
		const auto graph_to_screen = [&](const ImVec2& point)
		{
			return ImVec2(canvas_center.x + m_Pan.x + point.x * m_Zoom,
				canvas_center.y + m_Pan.y + point.y * m_Zoom);
		};

		if (m_Zoom <= 0.0f)
		{
			/* Fit 的包围盒 = 行背景的整体范围（含内边距）+ 跨行连线占用的总线列 */
			const float fit_min_x = cross_edges.empty()
				? -kLanePadding : std::min(-kLanePadding, rails_min_x - 8.0f);
			const ImVec2 bounds_min(fit_min_x, lanes.empty() ? 0.0f : lanes.front().Y);
			const ImVec2 bounds_max(max_lane_width + kLanePadding,
				lanes.empty() ? 0.0f : lanes.back().Y + lanes.back().Height);

			if (!lanes.empty())
			{
				const float fit_width = std::max(canvas_size.x - kCanvasPadding * 2.0f, 40.0f);
				const float fit_height = std::max(canvas_size.y - kCanvasPadding * 2.0f, 40.0f);
				const float bounds_width = std::max(bounds_max.x - bounds_min.x, 1.0f);
				const float bounds_height = std::max(bounds_max.y - bounds_min.y, 1.0f);
				m_Zoom = std::clamp(std::min(fit_width / bounds_width, fit_height / bounds_height), 0.3f, 1.5f);

				const ImVec2 bounds_center((bounds_min.x + bounds_max.x) * 0.5f, (bounds_min.y + bounds_max.y) * 0.5f);
				m_Pan = ImVec2(-bounds_center.x * m_Zoom, -bounds_center.y * m_Zoom);
			}
			else
			{
				m_Zoom = 1.0f;
				m_Pan = ImVec2(0.0f, 0.0f);
			}
		}
		compute_screen_rects();

		/* ---- 交互面：铺满画布的一个不可见按钮（缩放 / 平移 / 点选都挂它） ---- */
		ImGui::SetCursorScreenPos(canvas_min);
		ImGui::InvisibleButton("##GraphSurface", canvas_size, ImGuiButtonFlags_MouseButtonLeft);

		const ImGuiIO& io = ImGui::GetIO();
		const bool surface_hovered = ImGui::IsItemHovered();

		/* 命中测试：节点矩形不重叠，直接遍历（后加的在上） */
		const auto hit_test = [&](const ImVec2& pos) -> int
		{
			for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; --i)
			{
				if (pos.x >= nodes[i].ScreenMin.x && pos.x <= nodes[i].ScreenMax.x
					&& pos.y >= nodes[i].ScreenMin.y && pos.y <= nodes[i].ScreenMax.y)
					return i;
			}
			return -1;
		};

		/* 滚轮缩放：以鼠标为锚点（锚点下的图空间点在缩放前后停在鼠标位置） */
		if (surface_hovered && io.MouseWheel != 0.0f && !nodes.empty())
		{
			const float new_zoom = std::clamp(m_Zoom * std::pow(1.15f, io.MouseWheel), kMinZoom, kMaxZoom);
			if (new_zoom != m_Zoom)
			{
				const ImVec2 anchor_graph(
					(io.MousePos.x - canvas_center.x - m_Pan.x) / m_Zoom,
					(io.MousePos.y - canvas_center.y - m_Pan.y) / m_Zoom);
				m_Zoom = new_zoom;
				m_Pan = ImVec2(io.MousePos.x - canvas_center.x - anchor_graph.x * new_zoom,
					io.MousePos.y - canvas_center.y - anchor_graph.y * new_zoom);
				compute_screen_rects();
			}
		}

		/* 左键拖拽 = 平移；位移未超阈值则当点击（松开时选中命中的 Pass 节点） */
		if (ImGui::IsItemActivated())
		{
			m_IsDragging = true;
			m_DraggedDistance = 0.0f;
		}
		if (ImGui::IsItemActive())
		{
			m_DraggedDistance += std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y);
			if (m_DraggedDistance > kDragSlop)
			{
				m_Pan.x += io.MouseDelta.x;
				m_Pan.y += io.MouseDelta.y;
				compute_screen_rects();
			}
		}
		if (ImGui::IsItemDeactivated())
		{
			m_IsDragging = false;
			if (m_DraggedDistance <= kDragSlop)
			{
				/* 点击卡片 = 聚焦（archify 式：高亮整条传递链、其余变暗）；
				 * 再点同一张卡或点空白 = 取消聚焦 */
				const int clicked = hit_test(io.MousePos);
				if (clicked >= 0)
				{
					const NodeView& clicked_node = nodes[static_cast<size_t>(clicked)];
					const bool same_card = m_Focus.Name == clicked_node.Name
						&& m_Focus.LaneName == clicked_node.LaneName;
					if (same_card)
					{
						m_Focus = FocusRef{};
						m_SelectedPassName.clear();
					}
					else
					{
						m_Focus = FocusRef{ clicked_node.LaneName, clicked_node.Name, clicked_node.IsPass };
						m_SelectedPassName = clicked_node.IsPass ? clicked_node.Name : std::string();
					}
				}
				else
				{
					m_Focus = FocusRef{};
					m_SelectedPassName.clear();
				}
			}
		}

		const int hovered_node = (surface_hovered && !m_IsDragging) ? hit_test(io.MousePos) : -1;
		if (hovered_node >= 0)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

		/* ---- 激活高亮（archify 式交互）：搜索 > 聚焦 > 悬停预览 ----
		 * 命中集合外的卡 / 行背景 / 连线统一变暗；聚焦链 = 从焦点（资源或 Pass）
		 * 沿依赖图有向可达的上游（产出侧）与下游（消费侧）全部名字。 */
		std::unordered_set<std::string> highlight_names;
		bool highlight_active = false;
		{
			/* 大小写不敏感的子串匹配（搜索） */
			const auto matches_filter = [&](const std::string& candidate)
			{
				std::string lowered_candidate = candidate;
				std::string lowered_filter = m_SearchFilter;
				std::transform(lowered_candidate.begin(), lowered_candidate.end(), lowered_candidate.begin(),
					[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
				std::transform(lowered_filter.begin(), lowered_filter.end(), lowered_filter.begin(),
					[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
				return lowered_candidate.find(lowered_filter) != std::string::npos;
			};

			/* 有向遍历：upstream = res→产出 pass / pass→输入 res；downstream 反向 */
			const auto collect_reachable = [&](const std::string& origin, bool upstream)
			{
				std::unordered_set<std::string> visited;
				std::vector<std::string> frontier;
				visited.insert(origin);
				frontier.push_back(origin);
				for (size_t cursor = 0; cursor < frontier.size(); ++cursor)
				{
					const std::string current = frontier[cursor];
					const bool current_is_pass = name_is_pass.count(current) ? name_is_pass[current] : false;

					const std::unordered_map<std::string, std::vector<std::string>>* table = nullptr;
					if (current_is_pass)
						table = upstream ? &inputs_of : &outputs_of;
					else
						table = upstream ? &producers_of : &consumers_of;

					const auto neighbours_iter = table->find(current);
					if (neighbours_iter == table->end())
						continue;
					for (const std::string& neighbour : neighbours_iter->second)
					{
						if (visited.insert(neighbour).second)
							frontier.push_back(neighbour);
					}
				}
				return visited;
			};

			if (!m_SearchFilter.empty())
			{
				/* 搜索激活：只亮匹配的卡 */
				highlight_active = true;
				for (const NodeView& node : nodes)
				{
					if (matches_filter(node.Name))
						highlight_names.insert(node.Name);
				}
			}
			else if (!m_Focus.Name.empty())
			{
				/* 聚焦：亮整条传递链（上游 + 下游 + 焦点自身） */
				highlight_active = true;
				highlight_names = collect_reachable(m_Focus.Name, true);
				const std::unordered_set<std::string> downstream = collect_reachable(m_Focus.Name, false);
				highlight_names.insert(downstream.begin(), downstream.end());
			}
			else if (hovered_node >= 0 && static_cast<size_t>(hovered_node) < nodes.size())
			{
				/* 悬停预览（intent trace）：亮该卡的一跳上下游 */
				highlight_active = true;
				const NodeView& hovered = nodes[static_cast<size_t>(hovered_node)];
				highlight_names.insert(hovered.Name);

				const auto append_neighbours = [&](const std::unordered_map<std::string, std::vector<std::string>>& table)
				{
					const auto iter = table.find(hovered.Name);
					if (iter == table.end())
						return;
					for (const std::string& neighbour : iter->second)
						highlight_names.insert(neighbour);
				};
				if (hovered.IsPass)
				{
					append_neighbours(inputs_of);
					append_neighbours(outputs_of);
				}
				else
				{
					append_neighbours(producers_of);
					append_neighbours(consumers_of);
				}
			}
		}

		/* 卡的变暗系数（悬停的卡保持全亮 —— 悬停反馈优先于变暗） */
		const auto node_dim_by_index = [&](size_t index) -> float
		{
			if (!highlight_active || highlight_names.count(nodes[index].Name))
				return 1.0f;
			if (hovered_node >= 0 && static_cast<size_t>(hovered_node) == index)
				return 1.0f;
			return 0.22f;
		};

		/* ---- 绘制：行背景在底、连线在中、卡片在上 ---- */
		draw_list->PushClipRect(canvas_min, canvas_max, true);

		/* 连线笔触：粗细统一、圆角连续（感知缩放但有上下限，低倍不发虚、高倍也不压过卡片描边）；
		 * 箭头 = 实心三角，长度按缩放取 12 图空间单位（入口段可用长度 kRailInset=14，留了转角余量）。 */
		const float edge_thickness = std::clamp(2.2f * m_Zoom, 1.8f, 3.2f);
		const float arrow_length = 12.0f * m_Zoom;

		/* Pass 背景区域（行头与背景合并的载体）：整行、统一宽度的背景层 ——
		 * 先于连线绘制，连线（资源卡底的下行段）才能从区域上方经过；
		 * 区域 = 深色同相淡底 + 细描边，标题与输出资源卡叠在其上 */
		for (const LaneView& lane : lanes)
		{
			const NodeView& region = nodes[lane.HeaderIndex];
			const bool region_hovered = (hovered_node == static_cast<int>(lane.HeaderIndex));
			const bool region_selected = !m_Focus.Name.empty()
				&& region.Name == m_Focus.Name && region.LaneName == m_Focus.LaneName;

			ImVec4 fill = lane.Stage != nullptr
				? EditorTheme::WithAlpha(lane.Stage->Fill, 0.45f)
				: EditorTheme::WithAlpha(EditorTheme::Token::Neutral3, 0.42f);
			ImVec4 border = lane.Stage != nullptr
				? EditorTheme::WithAlpha(lane.Stage->Stroke, 0.30f)
				: EditorTheme::WithAlpha(EditorTheme::Token::Border, 0.50f);
			if (lane.IsCulled)
			{
				fill.w *= 0.5f;
				border.w *= 0.5f;
			}
			if (region_selected)
				border = EditorTheme::Token::Accent;
			else if (region_hovered)
				border = lane.Stage != nullptr ? BrightenedStroke(lane.Stage->Stroke)
					: EditorTheme::Token::TextDim;

			/* 区域跟随变暗（聚焦 / 搜索 / 悬停预览） */
			const float region_dim = region_hovered ? 1.0f : node_dim_by_index(lane.HeaderIndex);
			fill.w *= region_dim;
			border.w *= region_dim;

			draw_list->AddRectFilled(region.ScreenMin, region.ScreenMax, ImGui::GetColorU32(fill), 7.0f);
			/* 区域描边：随缩放加粗（选中再 +1px）；低倍仍有 1.5px 的下限 */
			const float region_border = std::clamp(1.8f * m_Zoom, 1.5f, 2.6f)
				+ (region_selected ? 1.0f : 0.0f);
			draw_list->AddRect(region.ScreenMin, region.ScreenMax, ImGui::GetColorU32(border), 7.0f, 0,
				region_border);

			/* 自检：背景区域矩形（行头与背景合并后的载体） */
			m_LastLaneRects.push_back(LaneRect{ region.Name, region.ScreenMin, region.ScreenMax });
		}

		/* ② 跨行连线：相邻行直连（源卡底竖直下行 → 目标区域顶边、箭头朝下）
		 * 或 绕左侧总线（源卡底 → 空档左行 → 总线下行 → 目标区域左缘右进）；
		 * 绕行段一笔画完、全直角转折（AddOrthogonalPath：纯直角 + 圆头连接） */
		for (const CrossEdge& edge : cross_edges)
		{
			const NodeView& source = nodes[edge.WriterIndex];
			const LaneView& target_lane = lanes[edge.TargetLane];
			const NodeView& target = nodes[target_lane.HeaderIndex];

			ImVec4 line_color = lanes[source.LaneIndex].Stage != nullptr
				? EditorTheme::WithAlpha(lanes[source.LaneIndex].Stage->Stroke, 0.70f)
				: EditorTheme::Token::Neutral7;
			if (lanes[source.LaneIndex].IsCulled || target_lane.IsCulled)
				line_color = EditorTheme::WithAlpha(line_color, 0.45f);
			const bool endpoint_hovered = hovered_node == static_cast<int>(edge.WriterIndex)
				|| hovered_node == static_cast<int>(target_lane.HeaderIndex);
			if (endpoint_hovered)
				line_color = EditorTheme::Token::Accent;
			else /* 两端都亮才全亮（聚焦 / 搜索 / 悬停预览的变暗） */
				line_color.w *= std::min(node_dim_by_index(edge.WriterIndex),
					node_dim_by_index(target_lane.HeaderIndex));
			const ImU32 color_u32 = ImGui::GetColorU32(line_color);

			if (edge.Direct)
			{
				/* 相邻行直连：沿源卡中心竖直下行，实心三角箭头咬合目标区域顶边（线段和箭头必须同一坐标
				 * 口径 —— 走 AddExactLine 而不是 AddLine，否则偏半像素错位）。注意：线段止于箭头底边、不要
				 * 铺到尖端 —— 半透明连线（0.7）铺进三角会叠出更深的"线影"、尖端冒线头。 */
				const float center_x = graph_to_screen(ImVec2(source.X, 0.0f)).x;
				const float tip_y = target.ScreenMin.y;
				AddExactLine(draw_list, ImVec2(center_x, source.ScreenMax.y + 1.0f),
					ImVec2(center_x, tip_y - arrow_length), color_u32, edge_thickness);
				AddArrowHead(draw_list, ImVec2(center_x, tip_y), 0.0f, 1.0f, arrow_length, color_u32);
				continue;
			}

			/* 屏幕坐标：两端咬合在卡片边框上（无缝隙），中段由图空间换算 */
			const ImVec2 start(graph_to_screen(ImVec2(edge.ExitX, 0.0f)).x, source.ScreenMax.y);
			const float exit_y = graph_to_screen(ImVec2(0.0f, edge.ExitY)).y;
			const float entry_y = graph_to_screen(ImVec2(0.0f, edge.EntryY)).y;
			const float rail_x = graph_to_screen(ImVec2(edge.RailX, 0.0f)).x;
			const float tip_x = target.ScreenMin.x;

			/* 直角折线（下 → 左 → 下 → 右）：3 个直角转折，无圆弧倒角。
			 * 末段同样止于箭头底边（不铺到尖端）—— 同直连的口径：
			 * 半透明连线铺进三角会叠出"线影"、尖端冒线头（见上方直连的说明） */
			const ImVec2 path_points[] = {
				start,
				ImVec2(start.x, exit_y),
				ImVec2(rail_x, exit_y),
				ImVec2(rail_x, entry_y),
				ImVec2(tip_x - arrow_length, entry_y),
			};
			AddOrthogonalPath(draw_list, path_points, 5, color_u32, edge_thickness);

			/* 箭头：实心三角（archify 同款）指向目标 Pass 背景区域（依赖方向：外 → 内），
			 * 尖端咬合区域边框 */
			AddArrowHead(draw_list, ImVec2(tip_x, entry_y), 1.0f, 0.0f, arrow_length, color_u32);
		}

		const ImFont* font = ImGui::GetFont();
		const float font_size = ImGui::GetFontSize();
		const float text_size = std::max(font_size * m_Zoom, 6.0f);

		for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
		{
			NodeView& node = nodes[i]; /* 绘制时填充缩略图屏幕矩形 */
			const bool hovered = (i == hovered_node);
			const bool selected = !m_Focus.Name.empty()
				&& node.Name == m_Focus.Name && node.LaneName == m_Focus.LaneName;
			/* 变暗（聚焦 / 搜索 / 悬停预览）：悬停的卡保持全亮 */
			const float node_display_dim = hovered ? 1.0f : node_dim_by_index(static_cast<size_t>(i));

			/* Pass 背景区域（填充 / 描边）在背景层已经画过，这里只补标题。资源卡按渲染阶段着色（同相
			 * 深底 + 彩色描边；没命中阶段 = Neutral2 填充 + 虚线描边）。画布是最深的 Neutral0；被剔除的
			 * 节点压暗。 */
			const StageFill* stage_fill = FindStageFill(node.Name);
			ImVec4 text_color = node.IsPass ? EditorTheme::Token::Text : EditorTheme::Token::TextLabel;
			if (node.IsCulled)
				text_color = EditorTheme::Token::TextDim;
			text_color.w *= node_display_dim;

			if (!node.IsPass)
			{
				ImVec4 fill = stage_fill != nullptr
					? ResourceFillOf(stage_fill->Fill) : EditorTheme::Token::Neutral2;
				ImVec4 border = stage_fill != nullptr
					? stage_fill->Stroke
					: EditorTheme::WithAlpha(EditorTheme::Token::Border, 0.65f);
				if (node.IsCulled)
				{
					fill = EditorTheme::WithAlpha(fill, 0.5f);
					border = EditorTheme::WithAlpha(border, 0.5f);
				}
				if (hovered && !selected)
					border = stage_fill != nullptr ? BrightenedStroke(stage_fill->Stroke)
						: EditorTheme::Token::TextDim;
				if (selected)
					border = EditorTheme::Token::Accent;

				fill.w *= node_display_dim;
				border.w *= node_display_dim;

				draw_list->AddRectFilled(node.ScreenMin, node.ScreenMax, ImGui::GetColorU32(fill));
				/* 虚线描边：随缩放加粗（与区域描边同一档），dash/gap 同步放长 ——
				 * 细笔触 + 短 dash 的"齿感"是像素感的主要来源 */
				AddDashedRect(draw_list, node.ScreenMin, node.ScreenMax, ImGui::GetColorU32(border),
					std::clamp(2.0f * m_Zoom, 1.6f, 2.8f),
					std::max(5.5f, 6.0f * m_Zoom), std::max(3.5f, 4.5f * m_Zoom));
			}

			/* 标题：两种排版语言不一样（各带专属图标；小盒 < 12px 自动简化）—— Pass 区域 = 名字独占首行
			 * （阶段色图标 + 左对齐名字；多路输出右上角标 "xN"）；资源卡 = 卡片 + 宝石图标 + 名字居中成组。
			 * 图标盒 = 像素对齐的偶数边长方盒。 */
			float icon_box = std::floor(std::clamp(kNodeIconBox * m_Zoom, 8.0f, 14.0f));
			if (static_cast<int>(icon_box) % 2 != 0)
				icon_box -= 1.0f;

			const ImVec4 icon_color = stage_fill != nullptr
				? EditorTheme::WithAlpha(stage_fill->Stroke, node_display_dim)
				: EditorTheme::WithAlpha(EditorTheme::Token::TextDim, node_display_dim);
			const auto record_icon = [&](const ImVec2& top_left)
			{
				m_LastIconRects.push_back(IconRect{ node.Name, node.LaneName, node.IsPass,
					top_left, ImVec2(top_left.x + icon_box, top_left.y + icon_box) });
			};

			if (node.IsPass)
			{
				/* 标题带 = 名字独占的首行：图标与资源内容行的左缘同一条基线
				 * （左缘 kLanePadding 起），多路输出在右上角标 "xN" */
				const float title_center_y = node.ScreenMin.y
					+ (kLanePadding * 0.5f + title_height * 0.5f) * m_Zoom;
				const ImVec2 icon_tl(std::floor(node.ScreenMin.x + kLanePadding * m_Zoom),
					std::floor(title_center_y - icon_box * 0.5f));
				AddPassIconGlyph(draw_list, icon_tl, icon_box, ImGui::GetColorU32(icon_color));
				record_icon(icon_tl);

				draw_list->AddText(font, text_size,
					ImVec2(icon_tl.x + icon_box + 6.0f * m_Zoom, title_center_y - text_size * 0.5f),
					ImGui::GetColorU32(text_color), node.Name.c_str());

				if (node.Outputs != nullptr && node.Outputs->size() > 1)
				{
					const std::string count_label = "x" + std::to_string(node.Outputs->size());
					const float count_width = ImGui::CalcTextSize(count_label.c_str()).x * (text_size / font_size);
					draw_list->AddText(font, text_size,
						ImVec2(node.ScreenMax.x - count_width - kLanePadding * m_Zoom, title_center_y - text_size * 0.5f),
						ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::TextDim, node_display_dim)),
						count_label.c_str());
				}
			}
			else
			{
				/* 「图标 + 名字」作为一个整体水平居中 */
				const float text_width = ImGui::CalcTextSize(node.Name.c_str()).x * (text_size / font_size);
				const float icon_gap = std::max(3.0f, 5.0f * m_Zoom);
				const float block_width = icon_box + icon_gap + text_width;
				const float block_left = (node.ScreenMin.x + node.ScreenMax.x - block_width) * 0.5f;
				const float title_center_y = node.ScreenMin.y + title_height * 0.5f * m_Zoom;
				/* 图标盒顶部夹紧在卡片内（低倍下标题带比图标盒矮时，图标不以
				 * 卡片上缘出血 —— 只略微下移，视觉上仍居中） */
				const ImVec2 icon_tl(std::floor(block_left),
					std::max(std::floor(title_center_y - icon_box * 0.5f),
						std::floor(node.ScreenMin.y)));
				AddResourceIconGlyph(draw_list, icon_tl, icon_box, ImGui::GetColorU32(icon_color));
				record_icon(icon_tl);

				draw_list->AddText(font, text_size,
					ImVec2(icon_tl.x + icon_box + icon_gap, title_center_y - text_size * 0.5f),
					ImGui::GetColorU32(text_color), node.Name.c_str());
			}

			/* 快照图贴节点底部、水平居中（图直接显示在节点上 —— Pass 为第一路
			 * 输出、资源为该资源最近被写入的快照）；信息行在标题行下方（图占满
			 * 节点下半，说明文字紧随标题） */
			node.ImageScreenMin = node.ImageScreenMax = ImVec2(0.0f, 0.0f);
			if (node.Image != nullptr)
			{
				const float image_half_w = node.ImageSize.x * 0.5f * m_Zoom;
				const float image_height = node.ImageSize.y * m_Zoom;
				const float image_bottom = node.ScreenMax.y - kImageSidePadding * m_Zoom;
				node.ImageScreenMin = ImVec2((node.ScreenMin.x + node.ScreenMax.x) * 0.5f - image_half_w,
					image_bottom - image_height);
				node.ImageScreenMax = ImVec2(node.ImageScreenMin.x + image_half_w * 2.0f, image_bottom);

				/* 默认目标快照顶行在前（不翻转 V）；离屏纹理底行在前（翻转） */
				ImVec2 image_uv0, image_uv1;
				SnapshotImageUVs(*node.Image, image_uv0, image_uv1);
				draw_list->AddImage((ImTextureID)node.Image->Preview.get(),
					node.ImageScreenMin, node.ImageScreenMax, image_uv0, image_uv1,
					ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, node_display_dim)));

				/* 信息行：标题行下方（"宽 x 高  格式"） */
				const float info_size = std::max(font_size * kInfoTextScale * m_Zoom, 6.0f);
				const float info_width = node.InfoTextWidth * (info_size / font_size);
				ImVec4 info_color = node.IsCulled
					? EditorTheme::WithAlpha(EditorTheme::Token::TextDim, 0.55f)
					: EditorTheme::Token::TextDim;
				info_color.w *= node_display_dim;
				draw_list->AddText(font, info_size,
					ImVec2((node.ScreenMin.x + node.ScreenMax.x - info_width) * 0.5f,
						node.ScreenMin.y + (title_height + 1.0f) * m_Zoom),
					ImGui::GetColorU32(info_color),
					node.InfoText.c_str());
			}
		}

		/* ---- 同名资源关联线（画在卡片之上）：列对齐的副本之间一条竖直虚线
		 * + 两端小圆点 —— "同一张 RT 被多个 Pass 先后写入"的专属标记（中性色、
		 * 无箭头；与实线依赖箭头区分）。副本已列对齐，连线自然是纯竖直的 */
		for (const auto& [upper_index, lower_index] : same_rt_links)
		{
			const NodeView& upper = nodes[upper_index];
			const NodeView& lower = nodes[lower_index];
			const float center_x = graph_to_screen(ImVec2(upper.X, 0.0f)).x;

			ImVec4 link_color = EditorTheme::WithAlpha(EditorTheme::Token::TextDim, 0.90f);
			const bool endpoint_hovered = hovered_node == static_cast<int>(upper_index)
				|| hovered_node == static_cast<int>(lower_index);
			if (endpoint_hovered)
				link_color = EditorTheme::Token::Accent;
			else /* 两端都亮才全亮（聚焦 / 搜索 / 悬停预览的变暗） */
				link_color.w *= std::min(node_dim_by_index(upper_index),
					node_dim_by_index(lower_index));
			const ImU32 color_u32 = ImGui::GetColorU32(link_color);

			const ImVec2 from(center_x, upper.ScreenMax.y);
			const ImVec2 to(center_x, lower.ScreenMin.y);
			/* 与卡片描边同一档粗细（略细一挡）、dash 节奏同步放长 */
			AddDashedLine(draw_list, from, to, color_u32, std::clamp(1.5f * m_Zoom, 1.2f, 2.2f),
				std::max(4.5f, 5.0f * m_Zoom), std::max(3.0f, 4.0f * m_Zoom));

			const float dot_radius = std::max(1.3f, 1.9f * m_Zoom);
			draw_list->AddCircleFilled(from, dot_radius, color_u32);
			draw_list->AddCircleFilled(to, dot_radius, color_u32);
		}

		/* ---- 渲染阶段图例（archify 式）：画布右上角、只列本帧出现的阶段 ---- */
		{
			const StageFill* present_stages[sizeof(kStageFills) / sizeof(kStageFills[0])];
			size_t present_count = 0;
			for (const StageFill& stage : kStageFills)
			{
				for (const NodeView& node : nodes)
				{
					if (FindStageFill(node.Name) == &stage)
					{
						present_stages[present_count++] = &stage;
						break;
					}
				}
			}

			if (present_count > 0)
			{
				constexpr float kLegendPadding = 8.0f;
				constexpr float kLegendSwatch = 9.0f;
				const float row_height = font_size + 4.0f;

				float label_width = 0.0f;
				for (size_t i = 0; i < present_count; ++i)
					label_width = std::max(label_width, ImGui::CalcTextSize(present_stages[i]->Display).x);

				const float legend_width = kLegendPadding * 2.0f + kLegendSwatch + 6.0f + label_width;
				const float legend_height = kLegendPadding * 2.0f + row_height * static_cast<float>(present_count);
				const ImVec2 legend_min(canvas_max.x - legend_width - 12.0f, canvas_min.y + 12.0f);
				const ImVec2 legend_max(legend_min.x + legend_width, legend_min.y + legend_height);

				draw_list->AddRectFilled(legend_min, legend_max,
					ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Neutral2, 0.92f)), 6.0f);
				draw_list->AddRect(legend_min, legend_max,
					ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Border, 0.65f)), 6.0f);

				for (size_t i = 0; i < present_count; ++i)
				{
					const float row_y = legend_min.y + kLegendPadding + row_height * static_cast<float>(i);
					const float swatch_y = row_y + (row_height - kLegendSwatch) * 0.5f;
					draw_list->AddRectFilled(ImVec2(legend_min.x + kLegendPadding, swatch_y),
						ImVec2(legend_min.x + kLegendPadding + kLegendSwatch, swatch_y + kLegendSwatch),
						ImGui::GetColorU32(present_stages[i]->Stroke), 2.0f);
					draw_list->AddText(ImVec2(legend_min.x + kLegendPadding + kLegendSwatch + 6.0f, row_y + 2.0f),
						ImGui::GetColorU32(EditorTheme::Token::TextLabel), present_stages[i]->Display);
				}
			}
		}

		if (nodes.empty())
		{
			const char* hint = "No passes in the current render graph.";
			const ImVec2 hint_size = ImGui::CalcTextSize(hint);
			draw_list->AddText(ImVec2(canvas_center.x - hint_size.x * 0.5f, canvas_center.y - hint_size.y * 0.5f),
				ImGui::GetColorU32(EditorTheme::Token::TextDim), hint);
		}

		draw_list->PopClipRect();

		/* ---- 悬停提示：命中快照图 → 放大图 + 名字 / 尺寸 / 格式；否则节点信息
		 * （Pass 的多路输出在信息里平铺全部输出的图像网格） ---- */
		if (hovered_node >= 0)
		{
			const NodeView& node = nodes[hovered_node];

			const bool image_hovered = node.Image != nullptr
				&& io.MousePos.x >= node.ImageScreenMin.x && io.MousePos.x <= node.ImageScreenMax.x
				&& io.MousePos.y >= node.ImageScreenMin.y && io.MousePos.y <= node.ImageScreenMax.y;

			if (image_hovered)
			{
				const FrameGraphCapture::Output& output = *node.Image;
				const TextureDesc& desc = output.Preview->GetTextureDesc();
				const float aspect = desc.Height > 0
					? static_cast<float>(desc.Width) / static_cast<float>(desc.Height) : 1.0f;

				ImGui::BeginTooltip();
				ImVec2 hover_uv0, hover_uv1;
				SnapshotImageUVs(output, hover_uv0, hover_uv1);
				ImGui::Image((ImTextureID)output.Preview.get(), FitBox(kHoverImageBox, aspect),
					hover_uv0, hover_uv1);
				ImGui::TextUnformatted(output.Name.c_str());
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::Text("%u x %u  %s", desc.Width, desc.Height, GetEnumName(desc.Format));
				ImGui::TextUnformatted(node.Name.c_str());
				ImGui::PopStyleColor();
				ImGui::EndTooltip();
			}
			else
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(node.Name.c_str());
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextUnformatted(node.IsPass ? "Pass" : "Resource");
				/* 背景色对应的渲染阶段（颜色分区自解释） */
				if (const StageFill* node_stage = FindStageFill(node.Name))
					ImGui::Text("Stage: %s", node_stage->Display);
				if (node.IsCulled)
					ImGui::TextUnformatted("culled (not executed)");
				ImGui::PopStyleColor();

				if (node.IsPass)
				{
					if (node.Outputs == nullptr)
					{
						ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
						ImGui::TextUnformatted("No captured output (enable Capture, or no displayable attachment).");
						ImGui::PopStyleColor();
					}
					else
					{
						/* 多路输出：平铺全部输出的图像网格（含深度灰阶） */
						constexpr float kCellWidth = 132.0f;
						constexpr int kCellColumns = 3;
						for (size_t i = 0; i < node.Outputs->size(); ++i)
						{
							const FrameGraphCapture::Output& output = (*node.Outputs)[i];
							const TextureDesc& desc = output.Preview->GetTextureDesc();
							const float aspect = desc.Height > 0
								? static_cast<float>(desc.Width) / static_cast<float>(desc.Height) : 1.0f;
							const ImVec2 image_size = FitBox(kCellWidth, aspect);

							if (i > 0 && i % kCellColumns != 0)
								ImGui::SameLine();

							ImGui::PushID(static_cast<int>(i));
							ImGui::BeginGroup();
							ImVec2 cell_uv0, cell_uv1;
							SnapshotImageUVs(output, cell_uv0, cell_uv1);
							ImGui::Image((ImTextureID)output.Preview.get(), image_size, cell_uv0, cell_uv1);
							DrawClippedCaption(output.Name, kCellWidth);
							ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
							ImGui::Text("%u x %u  %s", desc.Width, desc.Height, GetEnumName(desc.Format));
							ImGui::PopStyleColor();
							ImGui::EndGroup();
							ImGui::PopID();
						}
					}
				}
				ImGui::EndTooltip();
			}
		}

		/* 自检 / 自动化验证用的节点与快照图布局（屏幕坐标） */
		m_LastNodeRects.clear();
		m_LastNodeRects.reserve(nodes.size());
		m_LastOutputRects.clear();
		for (const NodeView& node : nodes)
		{
			m_LastNodeRects.push_back(NodeRect{ node.Name, node.IsPass, node.ScreenMin, node.ScreenMax, node.InfoText });
			if (node.Image != nullptr)
			{
				/* 缩略图都属于资源卡（Pass 区域无图）；PassName 记所属行（同名资源
				 * 在不同行有各自的"当刻"副本，按 行名 + 资源名 一起定位） */
				m_LastOutputRects.push_back(OutputRect{ node.LaneName,
					node.Image->Name, node.ImageScreenMin, node.ImageScreenMax });
			}
		}

		ImGui::EndChild();
	}
}
