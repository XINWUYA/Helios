#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>

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

	/* ---- 面板过滤词的文本匹配 ----
	 * 层级面板的实体搜索与资源浏览器的过滤共用这一处，语义保持一致。 */

	inline std::string ToLowercase(const char* text)
	{
		if (text == nullptr)
			return {};

		std::string result(text);
		std::transform(result.begin(), result.end(), result.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return result;
	}

	/* needle 必须是已经小写化的（用 ToLowercase 过一次）；
	 * needle 为空视为"全部命中"。 */
	inline bool ContainsCaseInsensitive(const char* haystack, const std::string& lowercase_needle)
	{
		if (haystack == nullptr)
			return false;

		if (lowercase_needle.empty())
			return true;

		std::string lowercase_haystack(haystack);
		std::transform(lowercase_haystack.begin(), lowercase_haystack.end(),
			lowercase_haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		return lowercase_haystack.find(lowercase_needle) != std::string::npos;
	}
}