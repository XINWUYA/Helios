#include "Pch.h"
#include "EditorIcons.h"
#include "Svg.h"
#include "Helios/ImGui/EditorTheme.h"
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

		/* 统一描边语言：所有矢量图标共用同一线宽比例，视觉重量一致 */
		inline float StrokeWidth(float size)
		{
			return std::max(1.25f, size * 0.078f);
		}

		/* 第二档色：同色、低透明度（双色调图标用）。
		 * 仍然从调用方传入的颜色派生，所以"禁用变淡 / 选中浸染强调色"这些行为不受影响。 */
		inline ImU32 Soften(ImU32 color, float alpha_scale)
		{
			const ImU32 alpha = static_cast<ImU32>(static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha_scale);
			return (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
		}

		/* 点缀色沿用调用方透明度，确保禁用态与整枚图标同步变淡。 */
		inline ImU32 PaletteColor(ImU32 source, ImU32 palette, float alpha_scale = 1.0f)
		{
			const float source_alpha = static_cast<float>((source >> IM_COL32_A_SHIFT) & 0xFF);
			const float palette_alpha = static_cast<float>((palette >> IM_COL32_A_SHIFT) & 0xFF);
			const ImU32 alpha = static_cast<ImU32>(source_alpha * palette_alpha * alpha_scale / 255.0f);
			return (palette & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
		}

		constexpr ImU32 kViolet = IM_COL32(108, 104, 217, 255);
		constexpr ImU32 kMint = IM_COL32(39, 173, 145, 255);
		constexpr ImU32 kRed = IM_COL32(213, 92, 85, 255);	/* 与调色板 #d55c55 同值 */
		constexpr ImU32 kGray = IM_COL32(170, 178, 193, 255);	/* 与调色板 #aab2c1 同值（深浅两画布通用） */

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

		/* 用归一化点集填充一个凸多边形（双色调图标的"面"） */
		template <int N>
		void FillPoly(ImDrawList* draw_list, const Canvas& canvas, const float (&points)[N][2], ImU32 color)
		{
			ImVec2 buffer[N];
			for (int i = 0; i < N; ++i)
				buffer[i] = canvas.At(points[i][0], points[i][1]);

			draw_list->AddConvexPolyFilled(buffer, N, color);
		}

		/* 凹多边形填充（阶梯、拉长的运动体这类收不了凸）：走耳剪切 */
		template <int N>
		void FillConcavePoly(ImDrawList* draw_list, const Canvas& canvas, const float (&points)[N][2], ImU32 color)
		{
			std::vector<ImVec2> buffer;
			buffer.reserve(N);
			for (int i = 0; i < N; ++i)
				buffer.push_back(canvas.At(points[i][0], points[i][1]));

			Svg::FillPolygon(draw_list, buffer, color);
		}

		/* 虚线圆：光晕衰减环用 */
		void StrokeDashedCircle(ImDrawList* draw_list, const ImVec2& center, float radius, ImU32 color,
		                        float thickness, float dash, float gap)
		{
			const float circumference = 2.0f * kPi * radius;
			for (float offset = 0.0f; offset < circumference; offset += dash + gap)
			{
				draw_list->PathArcTo(center, radius, offset / radius,
					std::min(offset + dash, circumference) / radius, 6);
				draw_list->PathStroke(color, 0, thickness);
			}
		}

		/* ---- 文件 ---- */

		/* 文件页：所有"页面型"图标共用的外形（右上折角，折角填一层淡色） */
		void StrokePage(ImDrawList* dl, const Canvas& c, float thickness, ImU32 color)
		{
			static constexpr float kPage[5][2] = {
				{ 0.24f, 0.06f }, { 0.60f, 0.06f }, { 0.82f, 0.28f }, { 0.82f, 0.94f }, { 0.24f, 0.94f }
			};

			/* 翻起的一角：填色后一页纸才有立体感，不然只是条空心折线 */
			dl->AddTriangleFilled(c.At(0.60f, 0.06f), c.At(0.82f, 0.28f), c.At(0.60f, 0.28f),
				PaletteColor(color, kViolet, 0.18f));

			StrokePolyline(dl, c, kPage, thickness, color, true);

			dl->AddLine(c.At(0.60f, 0.06f), c.At(0.60f, 0.28f), PaletteColor(color, kViolet), thickness);
			dl->AddLine(c.At(0.60f, 0.28f), c.At(0.82f, 0.28f), PaletteColor(color, kViolet), thickness);
		}

		void DrawNewScene(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			/* 空白文档 = 只有页，没有内容线 */
			StrokePage(dl, Canvas{ center, size }, StrokeWidth(size), color);
		}

		void DrawOpenScene(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 打开的文件夹：后片与前片错层，前片以淡紫填充表达打开动作 */
			static constexpr float kFolder[6][2] = {
				{ 0.10f, 0.83f }, { 0.10f, 0.25f }, { 0.39f, 0.25f },
				{ 0.50f, 0.39f }, { 0.90f, 0.39f }, { 0.90f, 0.83f }
			};
			static constexpr float kFront[4][2] = {
				{ 0.06f, 0.48f }, { 0.91f, 0.48f }, { 0.78f, 0.84f }, { 0.02f, 0.84f }
			};
			FillPoly(dl, c, kFront, PaletteColor(color, kViolet, 0.18f));
			StrokePolyline(dl, c, kFolder, t, color, true);
			StrokePolyline(dl, c, kFront, t, PaletteColor(color, kViolet), true);
			dl->AddLine(c.At(0.16f, 0.66f), c.At(0.56f, 0.66f), PaletteColor(color, kMint), t);
		}

		void DrawSave(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 软盘轮廓：淡紫标签窗配一条薄荷色保存指示 */
			dl->AddRectFilled(c.At(0.30f, 0.54f), c.At(0.70f, 0.83f), PaletteColor(color, kViolet, 0.18f), c.Len(0.025f));
			dl->AddRectFilled(c.At(0.35f, 0.16f), c.At(0.65f, 0.38f), PaletteColor(color, kViolet, 0.12f));

			static constexpr float kShell[5][2] = {
				{ 0.15f, 0.15f }, { 0.68f, 0.15f }, { 0.85f, 0.32f }, { 0.85f, 0.85f }, { 0.15f, 0.85f }
			};
			StrokePolyline(dl, c, kShell, t, color, true);

			dl->AddRect(c.At(0.33f, 0.15f), c.At(0.67f, 0.40f), PaletteColor(color, kViolet), 0.0f, 0, t);
			dl->AddRect(c.At(0.28f, 0.55f), c.At(0.72f, 0.85f), color, c.Len(0.025f), 0, t);
			dl->AddLine(c.At(0.37f, 0.72f), c.At(0.63f, 0.72f), PaletteColor(color, kMint), t);
		}

		void DrawImport(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 文档 + 向下导入箭头 + 接收托盘，避免与打开场景的文件夹混淆 */
			static constexpr float kPage[5][2] = {
				{ 0.20f, 0.08f }, { 0.56f, 0.08f }, { 0.73f, 0.25f }, { 0.73f, 0.50f }, { 0.20f, 0.50f }
			};
			StrokePolyline(dl, c, kPage, t, color, true);
			dl->AddLine(c.At(0.56f, 0.08f), c.At(0.56f, 0.25f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.56f, 0.25f), c.At(0.73f, 0.25f), PaletteColor(color, kViolet), t);

			dl->AddLine(c.At(0.50f, 0.34f), c.At(0.50f, 0.76f), PaletteColor(color, kViolet), t);
			dl->PathLineTo(c.At(0.36f, 0.62f));
			dl->PathLineTo(c.At(0.50f, 0.76f));
			dl->PathLineTo(c.At(0.64f, 0.62f));
			dl->PathStroke(PaletteColor(color, kViolet), 0, t);

			dl->AddLine(c.At(0.22f, 0.84f), c.At(0.78f, 0.84f), color, t);
			dl->AddLine(c.At(0.22f, 0.84f), c.At(0.22f, 0.73f), color, t);
			dl->AddCircleFilled(c.At(0.22f, 0.73f), c.Len(0.055f), PaletteColor(color, kMint));
		}

		/* 导出：导入图标的镜像 —— 承载托盘 + 向上的导出箭头（"数据送出去"，
		 * 与导入的"文档 + 向下箭头"同族）。几何与 SVG symbol 同一副坐标。 */
		void DrawExport(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddLine(c.At(0.50f, 0.66f), c.At(0.50f, 0.24f), PaletteColor(color, kViolet), t);
			dl->PathLineTo(c.At(0.36f, 0.38f));
			dl->PathLineTo(c.At(0.50f, 0.24f));
			dl->PathLineTo(c.At(0.64f, 0.38f));
			dl->PathStroke(PaletteColor(color, kViolet), 0, t);

			dl->AddLine(c.At(0.22f, 0.84f), c.At(0.78f, 0.84f), color, t);
			dl->AddLine(c.At(0.22f, 0.84f), c.At(0.22f, 0.73f), color, t);
			dl->AddCircleFilled(c.At(0.22f, 0.73f), c.Len(0.055f), PaletteColor(color, kMint));
		}

		/* 新建资源：一枚加号、不加外框，用"动作"强调色（跟 Add / Return / Filter 同源）。跟通用的
		 * 「Add」（圆圈加号）刻意分开：这里是"往当前目录建资源"、组件那个是"带圈加号"。笔画略重
		 * （2.6 / 32），因为它是整枚图标唯一的内容。 */
		void DrawNewAsset(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			constexpr float kArm = 9.0f / 32.0f;                       /* 臂长 9 / 32（与 SVG 同一副几何） */
			const float t = std::max(1.25f, size * (2.6f / 32.0f));    /* 线宽 2.6 / 32 */
			const ImU32 violet = PaletteColor(color, kViolet);

			dl->AddLine(c.At(0.50f, 0.50f - kArm), c.At(0.50f, 0.50f + kArm), violet, t);
			dl->AddLine(c.At(0.50f - kArm, 0.50f), c.At(0.50f + kArm, 0.50f), violet, t);
		}

		/* ---- 编辑历史 ----
		 * 半圆弧 + 尾线 + 实心箭头；mirror = 左右镜像（重做）。注意：镜像要对整条路径统一做 —— 弧的
		 * 点在同一套归一化坐标里逐点算出、统一过 at() 变换；只翻坐标或对调起止角会把弧翻到下方
		 * （严格左右对称、跟扫描方向无关）。 */
		void DrawHistoryArrow(ImDrawList* dl, const Canvas& c, float t, ImU32 color, bool mirror)
		{
			const float flip = mirror ? -1.0f : 1.0f;
			/* 归一化坐标先镜像、再交给 Canvas —— 所有子图元共用这一个入口，
			 * 不存在"某一段忘了镜像"的可能 */
			const auto at = [&](float x, float y) { return c.At(0.5f + flip * (x - 0.5f), y); };

			/* 弧：左端点 → 顶上 → 右端点（角度 π 起、扫 π，经 1.5π = 屏幕上方） */
			constexpr float kCenterX = 0.50f;
			constexpr float kCenterY = 0.58f;
			constexpr float kRadius = 0.27f;
			constexpr int kSegments = 24;

			for (int i = 0; i <= kSegments; ++i)
			{
				const float angle = kPi + kPi * (static_cast<float>(i) / kSegments);
				dl->PathLineTo(at(kCenterX + std::cos(angle) * kRadius,
					kCenterY + std::sin(angle) * kRadius));
			}

			/* 尾线：接在弧的右端点，向下收住 */
			dl->PathLineTo(at(0.77f, 0.86f));
			dl->PathStroke(color, 0, t);

			/* 箭头：贴在弧的左端点，指向正下方（即弧在该点的切线反向） */
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

		/* ---- 路径导航 ----
		 * 回到上次路径 / 重进路径：平直的方向箭头（尾线 + 折角箭头），跟撤销 / 重做的弧线刻意分开
		 * （那个管编辑历史，这里管浏览位置）。镜像跟 DrawHistoryArrow 同一套做法（归一化坐标先镜像）。 */
		void DrawNavArrow(ImDrawList* dl, const Canvas& c, float t, ImU32 color, bool mirror)
		{
			const float flip = mirror ? -1.0f : 1.0f;
			const auto at = [&](float x, float y) { return c.At(0.5f + flip * (x - 0.5f), y); };

			/* 尾线：从远端指到箭头折点 */
			dl->AddLine(at(0.81f, 0.50f), at(0.20f, 0.50f), color, t);

			/* 箭头的两条臂 */
			dl->PathLineTo(at(0.44f, 0.27f));
			dl->PathLineTo(at(0.20f, 0.50f));
			dl->PathLineTo(at(0.44f, 0.73f));
			dl->PathStroke(color, 0, t);
		}

		void DrawBack(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			DrawNavArrow(dl, Canvas{ center, size }, StrokeWidth(size), color, false);
		}

		void DrawForward(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			DrawNavArrow(dl, Canvas{ center, size }, StrokeWidth(size), color, true);
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
			const float t = StrokeWidth(size);
			const ImVec2 origin = c.At(0.50f, 0.50f);
			const float radius = c.Len(0.30f);
			const ImU32 violet = PaletteColor(color, kViolet);

			/* 两段半圆弧首尾相接：上弧终点向下、下弧终点向上，组成循环旋转 */
			dl->PathArcTo(origin, radius, kPi, 2.0f * kPi, 24);
			dl->PathStroke(violet, 0, t);
			dl->PathArcTo(origin, radius, 0.0f, kPi, 24);
			dl->PathStroke(color, 0, t);

			/* 右端箭头朝下，左端箭头朝上；两个折线尖端都落在半圆弧端点 */
			dl->PathLineTo(c.At(0.70f, 0.40f));
			dl->PathLineTo(c.At(0.80f, 0.50f));
			dl->PathLineTo(c.At(0.90f, 0.40f));
			dl->PathStroke(violet, 0, t);

			dl->PathLineTo(c.At(0.30f, 0.60f));
			dl->PathLineTo(c.At(0.20f, 0.50f));
			dl->PathLineTo(c.At(0.10f, 0.60f));
			dl->PathStroke(color, 0, t);
			dl->AddCircleFilled(origin, c.Len(0.075f), PaletteColor(color, kMint));
		}

		void DrawScale(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 由小到大的两个方框，用角点连线表达「缩放」；大框填一层淡色表示"结果" */
			dl->AddRectFilled(c.At(0.44f, 0.10f), c.At(0.90f, 0.56f), Soften(color, 0.26f));
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

		/* GPU 计时面板的观测方式图标（SVG 为主渲染路径；这里是与 SVG 同构的后备画法） */

		void DrawLive(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 活性脉冲线（实时数据流的通用符号） */
			const ImVec2 points[] = {
				c.At(0.09f, 0.50f), c.At(0.31f, 0.50f), c.At(0.45f, 0.22f),
				c.At(0.64f, 0.78f), c.At(0.78f, 0.50f), c.At(0.92f, 0.50f),
			};
			dl->AddPolyline(points, IM_ARRAYSIZE(points), color, 0, t);
		}

		void DrawRecord(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 录制圆点：细环 + 实心红点（录像机 REC 的通用符号） */
			dl->AddCircle(c.At(0.5f, 0.5f), c.Len(0.43f), color, 0, t);
			dl->AddCircleFilled(c.At(0.5f, 0.5f), c.Len(0.225f), PaletteColor(color, kRed));
		}

		/* 帧格（上一帧 / 下一帧）：胶片格 + 齿孔刻线 + 内容行 + 指向箭头，forward 为镜像 */
		void DrawFrameSlot(ImDrawList* dl, const Canvas& c, float t, ImU32 color, bool forward)
		{
			const float flip = forward ? -1.0f : 1.0f;
			const auto at = [&](float x, float y) { return c.At(0.5f + flip * (x - 0.5f), y); };

			/* 胶片格（圆角矩形，格内上下的齿孔刻线 + 三条内容行） */
			dl->AddRect(at(0.375f, 0.156f), at(0.875f, 0.844f), color, c.Len(0.0625f), 0, t);
			dl->AddLine(at(0.50f, 0.156f), at(0.50f, 0.281f), color, t);
			dl->AddLine(at(0.75f, 0.156f), at(0.75f, 0.281f), color, t);
			dl->AddLine(at(0.50f, 0.719f), at(0.50f, 0.844f), color, t);
			dl->AddLine(at(0.75f, 0.719f), at(0.75f, 0.844f), color, t);
			dl->AddLine(at(0.50f, 0.375f), at(0.75f, 0.375f), color, t);
			dl->AddLine(at(0.50f, 0.500f), at(0.656f, 0.500f), color, t);
			dl->AddLine(at(0.50f, 0.625f), at(0.75f, 0.625f), color, t);

			/* 指向帧格的箭头 */
			dl->AddLine(at(0.344f, 0.500f), at(0.094f, 0.500f), color, t);
			dl->AddLine(at(0.094f, 0.500f), at(0.250f, 0.344f), color, t);
			dl->AddLine(at(0.094f, 0.500f), at(0.250f, 0.656f), color, t);
		}

		void DrawPreviousFrame(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			DrawFrameSlot(dl, Canvas{ center, size }, StrokeWidth(size), color, false);
		}

		void DrawNextFrame(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			DrawFrameSlot(dl, Canvas{ center, size }, StrokeWidth(size), color, true);
		}

		/* 峰值（坐标轴 + 折线升至峰点 + 穿过圆心的十字） */
		void DrawPeak(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddLine(c.At(0.156f, 0.812f), c.At(0.156f, 0.188f), color, t);
			dl->AddLine(c.At(0.156f, 0.812f), c.At(0.844f, 0.812f), color, t);

			const ImVec2 points[] = {
				c.At(0.250f, 0.688f), c.At(0.406f, 0.531f), c.At(0.531f, 0.625f), c.At(0.781f, 0.313f),
			};
			dl->AddPolyline(points, IM_ARRAYSIZE(points), color, 0, t);

			/* 十字画在圆之前：实心圆盖住中段，圆的四周露出等长标记 */
			dl->AddLine(c.At(0.781f, 0.131f), c.At(0.781f, 0.494f), color, t);
			dl->AddLine(c.At(0.600f, 0.313f), c.At(0.963f, 0.313f), color, t);
			dl->AddCircleFilled(c.At(0.781f, 0.313f), c.Len(0.0875f), color);
		}

		/* 谷值（坐标轴 + 折线落至谷点再回升 + 穿过圆心的十字） */
		void DrawValley(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddLine(c.At(0.156f, 0.812f), c.At(0.156f, 0.188f), color, t);
			dl->AddLine(c.At(0.156f, 0.812f), c.At(0.844f, 0.812f), color, t);

			const ImVec2 points[] = {
				c.At(0.250f, 0.344f), c.At(0.438f, 0.688f), c.At(0.625f, 0.500f), c.At(0.781f, 0.281f),
			};
			dl->AddPolyline(points, IM_ARRAYSIZE(points), color, 0, t);

			dl->AddLine(c.At(0.438f, 0.506f), c.At(0.438f, 0.869f), color, t);
			dl->AddLine(c.At(0.256f, 0.688f), c.At(0.619f, 0.688f), color, t);
			dl->AddCircleFilled(c.At(0.438f, 0.688f), c.Len(0.0875f), color);
		}

		/* 追踪文本（CPU 录制选项：录制时同步写 chrome://tracing 时间线 JSON）：
		 * 文件页（文件族语言，折角软紫）+ 波形折线（性能族语言，薄荷） */
		void DrawTraceText(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			static constexpr float kPage[5][2] = {
				{ 0.25f, 0.125f }, { 0.594f, 0.125f }, { 0.75f, 0.281f }, { 0.75f, 0.875f }, { 0.25f, 0.875f }
			};
			dl->AddTriangleFilled(c.At(0.594f, 0.125f), c.At(0.75f, 0.281f), c.At(0.594f, 0.281f),
				PaletteColor(color, kViolet, 0.18f));
			StrokePolyline(dl, c, kPage, t, color, true);
			dl->AddLine(c.At(0.594f, 0.125f), c.At(0.594f, 0.281f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.594f, 0.281f), c.At(0.75f, 0.281f), PaletteColor(color, kViolet), t);

			static constexpr float kWave[7][2] = {
				{ 0.344f, 0.688f }, { 0.419f, 0.688f }, { 0.475f, 0.469f }, { 0.556f, 0.797f },
				{ 0.619f, 0.594f }, { 0.663f, 0.688f }, { 0.688f, 0.688f }
			};
			StrokePolyline(dl, c, kWave, t * 1.1f, PaletteColor(color, kMint), false);
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

			/* 圆形操作符号让「新增」从静态十字装饰中脱离出来 */
			dl->AddCircle(c.At(0.50f, 0.50f), c.Len(0.36f), color, 0, t);
			dl->AddLine(c.At(0.50f, 0.30f), c.At(0.50f, 0.70f), color, t);
			dl->AddLine(c.At(0.30f, 0.50f), c.At(0.70f, 0.50f), color, t);
		}

		void DrawRemove(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddCircle(c.At(0.50f, 0.50f), c.Len(0.36f), color, 0, t);
			dl->AddLine(c.At(0.30f, 0.50f), c.At(0.70f, 0.50f), color, t);
		}

		/* 删除垃圾桶：红盖 + 桶身 + 两条竖线（与 Remove 的减号区分） */
		void DrawDelete(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 red = PaletteColor(color, kRed);

			dl->AddLine(c.At(0.18f, 0.27f), c.At(0.82f, 0.27f), red, t);
			dl->AddLine(c.At(0.38f, 0.17f), c.At(0.62f, 0.17f), red, t);
			dl->AddRectFilled(c.At(0.25f, 0.31f), c.At(0.75f, 0.85f), Soften(color, 0.16f), c.Len(0.05f));
			dl->AddRect(c.At(0.25f, 0.31f), c.At(0.75f, 0.85f), red, c.Len(0.05f), 0, t);
			dl->AddLine(c.At(0.41f, 0.43f), c.At(0.41f, 0.73f), color, t);
			dl->AddLine(c.At(0.59f, 0.43f), c.At(0.59f, 0.73f), color, t);
		}

		void DrawReturn(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);

			/* 折返路径：顶栏（带左向箭头）绕右侧下行、沿底栏折回；与 SVG symbol 同一副坐标 */
			dl->PathLineTo(c.At(22.0f / 32.0f, 7.0f / 32.0f));
			dl->PathBezierQuadraticCurveTo(c.At(26.0f / 32.0f, 7.0f / 32.0f), c.At(26.0f / 32.0f, 11.0f / 32.0f), 8);
			dl->PathLineTo(c.At(26.0f / 32.0f, 18.0f / 32.0f));
			dl->PathBezierQuadraticCurveTo(c.At(26.0f / 32.0f, 23.0f / 32.0f), c.At(21.0f / 32.0f, 23.0f / 32.0f), 8);
			dl->PathLineTo(c.At(6.0f / 32.0f, 23.0f / 32.0f));
			dl->PathStroke(color, 0, t);

			dl->PathLineTo(c.At(10.0f / 32.0f, 3.0f / 32.0f));
			dl->PathLineTo(c.At(6.0f / 32.0f, 7.0f / 32.0f));
			dl->PathLineTo(c.At(10.0f / 32.0f, 11.0f / 32.0f));
			dl->PathStroke(violet, 0, t);

			dl->AddLine(c.At(9.0f / 32.0f, 7.0f / 32.0f), c.At(22.0f / 32.0f, 7.0f / 32.0f),
				PaletteColor(color, kMint), t);
		}

		/* 一键回根（Treemap 面包屑：跳过逐层返回、直接回根）：浏览器"主页"式房子 ——
		 * 屋顶折线 + 房身（软紫底 + 描边）+ 门（薄荷），与 Return 的折返箭头明显区分 */
		void DrawReturnRoot(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 房身 */
			dl->AddRectFilled(c.At(0.24f, 0.46f), c.At(0.76f, 0.84f),
				PaletteColor(color, kViolet, 0.16f), c.Len(0.05f));
			dl->AddRect(c.At(0.24f, 0.46f), c.At(0.76f, 0.84f), color, c.Len(0.05f), 0, t);

			/* 屋顶折线（左右屋檐探出房身） */
			dl->AddLine(c.At(0.13f, 0.51f), c.At(0.50f, 0.16f), color, t);
			dl->AddLine(c.At(0.50f, 0.16f), c.At(0.87f, 0.51f), color, t);

			/* 门（薄荷） */
			dl->AddRectFilled(c.At(0.43f, 0.62f), c.At(0.57f, 0.84f),
				PaletteColor(color, kMint), c.Len(0.02f));
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

		/* 渲染层（LayerMask）：三层堆叠 + 中间一层点亮并打勾 —— 与 SVG 同构 */
		void DrawLayerMask(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			for (int layer = 0; layer < 3; ++layer)
			{
				const float top = 0.16f + static_cast<float>(layer) * 0.24f;
				const bool active = (layer == 1);
				if (active)
					dl->AddRectFilled(c.At(0.14f, top), c.At(0.86f, top + 0.18f),
						PaletteColor(color, kMint, 0.14f), c.Len(0.045f));
				dl->AddRect(c.At(0.14f, top), c.At(0.86f, top + 0.18f),
					active ? PaletteColor(color, kMint) : color, c.Len(0.045f), 0, t);
			}

			/* 中间层的勾 */
			const ImU32 check = PaletteColor(color, kMint);
			dl->AddLine(c.At(0.54f, 0.47f), c.At(0.60f, 0.53f), check, t);
			dl->AddLine(c.At(0.60f, 0.53f), c.At(0.73f, 0.40f), check, t);
		}

		/* 脚本组件：圆角页（折角）+ 代码符号 —— 与 FileCppSource 同一枚记号，
		 * 圆角页是组件、折角页是磁盘文件。几何与 ScriptComponent.svg 同一副数。 */
		void DrawScriptComponent(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);
			const ImU32 mint = PaletteColor(color, kMint);

			dl->AddRectFilled(c.At(0.188f, 0.125f), c.At(0.781f, 0.875f),
				PaletteColor(color, kViolet, 0.18f), c.Len(0.0625f));
			dl->AddRect(c.At(0.188f, 0.125f), c.At(0.781f, 0.875f), color, c.Len(0.0625f), 0, t);
			dl->AddLine(c.At(0.625f, 0.125f), c.At(0.625f, 0.281f), violet, t);
			dl->AddLine(c.At(0.625f, 0.281f), c.At(0.781f, 0.281f), violet, t);

			/* 尖括号 + 斜杠 */
			dl->AddLine(c.At(0.406f, 0.406f), c.At(0.313f, 0.500f), mint, t);
			dl->AddLine(c.At(0.313f, 0.500f), c.At(0.406f, 0.594f), mint, t);
			dl->AddLine(c.At(0.563f, 0.406f), c.At(0.656f, 0.500f), mint, t);
			dl->AddLine(c.At(0.656f, 0.500f), c.At(0.563f, 0.594f), mint, t);
			dl->AddLine(c.At(0.531f, 0.375f), c.At(0.469f, 0.625f), mint, t);
		}

		void DrawSearch(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddCircle(c.At(0.42f, 0.42f), c.Len(0.26f), color, 0, t);
			dl->AddLine(c.At(0.61f, 0.61f), c.At(0.88f, 0.88f), color, t);
		}

		void DrawVisible(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->PathLineTo(c.At(0.08f, 0.50f));
			dl->PathBezierCubicCurveTo(c.At(0.30f, 0.16f), c.At(0.70f, 0.16f), c.At(0.92f, 0.50f), 24);
			dl->PathBezierCubicCurveTo(c.At(0.70f, 0.84f), c.At(0.30f, 0.84f), c.At(0.08f, 0.50f), 24);
			dl->PathStroke(color, 0, t);

			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.19f), Soften(color, 0.26f));
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.105f), color);
		}

		/* 隐藏（眼睛被划掉）：与 Visible 同一只眼睛、去掉瞳点，再压一道斜杠 ——
		 * 造型差异（有无斜杠）比颜色差异在 14px 行高下更稳，也更容易一眼读出。 */
		void DrawHidden(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->PathLineTo(c.At(0.08f, 0.50f));
			dl->PathBezierCubicCurveTo(c.At(0.30f, 0.16f), c.At(0.70f, 0.16f), c.At(0.92f, 0.50f), 24);
			dl->PathBezierCubicCurveTo(c.At(0.70f, 0.84f), c.At(0.30f, 0.84f), c.At(0.08f, 0.50f), 24);
			dl->PathStroke(color, 0, t);

			dl->AddLine(c.At(0.22f, 0.82f), c.At(0.78f, 0.18f), PaletteColor(color, kViolet), t);
		}

		/* ---- 场景树节点 ----
		 * "场景 / 实体类型"图标，用在层级树节点前面；只描必要的外形、细部靠实心小图元点缀，
		 * 正文行高（约 14px）下也能一眼区分。 */

		void DrawScene(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);
			const ImU32 mint = PaletteColor(color, kMint);

			/* 根节点向下分出三个子对象：用圆节点和连线表达场景层级 */
			dl->AddLine(c.At(0.50f, 0.30f), c.At(0.50f, 0.50f), violet, t);
			dl->AddLine(c.At(0.22f, 0.50f), c.At(0.78f, 0.50f), violet, t);
			dl->AddLine(c.At(0.22f, 0.50f), c.At(0.22f, 0.70f), violet, t);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.50f, 0.70f), violet, t);
			dl->AddLine(c.At(0.78f, 0.50f), c.At(0.78f, 0.70f), violet, t);

			const float root_radius = c.Len(0.125f);
			const float child_radius = c.Len(0.095f);
			dl->AddCircleFilled(c.At(0.50f, 0.22f), root_radius, PaletteColor(color, kMint, 0.20f));
			dl->AddCircle(c.At(0.50f, 0.22f), root_radius, mint, 0, t);
			dl->AddCircleFilled(c.At(0.22f, 0.78f), child_radius, PaletteColor(color, kViolet, 0.16f));
			dl->AddCircle(c.At(0.22f, 0.78f), child_radius, violet, 0, t);
			dl->AddCircleFilled(c.At(0.50f, 0.78f), child_radius, PaletteColor(color, kViolet, 0.16f));
			dl->AddCircle(c.At(0.50f, 0.78f), child_radius, violet, 0, t);
			dl->AddCircleFilled(c.At(0.78f, 0.78f), child_radius, PaletteColor(color, kViolet, 0.16f));
			dl->AddCircle(c.At(0.78f, 0.78f), child_radius, violet, 0, t);
			dl->AddCircleFilled(c.At(0.50f, 0.22f), c.Len(0.035f), mint);
		}

		void DrawCamera(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const float lens_radius = c.Len(0.22f);

			/* 相机机身、取景器凸起与镜头；镜头中心用薄荷绿聚焦点强调 */
			dl->AddRectFilled(c.At(0.08f, 0.28f), c.At(0.92f, 0.82f), PaletteColor(color, kViolet, 0.12f), c.Len(0.12f));
			dl->AddRect(c.At(0.08f, 0.28f), c.At(0.92f, 0.82f), color, c.Len(0.12f), 0, t);
			dl->AddRect(c.At(0.23f, 0.16f), c.At(0.50f, 0.29f), PaletteColor(color, kViolet), c.Len(0.045f), 0, t);
			dl->AddCircleFilled(c.At(0.57f, 0.55f), lens_radius, PaletteColor(color, kViolet, 0.22f));
			dl->AddCircle(c.At(0.57f, 0.55f), lens_radius, PaletteColor(color, kViolet), 0, t);
			dl->AddCircleFilled(c.At(0.57f, 0.55f), c.Len(0.075f), PaletteColor(color, kMint));
		}

		void DrawDirectionalLight(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 太阳 + 三束等长等距的平行光：区分于点光（四向短射线）、聚光（锥形）。
			 * 三束用同一个方向 + 垂直偏移量算出来，才不会像随手画的三条斜线。 */
			const ImVec2 sun = c.At(0.30f, 0.28f);
			dl->AddCircleFilled(sun, c.Len(0.16f), PaletteColor(color, kViolet, 0.18f));
			dl->AddCircle(sun, c.Len(0.16f), color, 0, t);
			dl->AddCircleFilled(sun, c.Len(0.045f), PaletteColor(color, kMint));

			const float along_x = 0.707f;
			const float along_y = 0.707f;
			const float offset_x = -0.707f;
			const float offset_y = 0.707f;

			for (const float spread : { -0.15f, 0.0f, 0.15f })
			{
				const float start_x = 0.30f + along_x * 0.30f + offset_x * spread;
				const float start_y = 0.28f + along_y * 0.30f + offset_y * spread;
				dl->AddLine(c.At(start_x, start_y),
					c.At(start_x + along_x * 0.26f, start_y + along_y * 0.26f), color, t);
			}
		}

		void DrawPointLight(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 发光球 + 四向短射线 */
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.22f), PaletteColor(color, kViolet, 0.18f));
			dl->AddCircle(c.At(0.50f, 0.50f), c.Len(0.22f), color, 0, t);
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.075f), PaletteColor(color, kMint));

			dl->AddLine(c.At(0.50f, 0.04f), c.At(0.50f, 0.18f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.50f, 0.82f), c.At(0.50f, 0.96f), color, t);
			dl->AddLine(c.At(0.04f, 0.50f), c.At(0.18f, 0.50f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.82f, 0.50f), c.At(0.96f, 0.50f), color, t);
		}

		void DrawSpotLight(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 灯源 + 向下张开的光锥（不封底，才读得出是"投出去的光"） */
			dl->AddCircleFilled(c.At(0.50f, 0.16f), c.Len(0.11f), PaletteColor(color, kMint));
			dl->AddLine(c.At(0.40f, 0.32f), c.At(0.14f, 0.92f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.60f, 0.32f), c.At(0.86f, 0.92f), PaletteColor(color, kViolet), t);
		}

		void DrawLight(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImVec2 bulb = c.At(0.50f, 0.42f);

			/* 灯泡轮廓 + 灯座；淡紫灯罩与薄荷色发光核心 */
			dl->AddCircleFilled(bulb, c.Len(0.25f), PaletteColor(color, kViolet, 0.18f));
			dl->AddCircle(bulb, c.Len(0.25f), color, 0, t);
			dl->AddLine(c.At(0.37f, 0.57f), c.At(0.42f, 0.69f), color, t);
			dl->AddLine(c.At(0.63f, 0.57f), c.At(0.58f, 0.69f), color, t);
			dl->AddLine(c.At(0.42f, 0.69f), c.At(0.58f, 0.69f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.43f, 0.77f), c.At(0.57f, 0.77f), color, t);
			dl->AddLine(c.At(0.45f, 0.84f), c.At(0.55f, 0.84f), color, t);
			dl->AddCircleFilled(bulb, c.Len(0.065f), PaletteColor(color, kMint));

			dl->AddLine(c.At(0.50f, 0.04f), c.At(0.50f, 0.11f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.13f, 0.42f), c.At(0.20f, 0.42f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.80f, 0.42f), c.At(0.87f, 0.42f), PaletteColor(color, kViolet), t);
		}

		void DrawAudio(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 扬声器主体填淡紫，声波用主线色，中心用薄荷色作识别点 */
			static constexpr float kSpeaker[6][2] = {
				{ 0.08f, 0.40f }, { 0.30f, 0.40f }, { 0.60f, 0.18f },
				{ 0.60f, 0.82f }, { 0.30f, 0.60f }, { 0.08f, 0.60f }
			};
			FillPoly(dl, c, kSpeaker, PaletteColor(color, kViolet, 0.18f));
			StrokePolyline(dl, c, kSpeaker, t, PaletteColor(color, kViolet), true);
			dl->PathArcTo(c.At(0.57f, 0.50f), c.Len(0.20f), -0.82f, 0.82f, 18);
			dl->PathStroke(color, 0, t);
			dl->PathArcTo(c.At(0.57f, 0.50f), c.Len(0.34f), -0.82f, 0.82f, 18);
			dl->PathStroke(color, 0, t);
			dl->AddCircleFilled(c.At(0.30f, 0.50f), c.Len(0.055f), PaletteColor(color, kMint));
		}

		void DrawParticle(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 大小不同的星形发射点与粒子点，保持轮廓清晰、不画成统计柱 */
			static constexpr float kBurst[8][2] = {
				{ 0.50f, 0.08f }, { 0.56f, 0.40f }, { 0.84f, 0.50f }, { 0.56f, 0.58f },
				{ 0.50f, 0.90f }, { 0.43f, 0.58f }, { 0.16f, 0.50f }, { 0.43f, 0.40f }
			};
			StrokePolyline(dl, c, kBurst, t, PaletteColor(color, kViolet), true);
			dl->AddCircleFilled(c.At(0.17f, 0.20f), c.Len(0.06f), PaletteColor(color, kMint));
			dl->AddCircleFilled(c.At(0.83f, 0.23f), c.Len(0.045f), PaletteColor(color, kMint));
			dl->AddCircleFilled(c.At(0.78f, 0.80f), c.Len(0.07f), color);
			dl->AddCircleFilled(c.At(0.20f, 0.81f), c.Len(0.04f), PaletteColor(color, kViolet));
		}

		void DrawTerrain(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 山脊剪影 + 两道地层曲线：与场景地面网格、图片缩略图区分 */
			static constexpr float kHill[7][2] = {
				{ 0.08f, 0.76f }, { 0.27f, 0.43f }, { 0.41f, 0.60f },
				{ 0.58f, 0.28f }, { 0.91f, 0.76f }, { 0.91f, 0.84f }, { 0.08f, 0.84f }
			};
			dl->AddTriangleFilled(c.At(0.27f, 0.76f), c.At(0.58f, 0.28f), c.At(0.91f, 0.76f),
				PaletteColor(color, kViolet, 0.14f));
			dl->AddTriangleFilled(c.At(0.08f, 0.76f), c.At(0.27f, 0.43f), c.At(0.41f, 0.60f),
				PaletteColor(color, kMint, 0.10f));
			StrokePolyline(dl, c, kHill, t, PaletteColor(color, kViolet), true);
			dl->AddLine(c.At(0.29f, 0.48f), c.At(0.40f, 0.58f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.60f, 0.34f), c.At(0.73f, 0.49f), PaletteColor(color, kMint), t);

			dl->PathLineTo(c.At(0.10f, 0.91f));
			dl->PathBezierCubicCurveTo(c.At(0.34f, 0.82f), c.At(0.66f, 0.98f), c.At(0.90f, 0.89f), 12);
			dl->PathStroke(color, 0, t);
		}

		void DrawReflectionProbe(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 球体 + 左上高光弧 + 反射亮点 */
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.40f), PaletteColor(color, kViolet, 0.12f));
			dl->AddCircle(c.At(0.50f, 0.50f), c.Len(0.40f), color, 0, t);

			dl->PathArcTo(c.At(0.50f, 0.50f), c.Len(0.22f), 3.34f, 4.56f, 16);
			dl->PathStroke(PaletteColor(color, kViolet), 0, t);

			dl->AddCircleFilled(c.At(0.66f, 0.66f), c.Len(0.08f), PaletteColor(color, kMint));
		}

		void DrawModel(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 等轴测线框立方体：六边形轮廓 + 中心三条棱；顶面填一层淡色，方块才有体积感 */
			static constexpr float kCube[6][2] = {
				{ 0.50f, 0.08f }, { 0.92f, 0.30f }, { 0.92f, 0.70f },
				{ 0.50f, 0.92f }, { 0.08f, 0.70f }, { 0.08f, 0.30f }
			};
			static constexpr float kTop[4][2] = {
				{ 0.50f, 0.08f }, { 0.92f, 0.30f }, { 0.50f, 0.52f }, { 0.08f, 0.30f }
			};
			FillPoly(dl, c, kTop, PaletteColor(color, kViolet, 0.16f));
			StrokePolyline(dl, c, kCube, t, color, true);

			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.50f, 0.08f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.92f, 0.70f), color, t);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.08f, 0.70f), color, t);
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.045f), PaletteColor(color, kMint));
		}

		/* ---- 默认 3D 形状（新建菜单 / 场景树）----
		 * 与 Model 的"线框盒 + 中心点"刻意分开：形状图标是"实心几何体"——
		 * 盒子的两个可见面带填色、不带中心点；球与平面各用自己的剪影语言。 */

		void DrawCubeShape(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 左面淡紫、顶面薄荷（右面留白）：两块带色的面给出实心体积感 */
			static constexpr float kTop[4][2] = {
				{ 0.50f, 0.08f }, { 0.92f, 0.30f }, { 0.50f, 0.52f }, { 0.08f, 0.30f }
			};
			static constexpr float kLeft[4][2] = {
				{ 0.08f, 0.30f }, { 0.50f, 0.52f }, { 0.50f, 0.92f }, { 0.08f, 0.70f }
			};
			FillPoly(dl, c, kLeft, PaletteColor(color, kViolet, 0.20f));
			FillPoly(dl, c, kTop, PaletteColor(color, kMint, 0.24f));

			static constexpr float kShell[6][2] = {
				{ 0.50f, 0.08f }, { 0.92f, 0.30f }, { 0.92f, 0.70f },
				{ 0.50f, 0.92f }, { 0.08f, 0.70f }, { 0.08f, 0.30f }
			};
			StrokePolyline(dl, c, kShell, t, color, true);
			dl->AddLine(c.At(0.50f, 0.52f), c.At(0.08f, 0.30f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.50f, 0.52f), c.At(0.92f, 0.30f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.50f, 0.52f), c.At(0.50f, 0.92f), PaletteColor(color, kViolet), t);
		}

		void DrawSphereShape(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 圆面 + 经线（竖椭圆）+ 纬线（横弦）：一眼是球，不是"光圈 / 灯泡" */
			const ImVec2 sphere_center = c.At(0.50f, 0.50f);
			dl->AddCircleFilled(sphere_center, c.Len(0.42f), PaletteColor(color, kViolet, 0.16f));

			constexpr int kMeridianSegments = 28;
			for (int i = 0; i <= kMeridianSegments; ++i)
			{
				const float angle = 2.0f * kPi * static_cast<float>(i) / kMeridianSegments;
				dl->PathLineTo(c.At(0.50f + std::cos(angle) * 0.16f, 0.50f + std::sin(angle) * 0.42f));
			}
			dl->PathStroke(PaletteColor(color, kViolet), 0, t);

			dl->AddLine(c.At(0.08f, 0.50f), c.At(0.92f, 0.50f), PaletteColor(color, kViolet), t);
			dl->AddCircle(sphere_center, c.Len(0.42f), color, 0, t);
		}

		void DrawPlaneShape(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 等轴测四边形（地砖）+ 中缝十字：用"摊平的菱形"表达零厚度的平面 */
			static constexpr float kQuad[4][2] = {
				{ 0.50f, 0.16f }, { 0.92f, 0.50f }, { 0.50f, 0.84f }, { 0.08f, 0.50f }
			};
			FillPoly(dl, c, kQuad, PaletteColor(color, kMint, 0.18f));
			StrokePolyline(dl, c, kQuad, t, color, true);
			dl->AddLine(c.At(0.29f, 0.33f), c.At(0.71f, 0.67f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.71f, 0.33f), c.At(0.29f, 0.67f), PaletteColor(color, kViolet), t);
		}

		/* 「3D」分组行（新建菜单）：线框小立方体 + 右下角一颗小球 —— "多种 3D 形状"
		 * 的集合语义；与 Cube（实心大块）、Model（带中心点）刻意分开。 */
		void DrawShape3D(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 小球先画（占右下角，不压几何） */
			dl->AddCircleFilled(c.At(0.76f, 0.76f), c.Len(0.18f), PaletteColor(color, kViolet, 0.14f));
			dl->AddCircle(c.At(0.76f, 0.76f), c.Len(0.18f), PaletteColor(color, kViolet), 0, t);

			/* 线框立方体（左上）：轮廓 + 三条中棱；左面垫一层淡色给出体积 */
			static constexpr float kShell[6][2] = {
				{ 0.37f, 0.04f }, { 0.70f, 0.205f }, { 0.70f, 0.535f },
				{ 0.37f, 0.70f }, { 0.04f, 0.535f }, { 0.04f, 0.205f }
			};
			static constexpr float kLeft[4][2] = {
				{ 0.04f, 0.205f }, { 0.37f, 0.37f }, { 0.37f, 0.70f }, { 0.04f, 0.535f }
			};
			FillPoly(dl, c, kLeft, PaletteColor(color, kViolet, 0.16f));
			StrokePolyline(dl, c, kShell, t, color, true);

			dl->AddLine(c.At(0.37f, 0.37f), c.At(0.37f, 0.70f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.37f, 0.37f), c.At(0.70f, 0.535f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.37f, 0.37f), c.At(0.04f, 0.535f), PaletteColor(color, kViolet), t);
		}

		void DrawSprite(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 画框 + 山 + 太阳：图片图标的通用语言 */
			dl->AddRect(c.At(0.08f, 0.16f), c.At(0.92f, 0.84f), color, c.Len(0.08f), 0, t);

			dl->AddTriangleFilled(c.At(0.18f, 0.74f), c.At(0.46f, 0.40f), c.At(0.72f, 0.74f), PaletteColor(color, kViolet, 0.85f));
			dl->AddCircleFilled(c.At(0.70f, 0.34f), c.Len(0.08f), PaletteColor(color, kMint));
		}

		void DrawEntity(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImVec2 origin = c.At(0.48f, 0.56f);
			const ImU32 violet = PaletteColor(color, kViolet);

			/* 空物体：中心锚点伸出平面轴与一条斜向轴，表达可被变换的原点 */
			dl->AddLine(origin, c.At(0.90f, 0.56f), color, t);
			dl->AddLine(origin, c.At(0.48f, 0.13f), color, t);
			dl->AddLine(origin, c.At(0.18f, 0.25f), violet, t);
			dl->AddLine(origin, c.At(0.48f, 0.94f), color, t);
			dl->AddLine(origin, c.At(0.08f, 0.56f), color, t);

			static constexpr float kAxisX[3][2] = { { 0.78f, 0.47f }, { 0.90f, 0.56f }, { 0.78f, 0.65f } };
			static constexpr float kAxisY[3][2] = { { 0.40f, 0.28f }, { 0.48f, 0.13f }, { 0.56f, 0.28f } };
			static constexpr float kAxisZ[3][2] = { { 0.16f, 0.38f }, { 0.18f, 0.25f }, { 0.31f, 0.28f } };
			FillPoly(dl, c, kAxisX, color);
			FillPoly(dl, c, kAxisY, color);
			FillPoly(dl, c, kAxisZ, violet);
			dl->AddCircleFilled(origin, c.Len(0.075f), PaletteColor(color, kMint));
		}

		/* ---- 组件卡头部 ----
		 * 与"实体类型"图标分开：这两枚表达的是「组件」本身，
		 * 与场景树里按实体持有的可视内容判定类型的图标不是同一套概念。 */

		void DrawTransform(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 三轴变换 gizmo：从同一原点伸出 X（右）/ Y（上）/ Z（斜向），
			 * 与「移动」（四向对称箭头）在造型上就不会混淆。 */
			const ImVec2 origin = c.At(0.26f, 0.76f);
			static constexpr float kTips[3][2] = {
				{ 0.94f, 0.76f }, { 0.26f, 0.08f }, { 0.78f, 0.28f }
			};

			for (int axis = 0; axis < 3; ++axis)
			{
				const ImVec2 tip = c.At(kTips[axis][0], kTips[axis][1]);
				dl->AddLine(origin, tip, color, t);

				const ImVec2 dir(tip.x - origin.x, tip.y - origin.y);
				const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y);
				if (length <= 0.0f)
					continue;

				/* 沿轴方向画一个实心箭头 */
				const ImVec2 along(dir.x / length, dir.y / length);
				const ImVec2 normal(-along.y, along.x);
				const float head = c.Len(0.17f);
				const float half = c.Len(0.105f);

				dl->AddTriangleFilled(tip,
					ImVec2(tip.x - along.x * head + normal.x * half, tip.y - along.y * head + normal.y * half),
					ImVec2(tip.x - along.x * head - normal.x * half, tip.y - along.y * head - normal.y * half),
					color);
			}
		}

		void DrawTag(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			/* 标签牌：左端平、右端收成尖角，左端一个孔 —— 名字/标识的通用造型。
			 * 牌面填一层淡色，比空心折线更像一张"实物"。 */
			static constexpr float kTag[5][2] = {
				{ 0.10f, 0.24f }, { 0.62f, 0.24f }, { 0.92f, 0.50f }, { 0.62f, 0.76f }, { 0.10f, 0.76f }
			};
			FillPoly(dl, c, kTag, PaletteColor(color, kViolet, 0.16f));
			StrokePolyline(dl, c, kTag, StrokeWidth(size), color, true);

			dl->AddCircleFilled(c.At(0.31f, 0.50f), c.Len(0.095f), PaletteColor(color, kMint));
		}

		void DrawStats(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 基线 + 三根高低不同的柱子：一眼读出"统计/数据" */
			dl->AddLine(c.At(0.10f, 0.88f), c.At(0.90f, 0.88f), color, t);

			const float radius = c.Len(0.04f);
			dl->AddRectFilled(c.At(0.18f, 0.60f), c.At(0.36f, 0.88f), color, radius);
			dl->AddRectFilled(c.At(0.41f, 0.28f), c.At(0.59f, 0.88f), color, radius);
			dl->AddRectFilled(c.At(0.64f, 0.46f), c.At(0.82f, 0.88f), color, radius);
		}

		void DrawFrameGraph(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const float radius = c.Len(0.055f);

			/* 依赖图：左节点汇入右侧上下两个节点 —— 三个节点框 + 两条汇聚线 */
			const ImVec2 left_min = c.At(0.05f, 0.36f);
			const ImVec2 left_max = c.At(0.44f, 0.64f);
			const ImVec2 top_min = c.At(0.56f, 0.14f);
			const ImVec2 top_max = c.At(0.95f, 0.44f);
			const ImVec2 bottom_min = c.At(0.56f, 0.56f);
			const ImVec2 bottom_max = c.At(0.95f, 0.86f);

			dl->AddRectFilled(left_min, left_max, PaletteColor(color, kViolet, 0.20f), radius);
			dl->AddRect(left_min, left_max, color, radius, 0, t);
			dl->AddRect(top_min, top_max, color, radius, 0, t);
			dl->AddRect(bottom_min, bottom_max, color, radius, 0, t);

			dl->AddLine(c.At(0.44f, 0.50f), c.At(0.50f, 0.50f), color, t);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.56f, 0.29f), color, t);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.56f, 0.71f), color, t);
		}

		/* 日志窗口（Log）：圆角面板 + 命令提示符 + 两行日志 —— "终端的日志输出"，
		 * 与 FrameGraph 同属"面板类"图形（圆角框语言） */
		void DrawLog(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const float radius = c.Len(0.06f);

			dl->AddRect(c.At(0.06f, 0.09f), c.At(0.94f, 0.91f), color, radius, 0, t);

			/* 提示符：左端小折线（软紫） */
			dl->AddLine(c.At(0.19f, 0.30f), c.At(0.28f, 0.41f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.28f, 0.41f), c.At(0.19f, 0.52f), PaletteColor(color, kViolet), t);

			/* 日志行：提示行（薄荷）+ 正文行 */
			dl->AddLine(c.At(0.40f, 0.41f), c.At(0.80f, 0.41f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.19f, 0.70f), c.At(0.80f, 0.70f), color, t);
		}

		/* 适配视图（Fit）：四角取景框 + 内容块 —— "把整张图收进这方视野"。
		 * 与 Gizmos 同族（四角框语言），中心从轴点换成内容块
		 * （软紫圆角块 + 薄荷芯点 = 被收进来的"图"）。 */
		void DrawFitView(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);
			const ImU32 mint = PaletteColor(color, kMint);

			/* 四角取景框 */
			dl->AddLine(c.At(0.125f, 0.344f), c.At(0.125f, 0.125f), violet, t * 1.1f);
			dl->AddLine(c.At(0.125f, 0.125f), c.At(0.344f, 0.125f), violet, t * 1.1f);
			dl->AddLine(c.At(0.656f, 0.125f), c.At(0.875f, 0.125f), violet, t * 1.1f);
			dl->AddLine(c.At(0.875f, 0.125f), c.At(0.875f, 0.344f), violet, t * 1.1f);
			dl->AddLine(c.At(0.875f, 0.656f), c.At(0.875f, 0.875f), violet, t * 1.1f);
			dl->AddLine(c.At(0.875f, 0.875f), c.At(0.656f, 0.875f), violet, t * 1.1f);
			dl->AddLine(c.At(0.344f, 0.875f), c.At(0.125f, 0.875f), violet, t * 1.1f);
			dl->AddLine(c.At(0.125f, 0.875f), c.At(0.125f, 0.656f), violet, t * 1.1f);

			/* 内容块：软底 + 墨色描边收口，中心一枚薄荷芯 */
			const ImVec2 block_min = c.At(0.359f, 0.359f);
			const ImVec2 block_max = c.At(0.641f, 0.641f);
			dl->AddRectFilled(block_min, block_max, PaletteColor(color, kViolet, 0.30f), c.Len(0.056f));
			dl->AddRect(block_min, block_max, color, c.Len(0.056f), 0, t);
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.053f), mint);
		}

		/* 预览垂直翻转（Frame Graph 工具行的镜像开关）：水平镜像轴（虚线）上下两块
		 * 互为镜像的三角块 —— 上块紫调（当前朝向）、下块藏青调（翻转后的朝向），
		 * 轴上中心一枚薄荷轴点。 */
		void DrawFlipVertical(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 镜像轴：虚线（"3 3"节奏 —— 小尺寸下短划不糊成点噪；中心一格留空给轴点，
			 * 与 light-point 的外圈虚线同族） */
			constexpr float dash_on = 3.0f / 32.0f;
			constexpr float dash_cycle = 3.0f / 32.0f * 2.0f;
			for (float x = 5.0f / 32.0f; x < 26.0f / 32.0f; x += dash_cycle)
				dl->AddLine(c.At(x, 0.5f), c.At(x + dash_on, 0.5f), PaletteColor(color, kViolet), t);

			static constexpr float kTop[3][2] = {
				{ 9.0f / 32.0f, 5.0f / 32.0f }, { 23.0f / 32.0f, 5.0f / 32.0f }, { 9.0f / 32.0f, 14.5f / 32.0f }
			};
			FillPoly(dl, c, kTop, PaletteColor(color, kViolet, 0.22f));
			StrokePolyline(dl, c, kTop, t, PaletteColor(color, kViolet), true);

			static constexpr float kBottom[3][2] = {
				{ 9.0f / 32.0f, 27.0f / 32.0f }, { 23.0f / 32.0f, 27.0f / 32.0f }, { 9.0f / 32.0f, 17.5f / 32.0f }
			};
			FillPoly(dl, c, kBottom, PaletteColor(color, kMint, 0.20f));
			StrokePolyline(dl, c, kBottom, t, color, true);

			/* 轴点：镜像中心（薄荷芯，落在留空的半格上） */
			dl->AddCircleFilled(c.At(0.5f, 0.5f), c.Len(0.05f), PaletteColor(color, kMint));
		}

		/* 弹窗最大化（CPU 图表弹窗的行尾按钮）：窗块（软紫底 + 标题条）+ 右上角向外撑的
		 * 角标箭头 —— 语汇与 Scale 的四角箭头同族，这里只留右上向外、读作"撑到全窗"。 */
		void DrawMaximize(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 mint = PaletteColor(color, kMint);

			/* 窗块 */
			dl->AddRectFilled(c.At(0.156f, 0.312f), c.At(0.688f, 0.750f), PaletteColor(color, kViolet, 0.16f), c.Len(0.056f));
			dl->AddRect(c.At(0.156f, 0.312f), c.At(0.688f, 0.750f), color, c.Len(0.056f), 0, t);
			dl->AddLine(c.At(0.156f, 0.453f), c.At(0.688f, 0.453f), PaletteColor(color, kViolet), t);

			/* 右上角标箭头：顶点 + 左 / 下两条短边 + 指向窗内的对角线 */
			dl->AddLine(c.At(0.8125f, 0.1875f), c.At(0.6250f, 0.1875f), mint, t * 1.1f);
			dl->AddLine(c.At(0.8125f, 0.1875f), c.At(0.8125f, 0.3750f), mint, t * 1.1f);
			dl->AddLine(c.At(0.8125f, 0.1875f), c.At(0.5938f, 0.4063f), mint, t * 1.1f);
		}

		/* 还原为浮动窗：前下后上两个错层窗块（经典"还原"语汇；后块灰调后退、
		 * 前块软紫底 + 标题条 —— 与 Overdraw 的两块叠放同族但层级感相反）。 */
		void DrawRestore(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddRectFilled(c.At(0.359f, 0.219f), c.At(0.828f, 0.594f), PaletteColor(color, kGray), c.Len(0.056f));
			dl->AddRectFilled(c.At(0.172f, 0.406f), c.At(0.641f, 0.781f), PaletteColor(color, kViolet, 0.16f), c.Len(0.056f));
			dl->AddRect(c.At(0.172f, 0.406f), c.At(0.641f, 0.781f), color, c.Len(0.056f), 0, t);
			dl->AddLine(c.At(0.172f, 0.531f), c.At(0.641f, 0.531f), PaletteColor(color, kViolet), t);
		}

		/* 侧边栏：外框 + 侧栏分隔线 + 指向内容的箭头 */
		void DrawExpandSidebar(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 arrow = PaletteColor(color, kMint);

			dl->AddRect(c.At(0.12f, 0.16f), c.At(0.88f, 0.84f), color, c.Len(0.06f), 0, t);
			dl->AddLine(c.At(0.62f, 0.16f), c.At(0.62f, 0.84f), color, t);
			dl->AddLine(c.At(0.52f, 0.50f), c.At(0.28f, 0.50f), arrow, t);
			dl->AddLine(c.At(0.28f, 0.50f), c.At(0.37f, 0.39f), arrow, t);
			dl->AddLine(c.At(0.28f, 0.50f), c.At(0.37f, 0.61f), arrow, t);
		}

		void DrawCollapseSidebar(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddRect(c.At(0.12f, 0.16f), c.At(0.88f, 0.84f), color, c.Len(0.06f), 0, t);
			dl->AddLine(c.At(0.38f, 0.16f), c.At(0.38f, 0.84f), color, t);
			dl->AddLine(c.At(0.48f, 0.50f), c.At(0.72f, 0.50f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.72f, 0.50f), c.At(0.63f, 0.39f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.72f, 0.50f), c.At(0.63f, 0.61f), PaletteColor(color, kViolet), t);
		}

		/* 抓取当前帧（相机）：机身（软紫底 + 描边）+ 顶部取景凸起 + 镜头（紫环 + 薄荷芯） */
		void DrawCapture(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 顶部取景凸起 */
			dl->AddLine(c.At(0.3438f, 0.2813f), c.At(0.4063f, 0.1875f), color, t);
			dl->AddLine(c.At(0.4063f, 0.1875f), c.At(0.5938f, 0.1875f), color, t);
			dl->AddLine(c.At(0.5938f, 0.1875f), c.At(0.6563f, 0.2813f), color, t);

			/* 机身 */
			dl->AddRectFilled(c.At(0.1563f, 0.2813f), c.At(0.8438f, 0.7500f),
				PaletteColor(color, kViolet, 0.16f), c.Len(0.069f));
			dl->AddRect(c.At(0.1563f, 0.2813f), c.At(0.8438f, 0.7500f), color, c.Len(0.069f), 0, t);

			/* 镜头：紫环 + 薄荷芯 */
			dl->AddCircleFilled(c.At(0.5f, 0.5156f), c.Len(0.1313f), PaletteColor(color, kViolet, 0.22f));
			dl->AddCircle(c.At(0.5f, 0.5156f), c.Len(0.1313f), PaletteColor(color, kViolet), t);
			dl->AddCircleFilled(c.At(0.5f, 0.5156f), c.Len(0.0438f), PaletteColor(color, kMint));
		}

		/* ---- 内容（资源浏览器）----
		 * 文件夹管"在哪"、文件图标管"是什么"；双色调：外形描边 + 主体低透明度同色（缩略图网格
		 * 放到 100px 也不显单薄）。 */

		void DrawDirectory(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 前板（主体）填得比标签页亮：文件夹才有"前板压住后片"的层次 */
			static constexpr float kTab[4][2] = {
				{ 0.07f, 0.19f }, { 0.35f, 0.19f }, { 0.46f, 0.34f }, { 0.07f, 0.34f }
			};
			static constexpr float kBody[4][2] = {
				{ 0.08f, 0.35f }, { 0.92f, 0.35f }, { 0.92f, 0.83f }, { 0.08f, 0.83f }
			};
			FillPoly(dl, c, kTab, PaletteColor(color, kViolet, 0.16f));
			FillPoly(dl, c, kBody, PaletteColor(color, kViolet, 0.12f));

			/* 外形（正视图 + 标签页）：与工具栏里打开状态的斜前片区分开 */
			static constexpr float kFolder[6][2] = {
				{ 0.07f, 0.83f }, { 0.07f, 0.19f }, { 0.35f, 0.19f },
				{ 0.46f, 0.34f }, { 0.93f, 0.34f }, { 0.93f, 0.83f }
			};
			StrokePolyline(dl, c, kFolder, t, color, true);
			dl->AddLine(c.At(0.16f, 0.57f), c.At(0.82f, 0.57f), PaletteColor(color, kMint), t);
		}

		void DrawFile(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 通用文件：页（折角填淡色）+ 三行长短不一的文字线 */
			StrokePage(dl, c, t, color);

			dl->AddLine(c.At(0.34f, 0.48f), c.At(0.72f, 0.48f), color, t);
			dl->AddLine(c.At(0.34f, 0.63f), c.At(0.72f, 0.63f), color, t);
			dl->AddLine(c.At(0.34f, 0.78f), c.At(0.56f, 0.78f), color, t);
		}

		void DrawFileImage(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 图片文件：页里装着一张风景 —— 两座山 + 太阳（形够大，14px 下也认得出） */
			StrokePage(dl, c, t, color);

			dl->AddTriangleFilled(c.At(0.28f, 0.82f), c.At(0.50f, 0.53f), c.At(0.72f, 0.82f), PaletteColor(color, kViolet, 0.90f));
			dl->AddTriangleFilled(c.At(0.50f, 0.82f), c.At(0.66f, 0.62f), c.At(0.82f, 0.82f), PaletteColor(color, kMint, 0.72f));
			dl->AddCircleFilled(c.At(0.68f, 0.44f), c.Len(0.085f), PaletteColor(color, kMint));
		}

		/* SVG 文件：页里一段贝塞尔曲线 + 两端锚点（矢量绘图的通行记号）——
		 * 几何与 FileSvg.svg 同一副数（见同目录的矢量版）。 */
		void DrawFileSvg(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			StrokePage(dl, c, t, color);

			dl->PathLineTo(c.At(0.359f, 0.703f));
			dl->PathBezierCubicCurveTo(c.At(0.437f, 0.500f), c.At(0.547f, 0.422f), c.At(0.641f, 0.406f));
			dl->PathStroke(PaletteColor(color, kViolet), 0, t);

			dl->AddCircleFilled(c.At(0.359f, 0.703f), c.Len(0.044f), PaletteColor(color, kMint));
			dl->AddCircleFilled(c.At(0.641f, 0.406f), c.Len(0.044f), PaletteColor(color, kMint));
		}

		void DrawFileMtlGraph(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const float rounding = c.Len(0.06f);

			/* 材质图 = 一个带输入 / 输出端口的节点（用过节点编辑器的都认得）：
			 * 方框填淡色 + 左右两个实心端口 + 两小段连线。
			 * 别用"三个点连两条线"——那个造型是"分享"，反而认不出是节点图。 */
			dl->AddRectFilled(c.At(0.30f, 0.24f), c.At(0.70f, 0.76f), PaletteColor(color, kViolet, 0.18f), rounding);
			dl->AddRect(c.At(0.30f, 0.24f), c.At(0.70f, 0.76f), color, rounding, 0, t);

			dl->AddLine(c.At(0.14f, 0.50f), c.At(0.30f, 0.50f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.70f, 0.50f), c.At(0.86f, 0.50f), color, t);

			dl->AddCircleFilled(c.At(0.13f, 0.50f), c.Len(0.095f), PaletteColor(color, kMint));
			dl->AddCircleFilled(c.At(0.87f, 0.50f), c.Len(0.095f), color);
		}

		/* 着色器文件：页里一颗"材质球"（右半浸紫 = 明暗交界，左上一点薄荷 = 高光）。
		 * 球是 DCC 里最通行的着色器 / 材质预览造型，比"写个 fx 字母"或波形更一眼认得出。 */
		void DrawFileShader(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			StrokePage(dl, c, t, color);

			dl->AddCircleFilled(c.At(0.53f, 0.62f), c.Len(0.17f), PaletteColor(color, kViolet, 0.18f));
			dl->AddCircle(c.At(0.53f, 0.62f), c.Len(0.17f), color, 0, t);
			/* 右半个圆弧（-90° → +90° 经过 +x）：球被照亮的那一半，用强调色压出来 */
			dl->PathArcTo(c.At(0.53f, 0.62f), c.Len(0.17f), -1.5708f, 1.5708f, 12);
			dl->PathStroke(PaletteColor(color, kViolet), 0, t);
			dl->AddCircleFilled(c.At(0.45f, 0.54f), c.Len(0.045f), PaletteColor(color, kMint));
		}

		/* 模型文件：页里一个立方体 —— 与场景树里的 Model 是同一副造型（同一件东西的两种用法：
		 * 那边是"场景里的模型实体"，这里是"磁盘上的模型文件"）。 */
		void DrawFileModel(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			StrokePage(dl, c, t, color);

			/* 六边形轮廓（顶 / 右上 / 右下 / 底 / 左下 / 左上），折面填淡色、中间那个 Y 画深色 */
			static constexpr float kCube[6][2] = {
				{ 0.53f, 0.34f }, { 0.75f, 0.46f }, { 0.75f, 0.70f },
				{ 0.53f, 0.82f }, { 0.31f, 0.70f }, { 0.31f, 0.46f }
			};
			FillPoly(dl, c, kCube, PaletteColor(color, kViolet, 0.16f));
			StrokePolyline(dl, c, kCube, t, PaletteColor(color, kViolet), true);

			dl->AddLine(c.At(0.31f, 0.46f), c.At(0.53f, 0.58f), color, t);
			dl->AddLine(c.At(0.53f, 0.58f), c.At(0.75f, 0.46f), color, t);
			dl->AddLine(c.At(0.53f, 0.58f), c.At(0.53f, 0.82f), color, t);
		}

		/* 材质文件：页里一滴颜料（"材质 / 上色"的通用隐喻；与"着色器"的材质球刻意分开）。 */
		void DrawFileMaterial(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			StrokePage(dl, c, t, color);

			/* 泪滴：尖顶 + 两侧贝塞尔 + 底部半圆（弧心与两侧肩点严格衔接；几何与 SVG 版同一副数） */
			const auto build_drop = [&]()
			{
				dl->PathLineTo(c.At(0.50f, 0.35f));
				dl->PathBezierCubicCurveTo(c.At(0.588f, 0.469f), c.At(0.634f, 0.538f), c.At(0.634f, 0.60f));
				dl->PathArcTo(c.At(0.50f, 0.60f), c.Len(0.134f), 0.0f, 3.1415927f, 12);
				dl->PathBezierCubicCurveTo(c.At(0.366f, 0.538f), c.At(0.412f, 0.469f), c.At(0.50f, 0.35f));
			};
			build_drop();
			dl->PathFillConvex(PaletteColor(color, kViolet, 0.16f));
			build_drop();
			dl->PathStroke(PaletteColor(color, kViolet), 0, t);

			dl->AddCircleFilled(c.At(0.444f, 0.609f), c.Len(0.034f), PaletteColor(color, kMint));
		}

		/* 探针缓存文件：页里一个八面体 + 中心星芒（与场景树里的 ReflectionProbe 同一处标记——
		 * 那边是"场景里的探针实体"，这里是"磁盘上的烘焙结果"）。 */
		void DrawFileProbe(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			StrokePage(dl, c, t, color);

			/* 八面体轮廓（顶 / 右上 / 右下 / 底 / 左下 / 左上），折面填淡色 */
			static constexpr float kBody[6][2] = {
				{ 0.500f, 0.316f }, { 0.713f, 0.431f }, { 0.713f, 0.663f },
				{ 0.500f, 0.778f }, { 0.287f, 0.663f }, { 0.287f, 0.431f }
			};
			FillPoly(dl, c, kBody, PaletteColor(color, kViolet, 0.16f));
			StrokePolyline(dl, c, kBody, t, PaletteColor(color, kViolet), true);

			/* 腰线 + 竖棱 */
			dl->AddLine(c.At(0.287f, 0.431f), c.At(0.500f, 0.547f), color, t);
			dl->AddLine(c.At(0.500f, 0.547f), c.At(0.713f, 0.431f), color, t);
			dl->AddLine(c.At(0.500f, 0.547f), c.At(0.500f, 0.778f), color, t);

			/* 中心星芒（探针特征） */
			static constexpr float kStar[8][2] = {
				{ 0.500f, 0.453f }, { 0.534f, 0.522f }, { 0.603f, 0.547f }, { 0.534f, 0.572f },
				{ 0.500f, 0.641f }, { 0.466f, 0.572f }, { 0.397f, 0.547f }, { 0.466f, 0.522f }
			};
			FillPoly(dl, c, kStar, PaletteColor(color, kMint));
		}

		/* 头文件：页里一对尖括号夹着 H —— "声明"那类文件，与源文件的尖括号斜杠是一对。
		 * 几何与 FileCppHeader.svg 同一副数。 */
		void DrawFileCppHeader(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 mint = PaletteColor(color, kMint);

			StrokePage(dl, c, t, color);

			dl->AddLine(c.At(0.369f, 0.456f), c.At(0.331f, 0.531f), mint, t);
			dl->AddLine(c.At(0.331f, 0.531f), c.At(0.369f, 0.606f), mint, t);
			dl->AddLine(c.At(0.631f, 0.456f), c.At(0.669f, 0.531f), mint, t);
			dl->AddLine(c.At(0.669f, 0.531f), c.At(0.631f, 0.606f), mint, t);

			dl->AddLine(c.At(0.444f, 0.434f), c.At(0.444f, 0.628f), mint, t);
			dl->AddLine(c.At(0.556f, 0.434f), c.At(0.556f, 0.628f), mint, t);
			dl->AddLine(c.At(0.444f, 0.531f), c.At(0.556f, 0.531f), mint, t);
		}

		/* 源文件：页里一对尖括号夹着斜杠（代码符号）—— 与 ScriptComponent 同一枚记号，
		 * 那边是组件、这里是磁盘上的文件。几何与 FileCppSource.svg 同一副数。 */
		void DrawFileCppSource(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 mint = PaletteColor(color, kMint);

			StrokePage(dl, c, t, color);

			dl->AddLine(c.At(0.444f, 0.447f), c.At(0.375f, 0.541f), mint, t);
			dl->AddLine(c.At(0.375f, 0.541f), c.At(0.444f, 0.634f), mint, t);
			dl->AddLine(c.At(0.556f, 0.453f), c.At(0.625f, 0.547f), mint, t);
			dl->AddLine(c.At(0.625f, 0.547f), c.At(0.556f, 0.641f), mint, t);
			dl->AddLine(c.At(0.556f, 0.422f), c.At(0.481f, 0.672f), mint, t);
		}

		/* ---- 视口辅助（Gizmos 显隐菜单）：工具栏按钮 + 弹层分项 ----
		 * 造型与 SVG symbol 对齐：四角取景框 + 中心轴点（Gizmos）、
		 * 3×3 网格（Grid）、三轴角标（WorldAxis）。 */

		void DrawGizmos(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);
			const ImU32 mint = PaletteColor(color, kMint);

			/* 四角取景框：视口里的辅助显示范围 */
			dl->AddLine(c.At(0.10f, 0.32f), c.At(0.10f, 0.10f), violet, t * 1.1f);
			dl->AddLine(c.At(0.10f, 0.10f), c.At(0.32f, 0.10f), violet, t * 1.1f);
			dl->AddLine(c.At(0.68f, 0.10f), c.At(0.90f, 0.10f), violet, t * 1.1f);
			dl->AddLine(c.At(0.90f, 0.10f), c.At(0.90f, 0.32f), violet, t * 1.1f);
			dl->AddLine(c.At(0.90f, 0.68f), c.At(0.90f, 0.90f), violet, t * 1.1f);
			dl->AddLine(c.At(0.90f, 0.90f), c.At(0.68f, 0.90f), violet, t * 1.1f);
			dl->AddLine(c.At(0.32f, 0.90f), c.At(0.10f, 0.90f), violet, t * 1.1f);
			dl->AddLine(c.At(0.10f, 0.90f), c.At(0.10f, 0.68f), violet, t * 1.1f);

			/* 中心十字 + 轴点：实体图标的位置标记 */
			dl->AddLine(c.At(0.50f, 0.34f), c.At(0.50f, 0.66f), mint, t);
			dl->AddLine(c.At(0.34f, 0.50f), c.At(0.66f, 0.50f), mint, t);
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.055f), mint);
		}

		void DrawGrid(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);

			/* 3×3 网格：外框 + 两条纵 / 横内线（内线细一档区分主次） */
			dl->AddRect(c.At(0.12f, 0.12f), c.At(0.88f, 0.88f), violet, c.Len(0.06f), 0, t * 1.1f);
			const float p1 = 0.12f + 0.76f / 3.0f;
			const float p2 = 0.12f + 0.76f * 2.0f / 3.0f;
			dl->AddLine(c.At(p1, 0.12f), c.At(p1, 0.88f), violet, t * 0.72f);
			dl->AddLine(c.At(p2, 0.12f), c.At(p2, 0.88f), violet, t * 0.72f);
			dl->AddLine(c.At(0.12f, p1), c.At(0.88f, p1), violet, t * 0.72f);
			dl->AddLine(c.At(0.12f, p2), c.At(0.88f, p2), violet, t * 0.72f);
		}

		void DrawWorldAxis(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 mint = PaletteColor(color, kMint);
			const ImU32 red = PaletteColor(color, kRed);
			const ImU32 violet = PaletteColor(color, kViolet);

			/* 三轴角标：Y 向上 / X 左下 / Z 右下，轴端各两撇小箭头 */
			const ImVec2 origin = c.At(0.50f, 0.52f);
			dl->AddLine(origin, c.At(0.50f, 0.14f), mint, t);
			dl->AddLine(c.At(0.50f, 0.14f), c.At(0.39f, 0.31f), mint, t);
			dl->AddLine(c.At(0.50f, 0.14f), c.At(0.61f, 0.31f), mint, t);

			dl->AddLine(origin, c.At(0.15f, 0.85f), red, t);
			dl->AddLine(c.At(0.15f, 0.85f), c.At(0.18f, 0.66f), red, t);
			dl->AddLine(c.At(0.15f, 0.85f), c.At(0.34f, 0.82f), red, t);

			dl->AddLine(origin, c.At(0.85f, 0.85f), violet, t);
			dl->AddLine(c.At(0.85f, 0.85f), c.At(0.82f, 0.66f), violet, t);
			dl->AddLine(c.At(0.85f, 0.85f), c.At(0.66f, 0.82f), violet, t);

			/* 轴心点：继承墨色外圈 + 薄荷芯 */
			dl->AddCircleFilled(origin, c.Len(0.06f), color);
			dl->AddCircleFilled(origin, c.Len(0.028f), mint);
		}

		/* 局部坐标空间：虚线方框跟着实体转，三根轴从中心伸出 */
		void DrawLocalSpace(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 mint = PaletteColor(color, kMint);
			const ImU32 gray = PaletteColor(color, kGray);

			dl->AddRect(c.At(0.17f, 0.17f), c.At(0.83f, 0.83f), gray, 0.0f, 0, t * 0.7f);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.50f, 0.12f), mint, t);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.88f, 0.50f), mint, t);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.50f, 0.88f), mint, t);
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.07f), gray);
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.035f), mint);
		}

		/* 世界坐标空间：地平线 + 虚线球体，三根轴朝向固定 */
		void DrawWorldSpace(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);
			const ImU32 mint = PaletteColor(color, kMint);
			const ImU32 gray = PaletteColor(color, kGray);
			const ImVec2 origin = c.At(0.44f, 0.53f);

			dl->AddLine(c.At(0.06f, 0.88f), c.At(0.94f, 0.88f), gray, t * 0.7f);
			StrokeDashedCircle(dl, origin, c.Len(0.33f), gray, t * 0.7f, c.Len(0.04f), c.Len(0.07f));
			dl->AddLine(origin, c.At(0.44f, 0.12f), violet, t);
			dl->AddLine(origin, c.At(0.88f, 0.53f), violet, t);
			dl->AddLine(origin, c.At(0.44f, 0.94f), violet, t);
			dl->AddCircleFilled(origin, c.Len(0.08f), gray);
			dl->AddCircleFilled(origin, c.Len(0.04f), mint);
		}

		/* 调试视图：带网格的监视器 + 折线读数 + 标记点（右侧两道红杠是"标定"）——
		 * 对齐设计稿 all_icons.svg 的 debug-view。 */
		void DrawDebugView(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			const ImVec2 panel_min = c.At(0.125f, 0.156f);
			const ImVec2 panel_max = c.At(0.875f, 0.844f);
			const ImU32 gray = PaletteColor(color, kGray);

			dl->AddRectFilled(panel_min, panel_max, PaletteColor(color, kViolet, 0.12f), c.Len(0.0625f));

			/* 网格：四横三竖，压在折线下面当读数背景 */
			for (int i = 0; i < 4; ++i)
			{
				const float y = 0.3125f + 0.125f * static_cast<float>(i);
				dl->AddLine(c.At(0.125f, y), c.At(0.875f, y), gray, t * 0.5f);
			}
			for (int i = 0; i < 3; ++i)
			{
				const float x = 0.3125f + 0.1875f * static_cast<float>(i);
				dl->AddLine(c.At(x, 0.156f), c.At(x, 0.844f), gray, t * 0.5f);
			}

			/* 折线读数 + 标记点 */
			const ImVec2 curve_points[] = {
				c.At(0.219f, 0.688f), c.At(0.375f, 0.469f), c.At(0.500f, 0.594f),
				c.At(0.625f, 0.344f), c.At(0.781f, 0.531f),
			};
			dl->AddPolyline(curve_points, IM_ARRAYSIZE(curve_points), PaletteColor(color, kMint), 0, t * 0.9f);
			dl->AddCircleFilled(c.At(0.625f, 0.344f), c.Len(0.0625f),
				PaletteColor(color, kMint, 0.22f));
			dl->AddCircle(c.At(0.625f, 0.344f), c.Len(0.0625f), PaletteColor(color, kViolet), 0, t * 0.7f);

			/* 标定红杠 */
			const ImU32 red = PaletteColor(color, kRed);
			dl->AddLine(c.At(0.750f, 0.250f), c.At(0.813f, 0.250f), red, t * 0.8f);
			dl->AddLine(c.At(0.750f, 0.406f), c.At(0.813f, 0.406f), red, t * 0.8f);

			dl->AddRect(panel_min, panel_max, PaletteColor(color, kViolet), c.Len(0.0625f), 0, t);
		}

		/* ---- 调试档位（弹层图标族）----
		 * 三个分区各一套图形语言：Surface = 圆角方块（通道读数）、Lighting = 圆盘（受光）、
		 * Analysis = 叠层方块（逐片元统计）。方块族共用底座 / 描边两步，内部图形画在两者之间。 */

		void ChannelTileBase(ImDrawList* dl, const Canvas& c, ImU32 color)
		{
			dl->AddRectFilled(c.At(0.14f, 0.14f), c.At(0.86f, 0.86f), Soften(color, 0.16f), c.Len(0.10f));
		}

		void ChannelTileOutline(ImDrawList* dl, const Canvas& c, float thickness, ImU32 color)
		{
			dl->AddRect(c.At(0.14f, 0.14f), c.At(0.86f, 0.86f), color, c.Len(0.10f), 0, thickness);
		}

		/* Albedo：对角双色块（基色读数） */
		void DrawAlbedo(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			ChannelTileBase(dl, c, color);

			const float kUpperLeft[3][2] = { { 0.14f, 0.14f }, { 0.86f, 0.14f }, { 0.14f, 0.86f } };
			const float kLowerRight[3][2] = { { 0.86f, 0.14f }, { 0.86f, 0.86f }, { 0.14f, 0.86f } };
			FillPoly(dl, c, kUpperLeft, PaletteColor(color, kViolet));
			FillPoly(dl, c, kLowerRight, PaletteColor(color, kGray));

			ChannelTileOutline(dl, c, StrokeWidth(size), color);
		}

		/* Normal：曲面上的法线方向（基点 + 斜向箭头） */
		void DrawNormal(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			ChannelTileBase(dl, c, color);

			const ImU32 violet = PaletteColor(color, kViolet);
			dl->AddLine(c.At(0.40f, 0.64f), c.At(0.62f, 0.42f), violet, t * 1.15f);
			dl->AddLine(c.At(0.62f, 0.42f), c.At(0.44f, 0.42f), violet, t * 1.15f);
			dl->AddLine(c.At(0.62f, 0.42f), c.At(0.62f, 0.60f), violet, t * 1.15f);
			dl->AddCircleFilled(c.At(0.34f, 0.70f), c.Len(0.075f), PaletteColor(color, kGray));

			ChannelTileOutline(dl, c, t, color);
		}

		/* Roughness：三段明度台阶（粗糙度读数） */
		void DrawRoughness(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			ChannelTileBase(dl, c, color);

			dl->AddRectFilled(c.At(0.28f, 0.26f), c.At(0.72f, 0.37f), PaletteColor(color, kViolet), c.Len(0.05f));
			dl->AddRectFilled(c.At(0.28f, 0.445f), c.At(0.72f, 0.555f), PaletteColor(color, kGray), c.Len(0.05f));
			dl->AddRectFilled(c.At(0.28f, 0.63f), c.At(0.72f, 0.74f), color, c.Len(0.05f));

			ChannelTileOutline(dl, c, StrokeWidth(size), color);
		}

		/* Metallic：金属球（灰球 + 高光点） */
		void DrawMetallic(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			ChannelTileBase(dl, c, color);

			dl->AddCircleFilled(c.At(0.56f, 0.50f), c.Len(0.25f), PaletteColor(color, kGray));
			dl->AddCircleFilled(c.At(0.43f, 0.37f), c.Len(0.085f), PaletteColor(color, kMint));

			ChannelTileOutline(dl, c, StrokeWidth(size), color);
		}

		/* Specular Color：高光星芒（十字细菱形） */
		void DrawSpecularColor(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			ChannelTileBase(dl, c, color);

			const float kDiamondVertical[4][2] = { { 0.50f, 0.22f }, { 0.62f, 0.50f }, { 0.50f, 0.78f }, { 0.38f, 0.50f } };
			const float kDiamondHorizontal[4][2] = { { 0.22f, 0.50f }, { 0.50f, 0.38f }, { 0.78f, 0.50f }, { 0.50f, 0.62f } };
			FillPoly(dl, c, kDiamondVertical, PaletteColor(color, kViolet));
			FillPoly(dl, c, kDiamondHorizontal, PaletteColor(color, kViolet));
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.055f), PaletteColor(color, kMint));

			ChannelTileOutline(dl, c, StrokeWidth(size), color);
		}

		/* Ambient Occlusion：内角压暗（对角两级渐变） */
		void DrawAmbientOcclusion(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			ChannelTileBase(dl, c, color);

			const float kUpperLeft[3][2] = { { 0.14f, 0.14f }, { 0.46f, 0.14f }, { 0.14f, 0.46f } };
			const float kLowerRight[3][2] = { { 0.86f, 0.86f }, { 0.54f, 0.86f }, { 0.86f, 0.54f } };
			FillPoly(dl, c, kUpperLeft, PaletteColor(color, kGray));
			FillPoly(dl, c, kLowerRight, PaletteColor(color, kGray));

			ChannelTileOutline(dl, c, StrokeWidth(size), color);
		}

		/* Emission：自发光核 + 光晕环 */
		void DrawEmission(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			ChannelTileBase(dl, c, color);

			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.15f), PaletteColor(color, kMint));
			dl->AddCircle(c.At(0.50f, 0.50f), c.Len(0.27f), PaletteColor(color, kViolet), 0, t * 0.9f);

			ChannelTileOutline(dl, c, t, color);
		}

		/* Ambient：环境穹顶（半球面 + 基线） */
		void DrawAmbient(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			ChannelTileBase(dl, c, color);

			/* 先画整圆、再把下半盖回底色 —— 半球只用一次填充 */
			dl->AddCircleFilled(c.At(0.50f, 0.62f), c.Len(0.24f), PaletteColor(color, kGray));
			dl->AddRectFilled(c.At(0.14f, 0.62f), c.At(0.86f, 0.86f), Soften(color, 0.16f));
			dl->AddLine(c.At(0.22f, 0.62f), c.At(0.78f, 0.62f), color, t);

			ChannelTileOutline(dl, c, t, color);
		}

		/* Diffuse：左半受光的球（漫反射项） */
		void DrawDiffuse(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			const float radius = c.Len(0.30f);
			dl->AddCircleFilled(center, radius, PaletteColor(color, kGray));
			dl->PathArcTo(center, radius, kPi * 0.5f, kPi * 1.5f, 24);	/* 下方 → 左 → 上方 = 左半 */
			dl->PathFillConvex(PaletteColor(color, kViolet));
			dl->AddCircle(center, radius, color, 0, t);
		}

		/* Specular（光照项）：哑面球 + 尖锐高光点 */
		void DrawSpecular(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddCircleFilled(center, c.Len(0.30f), PaletteColor(color, kGray));
			dl->AddCircleFilled(c.At(0.40f, 0.36f), c.Len(0.085f), PaletteColor(color, kMint));
			dl->AddCircle(center, c.Len(0.30f), color, 0, t);
		}

		/* Shadow：物体 + 地面投影（扁椭圆） */
		void DrawShadow(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 地面投影：自采点的扁椭圆（ImDrawList 各版本椭圆填充 API 不一，避开版本耦合） */
			constexpr int kSteps = 24;
			ImVec2 shadow[kSteps];
			const ImVec2 shadow_center = c.At(0.50f, 0.74f);
			for (int i = 0; i < kSteps; ++i)
			{
				const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(kSteps);
				shadow[i] = ImVec2(shadow_center.x + std::cos(angle) * c.Len(0.30f),
					shadow_center.y + std::sin(angle) * c.Len(0.07f));
			}
			dl->AddConvexPolyFilled(shadow, kSteps, PaletteColor(color, kGray));

			dl->AddCircleFilled(c.At(0.50f, 0.36f), c.Len(0.17f), Soften(color, 0.16f));
			dl->AddCircle(c.At(0.50f, 0.36f), c.Len(0.17f), color, 0, t);
		}

		/* Indirect Diffuse：环境光从上方落下、向四周散开（拱弧 + 三颗入射点 + 受光底座） */
		void DrawIndirectDiffuse(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);
			const ImU32 mint = PaletteColor(color, kMint);
			const ImU32 pale_mint = PaletteColor(color, kMint, 0.22f);

			/* 环境弧 */
			dl->PathArcTo(c.At(0.50f, 0.50f), c.Len(0.375f), kPi, kPi * 2.0f, 32);
			dl->PathStroke(violet, 0, t * 0.85f);

			/* 弧上的三颗入射点 */
			for (int i = 0; i < 3; ++i)
			{
				const float x = 0.25f + 0.25f * static_cast<float>(i);
				const ImVec2 p = c.At(x, 0.375f - (i == 1 ? 0.1875f : 0.0f));
				dl->AddCircleFilled(p, c.Len(0.044f), pale_mint);
				dl->AddCircle(p, c.Len(0.044f), mint, 0, t * 0.6f);
			}

			/* 漫反射方向 */
			dl->AddLine(c.At(0.25f, 0.469f), c.At(0.406f, 0.625f), mint, t * 0.8f);
			dl->AddLine(c.At(0.50f, 0.344f), c.At(0.50f, 0.594f), mint, t * 0.8f);
			dl->AddLine(c.At(0.75f, 0.469f), c.At(0.594f, 0.625f), mint, t * 0.8f);

			/* 地面 + 受光底座 */
			dl->AddLine(c.At(0.219f, 0.781f), c.At(0.781f, 0.781f), color, t * 0.8f);
			dl->PathArcTo(c.At(0.50f, 0.781f), c.Len(0.125f), kPi, kPi * 2.0f, 24);
			dl->PathFillConvex(pale_mint);
			dl->PathArcTo(c.At(0.50f, 0.781f), c.Len(0.125f), kPi, kPi * 2.0f, 24);
			dl->PathStroke(violet, 0, t * 0.8f);
		}

		/* Indirect Specular：环境光打到表面后按反射方向弹走（拱弧 + 反弹箭头 + 高光点） */
		void DrawIndirectSpecular(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);
			const ImU32 mint = PaletteColor(color, kMint);

			/* 环境弧 + 单颗入射点 */
			dl->PathArcTo(c.At(0.50f, 0.50f), c.Len(0.375f), kPi, kPi * 2.0f, 32);
			dl->PathStroke(violet, 0, t * 0.85f);
			dl->AddCircleFilled(c.At(0.25f, 0.375f), c.Len(0.044f), PaletteColor(color, kMint, 0.22f));
			dl->AddCircle(c.At(0.25f, 0.375f), c.Len(0.044f), mint, 0, t * 0.6f);

			/* 落到表面再反弹出去 */
			dl->AddLine(c.At(0.25f, 0.437f), c.At(0.437f, 0.656f), mint, t * 0.9f);
			dl->AddLine(c.At(0.437f, 0.656f), c.At(0.750f, 0.281f), mint, t * 0.9f);
			const float kArrow[3][2] = { { 0.750f, 0.281f }, { 0.625f, 0.312f }, { 0.719f, 0.375f } };
			FillPoly(dl, c, kArrow, mint);

			/* 地面 + 反射面 */
			dl->AddLine(c.At(0.219f, 0.781f), c.At(0.781f, 0.781f), color, t * 0.8f);
			dl->PathArcTo(c.At(0.50f, 0.781f), c.Len(0.125f), kPi, kPi * 2.0f, 24);
			dl->PathFillConvex(PaletteColor(color, kViolet, 0.12f));
			dl->PathArcTo(c.At(0.50f, 0.781f), c.Len(0.125f), kPi, kPi * 2.0f, 24);
			dl->PathStroke(violet, 0, t * 0.8f);

			/* 高光星 */
			const float kSparkle[8][2] = {
				{ 0.494f, 0.547f }, { 0.528f, 0.612f }, { 0.594f, 0.647f }, { 0.528f, 0.681f },
				{ 0.494f, 0.747f }, { 0.459f, 0.681f }, { 0.394f, 0.647f }, { 0.459f, 0.612f },
			};
			FillConcavePoly(dl, c, kSparkle, PaletteColor(color, kMint, 0.22f));
			StrokePolyline(dl, c, kSparkle, t * 0.6f, violet, true);
		}

		/* Probe：圆盘分三段（灰底 + 红 / 青绿两段弧）：两个探针按权重混合的示意 */
		void DrawProbe(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			const float radius = c.Len(0.30f);
			dl->AddCircleFilled(center, radius, PaletteColor(color, kGray));
			dl->PathArcTo(center, radius, 0.0f, kPi * 0.5f, 12);	/* 右下 1/4：探针 0 */
			dl->PathFillConvex(PaletteColor(color, kRed));
			dl->PathArcTo(center, radius, kPi * 0.5f, kPi, 12);	/* 左下 1/4：探针 1 */
			dl->PathFillConvex(PaletteColor(color, kMint));
			dl->AddCircle(center, radius, color, 0, t);
		}

		/* Sliders：三条滑轨 + 三个旋钮（材质参数浮层的开关） */
		void DrawSliders(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			for (int i = 0; i < 3; ++i)
			{
				const float y = 0.28f + 0.22f * static_cast<float>(i);
				dl->AddLine(c.At(0.19f, y), c.At(0.81f, y), color, t);
			}

			dl->AddCircleFilled(c.At(0.34f, 0.28f), c.Len(0.10f), color);
			dl->AddCircleFilled(c.At(0.62f, 0.50f), c.Len(0.10f), PaletteColor(color, kMint));
			dl->AddCircleFilled(c.At(0.47f, 0.72f), c.Len(0.10f), color);
		}

		/* Overdraw：两层半透明方块的叠加计数 */
		void DrawOverdraw(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			const ImVec2 first_min = c.At(0.18f, 0.18f);
			const ImVec2 first_max = c.At(0.62f, 0.62f);
			const ImVec2 second_min = c.At(0.38f, 0.38f);
			const ImVec2 second_max = c.At(0.82f, 0.82f);

			dl->AddRectFilled(first_min, first_max, PaletteColor(color, kGray), c.Len(0.05f));
			dl->AddRectFilled(second_min, second_max, PaletteColor(color, kViolet), c.Len(0.05f));
			/* 叠加区：半透明墨色 —— 深浅两画布下都是"再覆盖一层"的观感 */
			dl->AddRectFilled(second_min, first_max, Soften(color, 0.38f), c.Len(0.03f));

			dl->AddRect(first_min, first_max, color, c.Len(0.05f), 0, t);
			dl->AddRect(second_min, second_max, color, c.Len(0.05f), 0, t);
		}

		/* Mipmap：由外到内的层级方框（层级热力的形状语言） */
		void DrawMipmap(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddRectFilled(c.At(0.16f, 0.16f), c.At(0.84f, 0.84f), Soften(color, 0.16f), c.Len(0.09f));
			dl->AddRectFilled(c.At(0.30f, 0.30f), c.At(0.70f, 0.70f), PaletteColor(color, kGray), c.Len(0.05f));
			dl->AddCircleFilled(c.At(0.50f, 0.50f), c.Len(0.065f), color);

			ChannelTileOutline(dl, c, t, color);
		}

		enum class SvgInk : uint8_t
		{
			None,
			Navy,
			Violet,
			Mint,
			PaleMint,
			PaleViolet,
			Red,
			Gray,
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

		constexpr ImU32 SvgInkColor(SvgInk ink, uint8_t alpha, bool dark_canvas)
		{
			if (dark_canvas)
			{
				switch (ink)
				{
				case SvgInk::Navy:        return IM_COL32(211, 222, 241, alpha);
				case SvgInk::Violet:      return IM_COL32(166, 158, 255, alpha);
				case SvgInk::Mint:        return IM_COL32(82, 222, 187, alpha);
				case SvgInk::PaleMint:    return IM_COL32(27, 67, 60, alpha);
				case SvgInk::PaleViolet:  return IM_COL32(43, 43, 78, alpha);
				case SvgInk::Red:         return IM_COL32(255, 132, 126, alpha);
				case SvgInk::Gray:        return IM_COL32(174, 187, 208, alpha);
				default:                  return IM_COL32(0, 0, 0, 0);
				}
			}

			switch (ink)
			{
			case SvgInk::Navy:        return IM_COL32(41, 53, 80, alpha);   /* #293550 */
			case SvgInk::Violet:      return IM_COL32(101, 95, 208, alpha); /* #655FD0 */
			case SvgInk::Mint:        return IM_COL32(39, 168, 138, alpha); /* #27A88A */
			case SvgInk::PaleMint:    return IM_COL32(225, 245, 238, alpha);/* #E1F5EE */
			case SvgInk::PaleViolet:  return IM_COL32(238, 240, 255, alpha);/* #EEF0FF */
			case SvgInk::Red:         return IM_COL32(213, 92, 85, alpha);  /* #D55C55 */
			case SvgInk::Gray:        return IM_COL32(170, 178, 193, alpha);/* #AAB2C1 */
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
				return SvgInk::Violet;
			if (std::strcmp(value, "#27a88a") == 0)
				return SvgInk::Mint;
			if (std::strcmp(value, "#e1f5ee") == 0)
				return SvgInk::PaleMint;
			if (std::strcmp(value, "#eef0ff") == 0)
				return SvgInk::PaleViolet;
			if (std::strcmp(value, "#d55c55") == 0)
				return SvgInk::Red;
			if (std::strcmp(value, "#aab2c1") == 0)
				return SvgInk::Gray;
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

		/* 元数据表：Id 与表项一一对应（顺序必须与 Id 枚举一致）*/
		/* ---- 动画 ---- */

		/* AnimationClip：上下齿孔带 + 中间的播放三角（与 SVG 同构的后备画法） */
		void DrawAnimationClip(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddRectFilled(c.At(0.13f, 0.16f), c.At(0.87f, 0.84f), Soften(color, 0.18f), c.Len(0.06f));
			dl->AddRect(c.At(0.13f, 0.16f), c.At(0.87f, 0.84f), color, c.Len(0.06f), 0, t);
			for (int i = 0; i < 4; ++i)
			{
				const float x = 0.21f + 0.18f * static_cast<float>(i);
				dl->AddLine(c.At(x, 0.16f), c.At(x, 0.28f), color, t);
				dl->AddLine(c.At(x, 0.72f), c.At(x, 0.84f), color, t);
			}
			dl->AddTriangleFilled(c.At(0.43f, 0.40f), c.At(0.43f, 0.60f), c.At(0.61f, 0.50f), PaletteColor(color, kMint));
		}

		/* Animation：三条轨道 + 错开的关键帧 + 播放方向 */
		void DrawAnimation(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			const float ys[3] = { 0.24f, 0.50f, 0.76f };
			for (int i = 0; i < 3; ++i)
			{
				dl->AddLine(c.At(0.18f, ys[i]), c.At(0.82f, ys[i]), color, t);
				dl->AddCircleFilled(c.At(i == 1 ? 0.40f : 0.50f, ys[i]), c.Len(0.09f),
					i == 1 ? color : PaletteColor(color, kMint));
			}
			dl->AddTriangleFilled(c.At(0.66f, 0.40f), c.At(0.66f, 0.60f), c.At(0.82f, 0.50f), color);
		}

		void DrawPause(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			dl->AddRectFilled(c.At(0.32f, 0.28f), c.At(0.44f, 0.72f), color, c.Len(0.04f));
			dl->AddRectFilled(c.At(0.56f, 0.28f), c.At(0.68f, 0.72f), PaletteColor(color, kMint), c.Len(0.04f));
		}

		/* 循环播放：开口圆 + 箭头，中间一个小播放三角（与设计稿的 loop 同形） */
		void DrawLoop(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 ring = PaletteColor(color, kViolet);
			const ImU32 glyph = PaletteColor(color, kMint);

			/* 开口在右上：从 -60° 扫到 280°，箭头落在起点上 */
			dl->PathArcTo(c.At(0.5f, 0.5f), c.Len(0.3125f), -1.0472f, 4.8869f, 40);
			dl->PathStroke(ring, 0, t);

			const ImVec2 arrow[] = { c.At(0.656f, 0.229f), c.At(0.541f, 0.266f), c.At(0.567f, 0.104f) };
			dl->AddTriangleFilled(arrow[0], arrow[1], arrow[2], ring);

			const ImVec2 play[] = { c.At(0.406f, 0.375f), c.At(0.656f, 0.5f), c.At(0.406f, 0.625f) };
			dl->AddTriangleFilled(play[0], play[1], play[2], glyph);
		}

		void DrawCurve(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddLine(c.At(0.14f, 0.12f), c.At(0.14f, 0.86f), color, t);
			dl->AddLine(c.At(0.14f, 0.86f), c.At(0.88f, 0.86f), color, t);

			const ImVec2 points[] = {
				c.At(0.22f, 0.72f), c.At(0.44f, 0.66f), c.At(0.64f, 0.32f), c.At(0.84f, 0.48f),
			};
			dl->AddPolyline(points, IM_ARRAYSIZE(points), color, 0, t);
			for (const ImVec2& point : points)
				dl->AddCircleFilled(point, c.Len(0.055f), PaletteColor(color, kMint));
		}

		void DrawPreview(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddRect(c.At(0.12f, 0.20f), c.At(0.88f, 0.80f), color, c.Len(0.06f), 0, t);
			const ImVec2 points[] = {
				c.At(0.20f, 0.70f), c.At(0.38f, 0.46f), c.At(0.52f, 0.58f), c.At(0.68f, 0.38f), c.At(0.82f, 0.70f),
			};
			dl->AddPolyline(points, IM_ARRAYSIZE(points), PaletteColor(color, kMint), 0, t);
			dl->AddCircleFilled(c.At(0.70f, 0.32f), c.Len(0.055f), PaletteColor(color, kMint));
		}

		/* ---- 后处理 ---- */

		/* RenderPass：三张依次下落的板 + 右侧下行箭头（效果按顺序串成链） */
		void DrawRenderPass(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			for (int i = 0; i < 3; ++i)
			{
				const float top = 0.09f + 0.29f * static_cast<float>(i);
				const float plate[4][2] = {
					{ 0.375f, top }, { 0.625f, top + 0.125f }, { 0.375f, top + 0.25f }, { 0.125f, top + 0.125f }
				};
				if (i == 0)
					FillPoly(dl, c, plate, PaletteColor(color, kViolet, 0.18f));
				StrokePolyline(dl, c, plate, t, PaletteColor(color, kViolet), true);
			}

			dl->AddLine(c.At(0.844f, 0.16f), c.At(0.844f, 0.66f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.75f, 0.57f), c.At(0.844f, 0.66f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.938f, 0.57f), c.At(0.844f, 0.66f), PaletteColor(color, kMint), t);
		}

		/* PostProcess：画面外框 + 取景内容 + 提亮星芒（成片之上再加一层效果） */
		void DrawPostProcess(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const float rounding = c.Len(0.045f);

			dl->AddRectFilled(c.At(0.11f, 0.125f), c.At(0.89f, 0.875f), PaletteColor(color, kViolet, 0.18f), rounding);
			dl->AddRect(c.At(0.11f, 0.125f), c.At(0.89f, 0.875f), PaletteColor(color, kViolet), rounding, 0, t);
			dl->AddCircleFilled(c.At(0.312f, 0.312f), c.Len(0.0625f), PaletteColor(color, kMint));

			const ImVec2 ridge[] = {
				c.At(0.19f, 0.72f), c.At(0.375f, 0.50f), c.At(0.50f, 0.625f), c.At(0.625f, 0.47f), c.At(0.81f, 0.72f)
			};
			dl->AddPolyline(ridge, IM_ARRAYSIZE(ridge), PaletteColor(color, kViolet), 0, t);

			const ImVec2 star_vertical[] = {
				c.At(0.688f, 0.25f), c.At(0.724f, 0.375f), c.At(0.688f, 0.50f), c.At(0.652f, 0.375f)
			};
			const ImVec2 star_horizontal[] = {
				c.At(0.563f, 0.375f), c.At(0.688f, 0.34f), c.At(0.813f, 0.375f), c.At(0.688f, 0.41f)
			};
			dl->AddConvexPolyFilled(star_vertical, IM_ARRAYSIZE(star_vertical), PaletteColor(color, kMint));
			dl->AddConvexPolyFilled(star_horizontal, IM_ARRAYSIZE(star_horizontal), PaletteColor(color, kMint));
		}

		/* Bloom：中心亮核 + 两圈光晕（外圈虚线）+ 八向发散 */
		void DrawBloom(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImVec2 middle = c.At(0.50f, 0.50f);

			dl->AddCircleFilled(middle, c.Len(0.094f), PaletteColor(color, kMint));
			dl->AddCircle(middle, c.Len(0.25f), PaletteColor(color, kViolet), 0, t);
			StrokeDashedCircle(dl, middle, c.Len(0.406f), color, t, c.Len(0.047f), c.Len(0.094f));

			dl->AddLine(c.At(0.50f, 0.03f), c.At(0.50f, 0.125f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.50f, 0.875f), c.At(0.50f, 0.97f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.03f, 0.50f), c.At(0.125f, 0.50f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.875f, 0.50f), c.At(0.97f, 0.50f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.172f, 0.172f), c.At(0.238f, 0.238f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.762f, 0.762f), c.At(0.828f, 0.828f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.828f, 0.172f), c.At(0.762f, 0.238f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.238f, 0.762f), c.At(0.172f, 0.828f), PaletteColor(color, kViolet), t);
		}

		/* ColorGrading：色轮（三块扇区）+ 顶部选取点 */
		void DrawColorGrading(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImVec2 middle = c.At(0.50f, 0.50f);
			const float radius = c.Len(0.375f);

			dl->AddCircleFilled(middle, radius, PaletteColor(color, kViolet, 0.16f));

			/* 三个扇区填薄荷：从右侧经底部绕到顶部 */
			dl->PathArcTo(middle, radius, 0.0f, 1.5f * kPi, 24);
			dl->PathLineTo(middle);
			dl->PathFillConvex(PaletteColor(color, kMint, 0.16f));
			dl->PathArcTo(middle, radius, 0.0f, 1.5f * kPi, 24);
			dl->PathStroke(PaletteColor(color, kMint), 0, t);
			dl->AddLine(middle, c.At(0.875f, 0.50f), PaletteColor(color, kMint), t);
			dl->AddLine(middle, c.At(0.50f, 0.125f), PaletteColor(color, kMint), t);

			dl->AddCircle(middle, radius, PaletteColor(color, kViolet), 0, t);

			/* 顶部选取点 + 右侧明度弧 + 两个色点 */
			dl->AddCircleFilled(c.At(0.50f, 0.3125f), c.Len(0.069f), PaletteColor(color, kMint, 0.16f));
			dl->AddCircle(c.At(0.50f, 0.3125f), c.Len(0.069f), PaletteColor(color, kMint), 0, t);
			dl->PathArcTo(c.At(0.6625f, 0.506f), c.Len(0.0625f), kPi * 0.5f, kPi * 1.5f, 8);
			dl->PathStroke(PaletteColor(color, kViolet), 0, t);
			dl->AddCircleFilled(c.At(0.375f, 0.6875f), c.Len(0.018f), PaletteColor(color, kViolet));
			dl->AddCircleFilled(c.At(0.50f, 0.75f), c.Len(0.018f), PaletteColor(color, kViolet));
		}

		/* DepthOfField：相机机身 + 镜头 + 下方对焦条（清晰范围） */
		void DrawDepthOfField(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			static constexpr float kBody[8][2] = {
				{ 0.094f, 0.5625f }, { 0.094f, 0.281f }, { 0.3125f, 0.281f }, { 0.406f, 0.156f },
				{ 0.625f, 0.156f }, { 0.719f, 0.281f }, { 0.906f, 0.281f }, { 0.906f, 0.5625f }
			};
			FillPoly(dl, c, kBody, PaletteColor(color, kViolet, 0.18f));
			StrokePolyline(dl, c, kBody, t, PaletteColor(color, kViolet), true);

			dl->AddCircleFilled(c.At(0.50f, 0.422f), c.Len(0.156f), PaletteColor(color, kMint, 0.16f));
			dl->AddCircle(c.At(0.50f, 0.422f), c.Len(0.156f), PaletteColor(color, kMint), 0, t);
			dl->AddCircle(c.At(0.50f, 0.422f), c.Len(0.0625f), PaletteColor(color, kViolet), 0, t);

			dl->AddLine(c.At(0.125f, 0.781f), c.At(0.3125f, 0.781f), color, t);
			dl->AddLine(c.At(0.6875f, 0.781f), c.At(0.875f, 0.781f), color, t);
			dl->AddCircleFilled(c.At(0.50f, 0.781f), c.Len(0.047f), PaletteColor(color, kMint));
		}

		/* MotionBlur：左侧速度线 + 右侧拉长的运动体 */
		void DrawMotionBlur(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			dl->AddLine(c.At(0.125f, 0.25f), c.At(0.4375f, 0.25f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.219f, 0.406f), c.At(0.594f, 0.406f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.3125f, 0.5625f), c.At(0.656f, 0.5625f), PaletteColor(color, kViolet), t);
			dl->AddLine(c.At(0.406f, 0.719f), c.At(0.8125f, 0.719f), PaletteColor(color, kViolet), t);

			static constexpr float kBlob[10][2] = {
				{ 0.531f, 0.219f }, { 0.688f, 0.25f }, { 0.781f, 0.375f }, { 0.844f, 0.5625f }, { 0.781f, 0.75f },
				{ 0.656f, 0.844f }, { 0.50f, 0.875f }, { 0.594f, 0.75f }, { 0.625f, 0.5625f }, { 0.594f, 0.375f }
			};
			FillConcavePoly(dl, c, kBlob, PaletteColor(color, kMint, 0.18f));
			StrokePolyline(dl, c, kBlob, t, PaletteColor(color, kMint), true);
		}

		/* AntiAliasing：阶梯边缘 + 虚线对齐参考（锯齿与过渡采样） */
		void DrawAntiAliasing(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			static constexpr float kStairs[13][2] = {
				{ 0.1875f, 0.719f }, { 0.1875f, 0.281f }, { 0.375f, 0.281f }, { 0.375f, 0.1875f },
				{ 0.5625f, 0.1875f }, { 0.5625f, 0.094f }, { 0.8125f, 0.094f }, { 0.8125f, 0.656f },
				{ 0.594f, 0.656f }, { 0.594f, 0.75f }, { 0.375f, 0.75f }, { 0.375f, 0.844f }, { 0.1875f, 0.844f }
			};
			FillConcavePoly(dl, c, kStairs, PaletteColor(color, kViolet, 0.18f));
			StrokePolyline(dl, c, kStairs, t, PaletteColor(color, kViolet), true);

			dl->AddLine(c.At(0.094f, 0.8125f), c.At(0.281f, 0.8125f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.4375f, 0.8125f), c.At(0.594f, 0.8125f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.75f, 0.781f), c.At(0.875f, 0.781f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.281f, 0.90f), c.At(0.281f, 0.75f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.4375f, 0.844f), c.At(0.4375f, 0.6875f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.594f, 0.75f), c.At(0.594f, 0.594f), PaletteColor(color, kMint), t);
			dl->AddLine(c.At(0.656f, 0.125f), c.At(0.75f, 0.125f), PaletteColor(color, kMint), t);
		}

		/* Vignette：画面外框 + 内框 + 中心亮区 + 虚线衰减环 */
		void DrawVignette(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImVec2 middle = c.At(0.50f, 0.50f);
			const float rounding = c.Len(0.045f);

			dl->AddRectFilled(c.At(0.094f, 0.125f), c.At(0.906f, 0.875f), PaletteColor(color, kViolet, 0.18f), rounding);
			dl->AddRect(c.At(0.094f, 0.125f), c.At(0.906f, 0.875f), PaletteColor(color, kViolet), rounding, 0, t);
			dl->AddRect(c.At(0.156f, 0.1875f), c.At(0.844f, 0.8125f), PaletteColor(color, kMint), c.Len(0.02f), 0, t);

			dl->AddCircleFilled(middle, c.Len(0.1875f), PaletteColor(color, kMint, 0.16f));
			dl->AddCircle(middle, c.Len(0.1875f), PaletteColor(color, kMint), 0, t);
			StrokeDashedCircle(dl, middle, c.Len(0.281f), PaletteColor(color, kViolet), t, c.Len(0.044f), c.Len(0.075f));

			dl->AddLine(c.At(0.219f, 0.25f), c.At(0.281f, 0.25f), color, t);
			dl->AddLine(c.At(0.719f, 0.25f), c.At(0.781f, 0.25f), color, t);
			dl->AddLine(c.At(0.219f, 0.75f), c.At(0.281f, 0.75f), color, t);
			dl->AddLine(c.At(0.719f, 0.75f), c.At(0.781f, 0.75f), color, t);
		}

		constexpr IconDesc s_Icons[] = {
			{ "None",              nullptr },
			{ "NewScene",          &DrawNewScene },
			{ "OpenScene",         &DrawOpenScene },
			{ "Save",              &DrawSave },
			{ "Import",            &DrawImport },
			{ "Export",            &DrawExport },
			{ "NewAsset",          &DrawNewAsset },
			{ "Undo",              &DrawUndo },
			{ "Redo",              &DrawRedo },
			{ "Back",              &DrawBack },
			{ "Forward",           &DrawForward },
			{ "Translate",         &DrawTranslate },
			{ "Rotate",            &DrawRotate },
			{ "Scale",             &DrawScale },
			{ "Play",              &DrawPlay },
			{ "Stop",              &DrawStop },
			{ "Live",              &DrawLive },
			{ "Record",            &DrawRecord },
			{ "PreviousFrame",     &DrawPreviousFrame },
			{ "NextFrame",         &DrawNextFrame },
			{ "Peak",              &DrawPeak },
			{ "Valley",            &DrawValley },
			{ "TraceText",         &DrawTraceText },
			{ "Menu",              &DrawMenu },
			{ "Add",               &DrawAdd },
			{ "Remove",            &DrawRemove },
			{ "Delete",            &DrawDelete },
			{ "Return",            &DrawReturn },
			{ "ReturnRoot",        &DrawReturnRoot },
			{ "Filter",            &DrawFilter },
			{ "Search",            &DrawSearch },
			{ "Visible",           &DrawVisible },
			{ "Hidden",            &DrawHidden },
			{ "Scene",             &DrawScene },
			{ "Entity",            &DrawEntity },
			{ "Model",             &DrawModel },
			{ "Cube",              &DrawCubeShape },
			{ "Sphere",            &DrawSphereShape },
			{ "Plane",             &DrawPlaneShape },
			{ "Shape3D",           &DrawShape3D },
			{ "Camera",            &DrawCamera },
			{ "Light",             &DrawLight },
			{ "LightDirectional",  &DrawDirectionalLight },
			{ "LightPoint",        &DrawPointLight },
			{ "LightSpot",         &DrawSpotLight },
			{ "ReflectionProbe",   &DrawReflectionProbe },
			{ "Sprite",            &DrawSprite },
			{ "Audio",             &DrawAudio },
			{ "Particle",          &DrawParticle },
			{ "Terrain",           &DrawTerrain },
			{ "Transform",         &DrawTransform },
			{ "Tag",               &DrawTag },
			{ "ScriptComponent",   &DrawScriptComponent },
			{ "LayerMask",         &DrawLayerMask },
			{ "Stats",             &DrawStats },
			{ "FrameGraph",        &DrawFrameGraph },
			{ "Log",               &DrawLog },
			{ "FitView",           &DrawFitView },
			{ "FlipVertical",      &DrawFlipVertical },
			{ "Maximize",          &DrawMaximize },
			{ "Restore",           &DrawRestore },
			{ "ExpandSidebar",     &DrawExpandSidebar },
			{ "CollapseSidebar",   &DrawCollapseSidebar },
			{ "Capture",           &DrawCapture },
			{ "Directory",         &DrawDirectory },
			{ "File",              &DrawFile },
			{ "FileImage",         &DrawFileImage },
			{ "FileSvg",           &DrawFileSvg },
			{ "FileScene",         &DrawScene },
			{ "FileMtlGraph",      &DrawFileMtlGraph },
			{ "FileShader",        &DrawFileShader },
			{ "FileModel",         &DrawFileModel },
			{ "FileMaterial",      &DrawFileMaterial },
			{ "FileProbe",         &DrawFileProbe },
			{ "FileCppHeader",     &DrawFileCppHeader },
			{ "FileCppSource",     &DrawFileCppSource },
			{ "Gizmos",            &DrawGizmos },
			{ "Grid",              &DrawGrid },
			{ "WorldAxis",         &DrawWorldAxis },
			{ "LocalSpace",        &DrawLocalSpace },
			{ "WorldSpace",        &DrawWorldSpace },
			{ "DebugView",         &DrawDebugView },
			{ "Albedo",            &DrawAlbedo },
			{ "Normal",            &DrawNormal },
			{ "Roughness",         &DrawRoughness },
			{ "Metallic",          &DrawMetallic },
			{ "SpecularColor",     &DrawSpecularColor },
			{ "AmbientOcclusion",  &DrawAmbientOcclusion },
			{ "Emission",          &DrawEmission },
			{ "Ambient",           &DrawAmbient },
			{ "Diffuse",           &DrawDiffuse },
			{ "Specular",          &DrawSpecular },
			{ "Shadow",            &DrawShadow },
			{ "IndirectDiffuse",   &DrawIndirectDiffuse },
			{ "IndirectSpecular",  &DrawIndirectSpecular },
			{ "Overdraw",          &DrawOverdraw },
			{ "Mipmap",            &DrawMipmap },
			{ "Probe",             &DrawProbe },
			{ "Sliders",           &DrawSliders },
			{ "AnimationClip",     &DrawAnimationClip },
			{ "Animation",         &DrawAnimation },
			{ "Pause",             &DrawPause },
			{ "Curve",             &DrawCurve },
			{ "Preview",           &DrawPreview },
			{ "Loop",              &DrawLoop },
			{ "RenderPass",        &DrawRenderPass },
			{ "PostProcess",       &DrawPostProcess },
			{ "Bloom",             &DrawBloom },
			{ "ColorGrading",      &DrawColorGrading },
			{ "DepthOfField",      &DrawDepthOfField },
			{ "MotionBlur",        &DrawMotionBlur },
			{ "AntiAliasing",      &DrawAntiAliasing },
			{ "Vignette",          &DrawVignette },
		};

		static_assert(sizeof(s_Icons) / sizeof(s_Icons[0]) == static_cast<size_t>(Id::COUNT),
			"Icons metadata table out of sync with the Id enum; add the new entries");
	}

	const IconDesc& Get(Id id)
	{
		const auto index = static_cast<size_t>(id);
		return s_Icons[index < static_cast<size_t>(Id::COUNT) ? index : 0];
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
		if (s_SvgIcons[index].Shapes.empty())
		{
			if (s_Icons[index].Draw != nullptr)
				s_Icons[index].Draw(draw_list, center, optical_size, color);
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
