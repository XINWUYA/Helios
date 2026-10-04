#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace Helios
{
	/* 执行模式 */
	enum class PlayMode : uint8_t
	{
		Edit = 0,	/* 编辑模式 */
		Runtime		/* 运行模式 */
	};

	/* 渲染尺寸的安全像素区间（ClampRenderSize 使用）。
	 * 放在命名空间作用域：lambda 里 std::max 会取引用（odr-use），
	 * 若声明为函数内 constexpr 局部变量则会报"无法隐式捕获"。 */
	inline constexpr uint32_t kRenderSizeMin = 1;
	inline constexpr uint32_t kRenderSizeMax = 8192;

	/* 把「逻辑点尺寸 × 内容缩放」换算成像素尺寸、钳到安全区间：窗口极小时 ImGui 的
	 * GetContentRegionMax() 可能小于 Min()、宽高差为负，直接转 uint32_t 会回绕成 ~42 亿（分配
	 * 巨型 RenderTarget、甚至崩溃）；上限挡的是异常输入（NaN / 极大值）的显存爆炸。 */
	inline glm::uvec2 ClampRenderSize(float logical_width, float logical_height, float content_scale)
	{
		const auto clamp_axis = [](float v) -> uint32_t
		{
			if (!std::isfinite(v) || v <= 0.0f)
				return kRenderSizeMin;
			const auto px = static_cast<uint32_t>(v + 0.5f);
			return std::min(std::max(px, kRenderSizeMin), kRenderSizeMax);
		};

		return { clamp_axis(logical_width * content_scale),
		         clamp_axis(logical_height * content_scale) };
	}
}