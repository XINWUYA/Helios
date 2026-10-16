#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>
#include <imgui.h>
#include "Helios/Common/Common.h"

namespace Helios
{
	/* SVG 工具：路径几何（图标系统与资源预览共用一份）＋ 资源面板的矢量预览
	 * （恒按文件原色绘制——预览是通用查看器，图标那套画布换档不适用于它，
	 * 将来的非图标 svg 资源也一样；按 viewBox 装框，带 (路径, 写入时间) 缓存）。
	 * 元素认 g / path / circle / rect，颜色认 none / #rgb / #rrggbb / black / white。 */
	namespace Svg
	{
		/* 折线轮廓（viewBox 坐标系）。Closed = 路径里有显式 Z ——
		 * 图标源的填充面常不带 Z，绘制方按"有填充就闭合"补上（见各自的画法）。 */
		struct Contour
		{
			std::vector<ImVec2> Points;
			bool Closed = false;
		};

		/* path 的 d 串 -> 折线：C/S/Q/T 采样成折线，A 转成弧段点 */
		std::vector<Contour> ParsePath(const char* data);

		/* 凹多边形填充（耳剪切，逐三角形提交） */
		void FillPolygon(ImDrawList* draw_list, const std::vector<ImVec2>& points, ImU32 color);

		/* 折线描边：实线一次走完，虚线按 on/off 逐段画、圆头在每段两端补圆点。
		 * 颜色 / 线宽 / 虚线段长由调用方按屏幕尺度算好（图标走调色板，预览走原色）。 */
		void StrokePolyline(ImDrawList* draw_list, const std::vector<ImVec2>& points, bool closed,
			ImU32 color, float width, float dash_on, float dash_off, bool round_cap, bool round_join);

		/* ---- 资源面板的矢量预览 ---- */

		/* 一笔画法：none 或一个实色（认不出的颜色保持继承，与图标源的容错同口径） */
		struct Paint
		{
			bool  None = true;
			ImU32 Color = 0;
		};

		struct Style
		{
			Paint Stroke;
			Paint Fill;
			float StrokeWidth = 1.0f;
			float DashOn = 0.0f;
			float DashOff = 0.0f;
			bool  RoundCap = false;
			bool  RoundJoin = false;
		};

		struct Shape
		{
			enum class Type : uint8_t { Path, Circle, Rect };
			Type  Kind = Type::Path;
			Style Style;
			std::vector<Contour> Contours;   /* Path 用 */
			float X = 0.0f;                  /* Circle 圆心 / Rect 左上角 */
			float Y = 0.0f;
			float Width = 0.0f;
			float Height = 0.0f;
			float Radius = 0.0f;             /* Circle 半径 / Rect 圆角 */
		};

		struct Image
		{
			std::vector<Shape> Shapes;
			ImVec2 ViewBoxMin{ 0.0f, 0.0f };
			ImVec2 ViewBoxSize{ 32.0f, 32.0f };
		};

		/* 读一个 SVG 文件（只看文件写入时间，外部改过就重新解析）。
		 * 解析失败 / 不是 SVG 返回 nullptr —— 失败结果同样缓存，不会每帧重试。 */
		SharedPtr<const Image> GetImage(const std::filesystem::path& path);

		/* 把图按 viewBox 等比装进矩形（contain + 居中；两边 y 都朝下，不翻转） */
		void DrawImage(ImDrawList* draw_list, const Image& image, const ImVec2& rect_min, const ImVec2& rect_max);
	}
}
