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

		/* 第二档色：同色、低透明度（双色调图标用）。
		 * 仍然从调用方传入的颜色派生，所以"禁用变淡 / 选中浸染强调色"这些行为不受影响。 */
		inline ImU32 Soften(ImU32 color, float alpha_scale)
		{
			const ImU32 alpha = static_cast<ImU32>(static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha_scale);
			return (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
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
				Soften(color, 0.40f));

			StrokePolyline(dl, c, kPage, thickness, color, true);

			dl->AddLine(c.At(0.60f, 0.06f), c.At(0.60f, 0.28f), color, thickness);
			dl->AddLine(c.At(0.60f, 0.28f), c.At(0.82f, 0.28f), color, thickness);
		}

		void DrawNewScene(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			/* 空白文档 = 只有页，没有内容线 */
			StrokePage(dl, Canvas{ center, size }, StrokeWidth(size), color);
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

			dl->AddLine(c.At(0.50f, 0.16f), c.At(0.50f, 0.84f), color, t);
			dl->AddLine(c.At(0.16f, 0.50f), c.At(0.84f, 0.50f), color, t);
		}

		void DrawRemove(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };

			dl->AddLine(c.At(0.16f, 0.50f), c.At(0.84f, 0.50f), color, StrokeWidth(size));
		}

		void DrawReturn(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 敞口箭头（chevron）+ 横线：现代"返回"造型 —— 比实心三角轻，
			 * 行高附近也不会糊成一坨。 */
			dl->PathLineTo(c.At(0.44f, 0.26f));
			dl->PathLineTo(c.At(0.18f, 0.50f));
			dl->PathLineTo(c.At(0.44f, 0.74f));
			dl->PathStroke(color, 0, t);

			dl->AddLine(c.At(0.18f, 0.50f), c.At(0.88f, 0.50f), color, t);
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

		/* ---- 场景树节点 ----
		 * "场景 / 实体类型"图标，用在层级树节点前面；只描必要的外形、细部靠实心小图元点缀，
		 * 正文行高（约 14px）下也能一眼区分。 */

		void DrawScene(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 透视地面 = 一片"世界"：面填一层淡色 + 网格线，比纯描边有实体感。
			 * 造型上与模型（线框方块）、图片精灵（画框 + 山）都不撞。 */
			static constexpr float kFloor[4][2] = {
				{ 0.32f, 0.42f }, { 0.68f, 0.42f }, { 0.96f, 0.90f }, { 0.04f, 0.90f }
			};
			FillPoly(dl, c, kFloor, Soften(color, 0.30f));
			StrokePolyline(dl, c, kFloor, t, color, true);

			dl->AddLine(c.At(0.40f, 0.42f), c.At(0.28f, 0.90f), color, t);
			dl->AddLine(c.At(0.60f, 0.42f), c.At(0.72f, 0.90f), color, t);
			dl->AddLine(c.At(0.19f, 0.66f), c.At(0.81f, 0.66f), color, t);
		}

		void DrawCamera(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 机身 + 右侧楔形镜头 */
			dl->AddRect(c.At(0.06f, 0.30f), c.At(0.62f, 0.86f), color, c.Len(0.10f), 0, t);

			static constexpr float kLens[4][2] = {
				{ 0.62f, 0.44f }, { 0.94f, 0.24f }, { 0.94f, 0.92f }, { 0.62f, 0.72f }
			};
			StrokePolyline(dl, c, kLens, t, color, true);
		}

		void DrawDirectionalLight(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 太阳 + 三束等长等距的平行光：区分于点光（四向短射线）、聚光（锥形）。
			 * 三束用同一个方向 + 垂直偏移量算出来，才不会像随手画的三条斜线。 */
			const ImVec2 sun = c.At(0.30f, 0.28f);
			dl->AddCircle(sun, c.Len(0.16f), color, 0, t);

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
			dl->AddCircle(c.At(0.50f, 0.50f), c.Len(0.22f), color, 0, t);

			dl->AddLine(c.At(0.50f, 0.04f), c.At(0.50f, 0.18f), color, t);
			dl->AddLine(c.At(0.50f, 0.82f), c.At(0.50f, 0.96f), color, t);
			dl->AddLine(c.At(0.04f, 0.50f), c.At(0.18f, 0.50f), color, t);
			dl->AddLine(c.At(0.82f, 0.50f), c.At(0.96f, 0.50f), color, t);
		}

		void DrawSpotLight(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 灯源 + 向下张开的光锥（不封底，才读得出是"投出去的光"） */
			dl->AddCircleFilled(c.At(0.50f, 0.16f), c.Len(0.11f), color);

			dl->AddLine(c.At(0.40f, 0.32f), c.At(0.14f, 0.92f), color, t);
			dl->AddLine(c.At(0.60f, 0.32f), c.At(0.86f, 0.92f), color, t);
		}

		void DrawReflectionProbe(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 球体 + 左上高光弧 + 反射亮点 */
			dl->AddCircle(c.At(0.50f, 0.50f), c.Len(0.40f), color, 0, t);

			dl->PathArcTo(c.At(0.50f, 0.50f), c.Len(0.22f), 3.34f, 4.56f, 16);
			dl->PathStroke(color, 0, t);

			dl->AddCircleFilled(c.At(0.66f, 0.66f), c.Len(0.08f), color);
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
			FillPoly(dl, c, kTop, Soften(color, 0.26f));
			StrokePolyline(dl, c, kCube, t, color, true);

			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.50f, 0.08f), color, t);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.92f, 0.70f), color, t);
			dl->AddLine(c.At(0.50f, 0.50f), c.At(0.08f, 0.70f), color, t);
		}

		void DrawSprite(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 画框 + 山 + 太阳：图片图标的通用语言 */
			dl->AddRect(c.At(0.08f, 0.16f), c.At(0.92f, 0.84f), color, c.Len(0.08f), 0, t);

			dl->AddTriangleFilled(c.At(0.18f, 0.74f), c.At(0.46f, 0.40f), c.At(0.72f, 0.74f), color);
			dl->AddCircleFilled(c.At(0.70f, 0.34f), c.Len(0.08f), color);
		}

		void DrawEntity(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);

			/* 只有 Transform 的实体没有可视形态：用取景框四角表示"一个空对象"。
			 * 同一个直角括号镜像四次，形状只有一份定义。 */
			static constexpr float kBracket[3][2] = {
				{ 0.12f, 0.36f }, { 0.12f, 0.12f }, { 0.36f, 0.12f }
			};

			for (int corner = 0; corner < 4; ++corner)
			{
				const float flip_x = (corner & 1) ? -1.0f : 1.0f;
				const float flip_y = (corner & 2) ? -1.0f : 1.0f;

				for (int i = 0; i < 3; ++i)
				{
					const float x = 0.5f + flip_x * (kBracket[i][0] - 0.5f);
					const float y = 0.5f + flip_y * (kBracket[i][1] - 0.5f);
					dl->PathLineTo(c.At(x, y));
				}

				dl->PathStroke(color, 0, t);
			}
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
			FillPoly(dl, c, kTag, Soften(color, 0.26f));
			StrokePolyline(dl, c, kTag, StrokeWidth(size), color, true);

			dl->AddCircleFilled(c.At(0.31f, 0.50f), c.Len(0.095f), color);
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
			FillPoly(dl, c, kTab, Soften(color, 0.26f));
			FillPoly(dl, c, kBody, Soften(color, 0.42f));

			/* 外形（正视图 + 标签页）：与工具栏里纯描边的"打开"区分开 */
			static constexpr float kFolder[6][2] = {
				{ 0.07f, 0.83f }, { 0.07f, 0.19f }, { 0.35f, 0.19f },
				{ 0.46f, 0.34f }, { 0.93f, 0.34f }, { 0.93f, 0.83f }
			};
			StrokePolyline(dl, c, kFolder, t, color, true);
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

			dl->AddTriangleFilled(c.At(0.28f, 0.82f), c.At(0.50f, 0.53f), c.At(0.72f, 0.82f), color);
			dl->AddTriangleFilled(c.At(0.50f, 0.82f), c.At(0.66f, 0.62f), c.At(0.82f, 0.82f), Soften(color, 0.55f));
			dl->AddCircleFilled(c.At(0.68f, 0.44f), c.Len(0.085f), color);
		}

		void DrawFileMtlGraph(ImDrawList* dl, const ImVec2& center, float size, ImU32 color)
		{
			const Canvas c{ center, size };
			const float t = StrokeWidth(size);
			const float rounding = c.Len(0.06f);

			/* 材质图 = 一个带输入 / 输出端口的节点（用过节点编辑器的都认得）：
			 * 方框填淡色 + 左右两个实心端口 + 两小段连线。
			 * 别用"三个点连两条线"——那个造型是"分享"，反而认不出是节点图。 */
			dl->AddRectFilled(c.At(0.30f, 0.24f), c.At(0.70f, 0.76f), Soften(color, 0.30f), rounding);
			dl->AddRect(c.At(0.30f, 0.24f), c.At(0.70f, 0.76f), color, rounding, 0, t);

			dl->AddLine(c.At(0.14f, 0.50f), c.At(0.30f, 0.50f), color, t);
			dl->AddLine(c.At(0.70f, 0.50f), c.At(0.86f, 0.50f), color, t);

			dl->AddCircleFilled(c.At(0.13f, 0.50f), c.Len(0.095f), color);
			dl->AddCircleFilled(c.At(0.87f, 0.50f), c.Len(0.095f), color);
		}

		/* 元数据表：Id 与表项一一对应（顺序必须与 Id 枚举一致）*/
		constexpr IconDesc s_Icons[] = {
			{ "None",              nullptr },
			{ "NewScene",          &DrawNewScene },
			{ "OpenScene",         &DrawOpenScene },
			{ "Save",              &DrawSave },
			{ "Undo",              &DrawUndo },
			{ "Redo",              &DrawRedo },
			{ "Translate",         &DrawTranslate },
			{ "Rotate",            &DrawRotate },
			{ "Scale",             &DrawScale },
			{ "Play",              &DrawPlay },
			{ "Stop",              &DrawStop },
			{ "Menu",              &DrawMenu },
			{ "Add",               &DrawAdd },
			{ "Remove",            &DrawRemove },
			{ "Return",            &DrawReturn },
			{ "Filter",            &DrawFilter },
			{ "Visible",           &DrawVisible },
			{ "Scene",             &DrawScene },
			{ "Camera",            &DrawCamera },
			{ "LightDirectional",  &DrawDirectionalLight },
			{ "LightPoint",        &DrawPointLight },
			{ "LightSpot",         &DrawSpotLight },
			{ "ReflectionProbe",   &DrawReflectionProbe },
			{ "Model",             &DrawModel },
			{ "Sprite",            &DrawSprite },
			{ "Entity",            &DrawEntity },
			{ "Transform",         &DrawTransform },
			{ "Tag",               &DrawTag },
			{ "Stats",             &DrawStats },
			{ "Directory",         &DrawDirectory },
			{ "File",              &DrawFile },
			{ "FileImage",         &DrawFileImage },
			{ "FileScene",         &DrawScene },
			{ "FileMtlGraph",      &DrawFileMtlGraph },
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
