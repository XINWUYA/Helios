#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace Helios::TerrainSampling
{
	/* 地形高度采样的纯函数版本：CPU 同步路径、后台重建任务、以及对应的 GLSL
	 * （TerrainCommon.glsl / GrassPlacement.glsl）必须用同一套公式。
	 * heights 是归一化 0..1 的原始数组，行主序。 */

	inline bool Contains(const float* heights, uint32_t resolution, const glm::vec3& origin,
		const glm::vec2& extent, const glm::vec2& world_xz)
	{
		if (heights == nullptr || resolution < 2 || extent.x <= 0.0f || extent.y <= 0.0f)
			return false;

		const glm::vec2 uv = (world_xz - glm::vec2(origin.x, origin.z)) / (extent * 2.0f) + 0.5f;
		return uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
	}

	inline bool SampleHeight(const float* heights, uint32_t resolution, const glm::vec3& origin,
		const glm::vec2& extent, float height_scale, const glm::vec2& world_xz, float& out_height)
	{
		if (!Contains(heights, resolution, origin, extent, world_xz))
			return false;

		const glm::vec2 uv = (world_xz - glm::vec2(origin.x, origin.z)) / (extent * 2.0f) + 0.5f;
		const float max_index = static_cast<float>(resolution - 1);
		const float fx = std::clamp(uv.x * max_index, 0.0f, max_index);
		const float fz = std::clamp(uv.y * max_index, 0.0f, max_index);

		const uint32_t x0 = static_cast<uint32_t>(fx);
		const uint32_t z0 = static_cast<uint32_t>(fz);
		const uint32_t x1 = std::min(x0 + 1, resolution - 1);
		const uint32_t z1 = std::min(z0 + 1, resolution - 1);
		const float tx = fx - static_cast<float>(x0);
		const float tz = fz - static_cast<float>(z0);

		const auto at = [heights, resolution](uint32_t x, uint32_t z)
		{
			return heights[static_cast<size_t>(z) * resolution + x];
		};

		const float h = glm::mix(glm::mix(at(x0, z0), at(x1, z0), tx),
			glm::mix(at(x0, z1), at(x1, z1), tx), tz);
		out_height = origin.y + h * height_scale;
		return true;
	}

	/* morph 的粗表面：在 2×step 的网格上取样再线性插值，网格锚在地形原点上。
	 * 与 TerrainCommon.glsl 的 TerrainHeightCoarse 同一套（改一处要同步）。 */
	inline bool SampleHeightCoarse(const float* heights, uint32_t resolution, const glm::vec3& origin,
		const glm::vec2& extent, float height_scale, const glm::vec2& world_xz, float step,
		float& out_height)
	{
		if (heights == nullptr || step <= 0.0f)
			return false;
		if (!Contains(heights, resolution, origin, extent, world_xz))
			return false;

		const glm::vec2 anchor(origin.x, origin.z);
		const glm::vec2 local = (world_xz - anchor) / step;
		const glm::vec2 cell = glm::floor(local);
		const glm::vec2 frac = local - cell;
		const glm::vec2 base = anchor + cell * step;

		float h00 = 0.0f;
		float h10 = 0.0f;
		float h01 = 0.0f;
		float h11 = 0.0f;
		if (!SampleHeight(heights, resolution, origin, extent, height_scale, base, h00)
			|| !SampleHeight(heights, resolution, origin, extent, height_scale, base + glm::vec2(step, 0.0f), h10)
			|| !SampleHeight(heights, resolution, origin, extent, height_scale, base + glm::vec2(0.0f, step), h01)
			|| !SampleHeight(heights, resolution, origin, extent, height_scale, base + glm::vec2(step), h11))
		{
			return false;
		}

		out_height = glm::mix(glm::mix(h00, h10, frac.x), glm::mix(h01, h11, frac.x), frac.y);
		return true;
	}

	inline bool SampleNormal(const float* heights, uint32_t resolution, const glm::vec3& origin,
		const glm::vec2& extent, float height_scale, const glm::vec2& world_xz, glm::vec3& out_normal)
	{
		if (!Contains(heights, resolution, origin, extent, world_xz))
			return false;

		/* 差分步长取两个纹素：再小会被高度图的高频细节抖成麻点 */
		const float texel = (extent.x * 2.0f) / static_cast<float>(std::max(1u, resolution - 1));
		const float step = std::max(texel * 2.0f, 0.01f);

		float hl = 0.0f;
		float hr = 0.0f;
		float hd = 0.0f;
		float hu = 0.0f;
		if (!SampleHeight(heights, resolution, origin, extent, height_scale,
				world_xz - glm::vec2(step, 0.0f), hl)
			|| !SampleHeight(heights, resolution, origin, extent, height_scale,
				world_xz + glm::vec2(step, 0.0f), hr)
			|| !SampleHeight(heights, resolution, origin, extent, height_scale,
				world_xz - glm::vec2(0.0f, step), hd)
			|| !SampleHeight(heights, resolution, origin, extent, height_scale,
				world_xz + glm::vec2(0.0f, step), hu))
		{
			return false;
		}

		out_normal = glm::normalize(glm::vec3(hl - hr, 2.0f * step, hd - hu));
		return true;
	}
}
