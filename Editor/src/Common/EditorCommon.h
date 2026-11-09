#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <imgui.h>
#include <string>

#include "Common/TextMatch.h"
#include "UI/EditorTheme.h"

namespace Helios
{
	/* ImGui 1.86 的 IsKeyPressed / IsKeyDown 收的是"后端键号"（KeyMap 里的值，如 GLFW 键码），
	 * ImGuiKey 只是 KeyMap 的下标 —— 直接把 ImGuiKey 传进去会读到别的键，所以统一经这里转一道。
	 * 映射不存在（KeyMap 为 -1）时返回 false。 */
	inline bool KeyPressed(ImGuiKey key, bool repeat = true)
	{
		return ImGui::IsKeyPressed(ImGui::GetKeyIndex(key), repeat);
	}

	inline bool KeyDown(ImGuiKey key)
	{
		return ImGui::IsKeyDown(ImGui::GetKeyIndex(key));
	}

	/* 执行状态：唯一真值在 PlaySession（见 Panels/SceneEditor/PlaySession）。
	 * 运行态当前世界是克隆出来的运行世界，编辑世界被冻结；退出直接丢弃运行世界 */
	enum class PlayState : uint8_t
	{
		Edit = 0,	/* 编辑：当前世界就是编辑世界 */
		Playing,	/* 运行：脚本与时间按帧推进 */
		Paused		/* 运行中暂停：模拟停、渲染照常 */
	};

	/* 渲染尺寸的安全像素区间（ClampRenderSize 使用）。
	 * 放在命名空间作用域：lambda 里 std::max 会取引用（odr-use），
	 * 若声明为函数内 constexpr 局部变量则会报"无法隐式捕获"。 */
	inline constexpr uint32_t kRenderSizeMin = 1;
	inline constexpr uint32_t kRenderSizeMax = 8192;

	/* 设计像素 → ImGui 单位：× EditorTheme::UiScale（Windows 高 DPI 放大、macOS 恒 1，
	 * 见 EditorTheme 顶部的 UI 缩放说明）。编辑器里一切"屏幕尺寸"常量 / 魔数都经它折算 ——
	 * 按设计值书写、喂给 ImGui 前过一道 Scaled()，高 DPI 自然跟上。
	 * 注：多数内置控件尺寸已由 style.ScaleAllSizes 兜底；这里用于自绘元素与需要精确
	 * 对齐的间距（缩略图网格、轴盘、流程图节点等）。 */
	inline float Scaled(float design_pixels)
	{
		return design_pixels * EditorTheme::UiScale();
	}

	/* ImGui 客户区单位 → 帧缓冲物理像素的比率（macOS = 2：ImGui 单位是逻辑点、渲染端本就乘它到像素；
	 * Windows = 1：窗口 = 帧缓冲 = 物理像素，UI 的尺寸放大由 EditorTheme::UiScale 层完成）。
	 * RT 分配 / 拾取坐标这类"从 ImGui 尺寸换算像素"的地方统一用它 —— 不要用窗口的 content scale：
	 * Windows 上再乘它相当于按物理像素二次放大（成倍超采样），两平台语义也从此对齐。 */
	inline float ImGuiToPhysicalScale()
	{
		const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
		return (std::isfinite(scale) && scale > 0.0f) ? scale : 1.0f;
	}

	/* 把「ImGui 尺寸 × 帧缓冲换算比率」折算成物理像素尺寸、钳到安全区间：窗口极小时 ImGui 的
	 * GetContentRegionMax() 可能小于 Min()、宽高差为负，直接转 uint32_t 会回绕成 ~42 亿（分配
	 * 巨型 RenderTarget、甚至崩溃）；上限挡的是异常输入（NaN / 极大值）的显存爆炸。 */
	inline glm::uvec2 ClampRenderSize(float imgui_width, float imgui_height, float to_physical_scale)
	{
		const auto clamp_axis = [](float v) -> uint32_t
		{
			if (!std::isfinite(v) || v <= 0.0f)
				return kRenderSizeMin;
			const auto px = static_cast<uint32_t>(v + 0.5f);
			return std::min(std::max(px, kRenderSizeMin), kRenderSizeMax);
		};

		return { clamp_axis(imgui_width * to_physical_scale),
		         clamp_axis(imgui_height * to_physical_scale) };
	}
}