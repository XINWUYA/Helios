#include "Pch.h"
#include "EditorIcons.h"
#include "EditorIconsSvg.h"
#include "Helios/ImGui/EditorTheme.h"
#include <tinyxml2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

		void DrawReturn(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const ImU32 violet = PaletteColor(color, kViolet);

			/* 折返路径先向左、再下行，末端箭头明确指向左侧 */
			dl->PathLineTo(c.At(0.86f, 0.28f));
			dl->PathLineTo(c.At(0.40f, 0.28f));
			dl->PathBezierCubicCurveTo(c.At(0.31f, 0.28f), c.At(0.30f, 0.35f), c.At(0.30f, 0.44f), 8);
			dl->PathLineTo(c.At(0.30f, 0.72f));
			dl->PathStroke(color, 0, t);

			dl->PathLineTo(c.At(0.30f, 0.58f));
			dl->PathLineTo(c.At(0.12f, 0.72f));
			dl->PathLineTo(c.At(0.30f, 0.86f));
			dl->PathStroke(violet, 0, t);
			dl->AddLine(c.At(0.68f, 0.28f), c.At(0.86f, 0.28f), PaletteColor(color, kMint), t);
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

		struct SvgContour
		{
			std::vector<ImVec2> Points;
			bool Closed = false;
		};

		struct SvgShape
		{
			enum class Type : uint8_t { Path, Circle, Rect };
			Type Kind = Type::Path;
			SvgStyle Style;
			std::vector<SvgContour> Contours;
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

		constexpr const char* kSvgSymbolIds[] = {
			"", "new-scene", "open-scene", "save", "import", "new-asset", "undo", "redo", "back", "forward",
			"translate", "rotate", "scale",
			"play", "stop", "menu", "add", "remove", "return", "filter", "search", "visible", "scene",
			"entity", "model", "camera", "light", "light-directional", "light-point", "light-spot",
			"reflection-probe", "sprite", "audio", "particle", "terrain", "transform", "tag", "stats",
			"directory", "file", "file-image", "file-scene", "file-mtl-graph", "file-shader", "file-model",
		};
		static_assert(IM_ARRAYSIZE(kSvgSymbolIds) == static_cast<size_t>(Id::COUNT),
			"SVG symbol map must remain aligned with Icons::Id");

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

		struct SvgPathToken
		{
			char Command = 0;
			float Number = 0.0f;
			bool IsCommand = false;
		};

		std::vector<SvgPathToken> TokenizeSvgPath(const char* data)
		{
			std::vector<SvgPathToken> tokens;
			const char* cursor = data;
			while (*cursor != '\0')
			{
				if ((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z'))
				{
					tokens.push_back({ *cursor++, 0.0f, true });
					continue;
				}
				if (*cursor == ',' || *cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')
				{
					++cursor;
					continue;
				}
				char* end = nullptr;
				const float number = std::strtof(cursor, &end);
				if (end == cursor)
				{
					++cursor;
					continue;
				}
				tokens.push_back({ 0, number, false });
				cursor = end;
			}
			return tokens;
		}

		void AppendSvgArc(std::vector<ImVec2>& points, const ImVec2& from,
		                  float rx, float ry, float rotation, bool large_arc, bool sweep,
		                  const ImVec2& to)
		{
			rx = std::fabs(rx);
			ry = std::fabs(ry);
			if (rx <= 0.0f || ry <= 0.0f || (std::fabs(from.x - to.x) < 0.0001f && std::fabs(from.y - to.y) < 0.0001f))
			{
				points.push_back(to);
				return;
			}

			const float phi = rotation * (kPi / 180.0f);
			const float cos_phi = std::cos(phi);
			const float sin_phi = std::sin(phi);
			const float dx = (from.x - to.x) * 0.5f;
			const float dy = (from.y - to.y) * 0.5f;
			const float x1p = cos_phi * dx + sin_phi * dy;
			const float y1p = -sin_phi * dx + cos_phi * dy;
			const float lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
			if (lambda > 1.0f)
			{
				const float scale = std::sqrt(lambda);
				rx *= scale;
				ry *= scale;
			}

			const float rx2 = rx * rx;
			const float ry2 = ry * ry;
			const float numerator = std::max(0.0f, rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p);
			const float denominator = rx2 * y1p * y1p + ry2 * x1p * x1p;
			const float sign = large_arc == sweep ? -1.0f : 1.0f;
			const float factor = denominator > 0.0f ? sign * std::sqrt(numerator / denominator) : 0.0f;
			const float cxp = factor * rx * y1p / ry;
			const float cyp = factor * -ry * x1p / rx;
			const float cx = cos_phi * cxp - sin_phi * cyp + (from.x + to.x) * 0.5f;
			const float cy = sin_phi * cxp + cos_phi * cyp + (from.y + to.y) * 0.5f;

			const float ux = (x1p - cxp) / rx;
			const float uy = (y1p - cyp) / ry;
			const float vx = (-x1p - cxp) / rx;
			const float vy = (-y1p - cyp) / ry;
			float start = std::atan2(uy, ux);
			float delta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
			if (!sweep && delta > 0.0f)
				delta -= 2.0f * kPi;
			else if (sweep && delta < 0.0f)
				delta += 2.0f * kPi;

			const int segments = std::max(4, static_cast<int>(std::ceil(std::fabs(delta) * 12.0f / kPi)));
			for (int i = 1; i <= segments; ++i)
			{
				const float angle = start + delta * (static_cast<float>(i) / segments);
				const float x = rx * std::cos(angle);
				const float y = ry * std::sin(angle);
				points.emplace_back(cx + cos_phi * x - sin_phi * y, cy + sin_phi * x + cos_phi * y);
			}
		}

		std::vector<SvgContour> ParseSvgPath(const char* data, SvgInk fill)
		{
			const std::vector<SvgPathToken> tokens = TokenizeSvgPath(data);
			std::vector<SvgContour> contours;
			SvgContour contour;
			ImVec2 current(0.0f, 0.0f);
			ImVec2 start(0.0f, 0.0f);
			ImVec2 previous_cubic(0.0f, 0.0f);
			ImVec2 previous_quadratic(0.0f, 0.0f);
			char command = 0;
			size_t index = 0;
			bool previous_was_cubic = false;
			bool previous_was_quadratic = false;

			const auto flush = [&]()
			{
				if (contour.Points.size() > 1)
				{
					if (fill != SvgInk::None)
						contour.Closed = true;
					contours.push_back(std::move(contour));
				}
				contour = SvgContour{};
			};
			const auto read_number = [&](float& value) -> bool
			{
				if (index >= tokens.size() || tokens[index].IsCommand)
					return false;
				value = tokens[index++].Number;
				return true;
			};
			const auto read_point = [&](bool relative, ImVec2& point) -> bool
			{
				float x = 0.0f, y = 0.0f;
				if (!read_number(x) || !read_number(y))
					return false;
				point = relative ? ImVec2(current.x + x, current.y + y) : ImVec2(x, y);
				return true;
			};

			while (index < tokens.size())
			{
				if (tokens[index].IsCommand)
					command = tokens[index++].Command;
				if (command == 0)
					break;

				const bool relative = command >= 'a' && command <= 'z';
				const char op = relative ? static_cast<char>(command - 'a' + 'A') : command;
				if (op == 'Z')
				{
					current = start;
					contour.Closed = true;
					flush();
					previous_was_cubic = false;
					previous_was_quadratic = false;
					command = 0;
					continue;
				}
				if (index >= tokens.size() || tokens[index].IsCommand)
					continue;

				if (op == 'M' || op == 'L')
				{
					ImVec2 point;
					if (!read_point(relative, point))
						break;
					if (op == 'M')
					{
						flush();
						current = point;
						start = point;
						contour.Points.push_back(point);
						command = relative ? 'l' : 'L';
					}
					else
					{
						current = point;
						contour.Points.push_back(point);
					}
					previous_was_cubic = false;
					previous_was_quadratic = false;
					continue;
				}
				if (op == 'H' || op == 'V')
				{
					float value = 0.0f;
					if (!read_number(value))
						break;
					current = op == 'H' ? ImVec2(relative ? current.x + value : value, current.y)
					                    : ImVec2(current.x, relative ? current.y + value : value);
					contour.Points.push_back(current);
					previous_was_cubic = false;
					previous_was_quadratic = false;
					continue;
				}
				if (op == 'C')
				{
					ImVec2 c1, c2, end;
					if (!read_point(relative, c1) || !read_point(relative, c2) || !read_point(relative, end))
						break;
					const ImVec2 from = current;
					for (int step = 1; step <= 12; ++step)
					{
						const float t = static_cast<float>(step) / 12.0f;
						const float u = 1.0f - t;
						contour.Points.emplace_back(u*u*u*from.x + 3*u*u*t*c1.x + 3*u*t*t*c2.x + t*t*t*end.x,
							u*u*u*from.y + 3*u*u*t*c1.y + 3*u*t*t*c2.y + t*t*t*end.y);
					}
					current = end;
					previous_cubic = c2;
					previous_was_cubic = true;
					previous_was_quadratic = false;
					continue;
				}
				if (op == 'S')
				{
					ImVec2 c1, c2, end;
					c1 = previous_was_cubic
						? ImVec2(2.0f * current.x - previous_cubic.x, 2.0f * current.y - previous_cubic.y)
						: current;
					if (!read_point(relative, c2) || !read_point(relative, end))
						break;
					const ImVec2 from = current;
					for (int step = 1; step <= 12; ++step)
					{
						const float t = static_cast<float>(step) / 12.0f;
						const float u = 1.0f - t;
						contour.Points.emplace_back(u*u*u*from.x + 3*u*u*t*c1.x + 3*u*t*t*c2.x + t*t*t*end.x,
							u*u*u*from.y + 3*u*u*t*c1.y + 3*u*t*t*c2.y + t*t*t*end.y);
					}
					current = end;
					previous_cubic = c2;
					previous_was_cubic = true;
					previous_was_quadratic = false;
					continue;
				}
				if (op == 'Q' || op == 'T')
				{
					ImVec2 control, end;
					if (op == 'Q')
					{
						if (!read_point(relative, control) || !read_point(relative, end))
						break;
					}
					else
					{
						control = previous_was_quadratic
							? ImVec2(2.0f * current.x - previous_quadratic.x, 2.0f * current.y - previous_quadratic.y)
							: current;
						if (!read_point(relative, end))
						break;
					}
					const ImVec2 from = current;
					for (int step = 1; step <= 10; ++step)
					{
						const float t = static_cast<float>(step) / 10.0f;
						const float u = 1.0f - t;
						contour.Points.emplace_back(u*u*from.x + 2*u*t*control.x + t*t*end.x,
							u*u*from.y + 2*u*t*control.y + t*t*end.y);
					}
					current = end;
					previous_quadratic = control;
					previous_was_quadratic = true;
					previous_was_cubic = false;
					continue;
				}
				if (op == 'A')
				{
					float rx = 0.0f, ry = 0.0f, rotation = 0.0f, large = 0.0f, sweep = 0.0f;
					ImVec2 end;
					if (!read_number(rx) || !read_number(ry) || !read_number(rotation)
						|| !read_number(large) || !read_number(sweep) || !read_point(relative, end))
						break;
					AppendSvgArc(contour.Points, current, rx, ry, rotation, large != 0.0f, sweep != 0.0f, end);
					current = end;
					previous_was_cubic = false;
					previous_was_quadratic = false;
					continue;
				}
				break;
			}
			flush();
			return contours;
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
						shape.Contours = ParseSvgPath(data, style.Fill);
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
		std::once_flag s_SvgIconsInit;

		void InitializeSvgIcons()
		{
			tinyxml2::XMLDocument document;
			if (document.Parse(Detail::kSvgSymbols) != tinyxml2::XML_SUCCESS || document.RootElement() == nullptr)
				return;
			const tinyxml2::XMLElement* defs = document.RootElement()->FirstChildElement("defs");
			if (defs == nullptr)
				return;

			for (size_t i = 1; i < static_cast<size_t>(Id::COUNT); ++i)
			{
				const tinyxml2::XMLElement* symbol = nullptr;
				for (const tinyxml2::XMLElement* candidate = defs->FirstChildElement("symbol"); candidate != nullptr;
					candidate = candidate->NextSiblingElement("symbol"))
				{
					const char* symbol_id = candidate->Attribute("id");
					if (symbol_id != nullptr && std::strcmp(symbol_id, kSvgSymbolIds[i]) == 0)
					{
						symbol = candidate;
						break;
					}
				}
				if (symbol != nullptr)
					ParseSvgElement(symbol, SvgStyle{}, s_SvgIcons[i]);
			}
		}

		bool IsSvgPointInsideTriangle(const ImVec2& p, const ImVec2& a, const ImVec2& b, const ImVec2& c)
		{
			const auto edge = [](const ImVec2& p0, const ImVec2& p1, const ImVec2& p2)
			{
				return (p2.x - p0.x) * (p1.y - p0.y) - (p1.x - p0.x) * (p2.y - p0.y);
			};
			const float d1 = edge(p, a, b);
			const float d2 = edge(p, b, c);
			const float d3 = edge(p, c, a);
			const bool negative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
			const bool positive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
			return !(negative && positive);
		}

		void FillSvgPolygon(ImDrawList* draw_list, const std::vector<ImVec2>& points, ImU32 color)
		{
			if (points.size() < 3)
				return;
			std::vector<int> indices(points.size());
			for (size_t i = 0; i < indices.size(); ++i)
				indices[i] = static_cast<int>(i);
			float area = 0.0f;
			for (size_t i = 0; i < points.size(); ++i)
			{
				const ImVec2& a = points[i];
				const ImVec2& b = points[(i + 1) % points.size()];
				area += a.x * b.y - b.x * a.y;
			}
			const float orientation = area >= 0.0f ? 1.0f : -1.0f;
			int guard = static_cast<int>(points.size() * points.size());
			while (indices.size() > 3 && guard-- > 0)
			{
				bool clipped = false;
				for (size_t i = 0; i < indices.size(); ++i)
				{
					const int ia = indices[(i + indices.size() - 1) % indices.size()];
					const int ib = indices[i];
					const int ic = indices[(i + 1) % indices.size()];
					const ImVec2& a = points[ia];
					const ImVec2& b = points[ib];
					const ImVec2& c = points[ic];
					const float cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
					if (cross * orientation <= 0.00001f)
						continue;
					bool contains = false;
					for (int index : indices)
					{
						if (index == ia || index == ib || index == ic)
							continue;
						if (IsSvgPointInsideTriangle(points[index], a, b, c))
						{
							contains = true;
						break;
						}
					}
					if (contains)
						continue;
					draw_list->AddTriangleFilled(a, b, c, color);
					indices.erase(indices.begin() + static_cast<ptrdiff_t>(i));
					clipped = true;
					break;
				}
				if (!clipped)
					break;
			}
			if (indices.size() == 3)
				draw_list->AddTriangleFilled(points[indices[0]], points[indices[1]], points[indices[2]], color);
		}

		float SvgStrokeWidth(const SvgStyle& style, float unit, bool dark_canvas)
		{
			const float scaled = style.StrokeWidth * unit * (dark_canvas ? 1.12f : 1.0f);
			return dark_canvas ? std::max(1.35f, scaled) : scaled;
		}

		float SvgDarkOpticalBoost(float size)
		{
			if (size <= 16.0f)
				return 1.30f;
			if (size <= 24.0f)
				return 1.30f - (size - 16.0f) * (0.16f / 8.0f);
			if (size <= 32.0f)
				return 1.14f - (size - 24.0f) * (0.14f / 8.0f);
			return 1.0f;
		}

		void DrawSvgContour(ImDrawList* draw_list, const SvgContour& contour, const SvgStyle& style,
		                    const ImVec2& center, float unit, uint8_t alpha, bool dark_canvas)
		{
			if (contour.Points.size() < 2)
				return;
			std::vector<ImVec2> points;
			points.reserve(contour.Points.size());
			for (const ImVec2& point : contour.Points)
				points.emplace_back(center.x + (point.x - 16.0f) * unit, center.y + (point.y - 16.0f) * unit);

			if (style.Fill != SvgInk::None && contour.Closed)
				FillSvgPolygon(draw_list, points, SvgInkColor(style.Fill, alpha, dark_canvas));

			if (style.Stroke != SvgInk::None && style.StrokeWidth > 0.0f)
			{
				const ImU32 stroke_color = SvgInkColor(style.Stroke, alpha, dark_canvas);
				const float width = SvgStrokeWidth(style, unit, dark_canvas);
				const bool dashed = style.DashOn > 0.0f && style.DashOff > 0.0f;
				const size_t segment_count = points.size() - 1 + (contour.Closed ? 1 : 0);

				if (!dashed)
				{
					draw_list->PathClear();
					for (const ImVec2& point : points)
						draw_list->PathLineTo(point);
					draw_list->PathStroke(stroke_color, contour.Closed ? ImDrawFlags_Closed : 0, width);
				}

				for (size_t i = 0; i < segment_count; ++i)
				{
					const ImVec2& a = points[i];
					const ImVec2& b = points[(i + 1) % points.size()];
					if (!dashed)
						continue;

					const float dx = b.x - a.x;
					const float dy = b.y - a.y;
					const float length = std::sqrt(dx * dx + dy * dy);
					const float cycle = (style.DashOn + style.DashOff) * unit;
					if (length <= 0.001f || cycle <= 0.001f)
						continue;
					for (float offset = 0.0f; offset < length; offset += cycle)
					{
						const float end = std::min(offset + style.DashOn * unit, length);
						const ImVec2 p0(a.x + dx * (offset / length), a.y + dy * (offset / length));
						const ImVec2 p1(a.x + dx * (end / length), a.y + dy * (end / length));
						draw_list->AddLine(p0, p1, stroke_color, width);
						if (style.RoundCap)
						{
							const float radius = width * 0.5f;
							draw_list->AddCircleFilled(p0, radius, stroke_color, 8);
							draw_list->AddCircleFilled(p1, radius, stroke_color, 8);
						}
					}
				}

				if (!dashed && style.RoundCap && !contour.Closed)
				{
					const float radius = width * 0.5f;
					draw_list->AddCircleFilled(points.front(), radius, stroke_color, 8);
					draw_list->AddCircleFilled(points.back(), radius, stroke_color, 8);
				}
				if (style.RoundJoin)
				{
					const float radius = width * 0.5f;
					const size_t point_count = contour.Closed ? points.size() : points.size() - 2;
					for (size_t i = 0; i < point_count; ++i)
					{
						const size_t point_index = contour.Closed ? i : i + 1;
						draw_list->AddCircleFilled(points[point_index], radius, stroke_color, 8);
					}
				}
			}
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
					for (const SvgContour& contour : shape.Contours)
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
		constexpr IconDesc s_Icons[] = {
			{ "None",              nullptr,                 1.0f },
			{ "NewScene",          &DrawNewScene,           0.86f },
			{ "OpenScene",         &DrawOpenScene,          0.86f },
			{ "Save",              &DrawSave,               0.97f },
			{ "Import",            &DrawImport,             0.93f },
			{ "NewAsset",          &DrawNewAsset,           1.0f },
			{ "Undo",              &DrawUndo,               0.96f },
			{ "Redo",              &DrawRedo,               1.04f },
			{ "Back",              &DrawBack,               0.97f },
			{ "Forward",           &DrawForward,            0.97f },
			{ "Translate",         &DrawTranslate,          0.92f },
			{ "Rotate",            &DrawRotate,             1.09375f },
			{ "Scale",             &DrawScale,              0.93f },
			{ "Play",              &DrawPlay,               0.93f },
			{ "Stop",              &DrawStop,               0.93f },
			{ "Menu",              &DrawMenu,               1.09f },
			{ "Add",               &DrawAdd,                0.93f },
			{ "Remove",            &DrawRemove,             0.93f },
			{ "Return",            &DrawReturn,             0.91f },
			{ "Filter",            &DrawFilter,             0.93f },
			{ "Search",            &DrawSearch,             1.109375f },
			{ "Visible",           &DrawVisible,            0.86f },
			{ "Scene",             &DrawScene,              0.90f },
			{ "Entity",            &DrawEntity,             0.93f },
			{ "Model",             &DrawModel,              0.93f },
			{ "Camera",            &DrawCamera,             0.83f },
			{ "Light",             &DrawLight,              0.86f },
			{ "LightDirectional",  &DrawDirectionalLight,   0.83f },
			{ "LightPoint",        &DrawPointLight,         0.76f },
			{ "LightSpot",         &DrawSpotLight,          0.81f },
			{ "ReflectionProbe",   &DrawReflectionProbe,    0.93f },
			{ "Sprite",            &DrawSprite,             0.93f },
			{ "Audio",             &DrawAudio,              0.90f },
			{ "Particle",          &DrawParticle,           0.97f },
			{ "Terrain",           &DrawTerrain,            0.86f },
			{ "Transform",         &DrawTransform,          0.81f },
			{ "Tag",               &DrawTag,                0.93f },
			{ "Stats",             &DrawStats,              0.93f },
			{ "Directory",         &DrawDirectory,          0.86f },
			{ "File",              &DrawFile,               0.86f },
			{ "FileImage",         &DrawFileImage,          0.86f },
			{ "FileScene",         &DrawScene,              0.86f },
			{ "FileMtlGraph",      &DrawFileMtlGraph,       0.86f },
			{ "FileShader",        &DrawFileShader,         0.86f },
			{ "FileModel",         &DrawFileModel,          0.86f },
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

		std::call_once(s_SvgIconsInit, InitializeSvgIcons);
		const float optical_size = size * s_Icons[index].OpticalScale;
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
	constexpr float kLeadingIconX = 11.0f;
	constexpr float kLeadingIconPad = 20.0f;

	inline float LeadingIconSize()
	{
		return std::min(ImGui::GetFontSize(), 14.0f);
	}

	/* 预留图标占的横向空间（包住控件，之后用 Pop 还原）。
	 * 撑大的是 `FramePadding`，而 `BeginComboPopup` 会把弹层的横向内边距取成它 ——
	 * 带弹层的控件（下拉框）别用这一对，会连带把弹层撑开；输入框这类没有弹层的才用它。 */
	void BeginLeadingIcon()
	{
		const ImVec2 frame_padding = ImGui::GetStyle().FramePadding;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
			ImVec2(frame_padding.x + kLeadingIconPad, frame_padding.y));
	}

	/* 把图标画在控件左端中部：用弱化色（跟搜索框的放大镜同一个色，都是"这东西是干嘛的"的提示）。
	 * 矩形由调用方给（不用 GetItemRect*）：下拉框弹层打开时当前窗口已经切到弹层了。 */
	void DrawLeadingIcon(Id id, const ImVec2& frame_min, const ImVec2& frame_max)
	{
		Draw(ImGui::GetWindowDrawList(), id,
			ImVec2(frame_min.x + kLeadingIconX, (frame_min.y + frame_max.y) * 0.5f),
			LeadingIconSize(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
	}

	void EndLeadingIcon()
	{
		ImGui::PopStyleVar();
	}

	float LeadingIconSpace()
	{
		return kLeadingIconPad;
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

		Draw(ImGui::GetWindowDrawList(), id, center, icon_size, color);

		if (tooltip != nullptr && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);

		ImGui::PopID();
		return clicked;
	}
}
