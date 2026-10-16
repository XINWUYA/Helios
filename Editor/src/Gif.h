#pragma once
#include <filesystem>
#include <vector>
#include <imgui.h>
#include "Helios/Common/Common.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	/* GIF 动图预览：全部帧竖排进一张图集纹理（帧 0 在最上），外加每帧的时长累计。
	 * 时刻到帧号是纯函数（循环播放）—— 浏览器网格与属性卡共用一份，两边播到哪帧都一样。
	 * headless（没有渲染后端）时纹理为空，调用方回落到静态首帧 / 类型图标。 */
	namespace Gif
	{
		struct Clip
		{
			SharedPtr<DeviceTexture> Texture;   /* 图集：帧 k 在 v = k/N..(k+1)/N；无后端时为空 */
			int   FrameCount = 0;
			float FrameWidth = 0.0f;
			float FrameHeight = 0.0f;
			/* 每帧起点（秒）：[i]..[i+1] 是第 i 帧；末项 = 总时长 */
			std::vector<float> FrameStarts;
		};

		/* 读一个 .gif（按 (路径, 写入时间) 缓存）。非 .gif / 解析失败返回空 —— 失败也缓存，不重试。 */
		SharedPtr<const Clip> GetClip(const std::filesystem::path& path);

		/* 时刻 → 帧号（对总时长取余循环；段界归下一帧） */
		int FrameAt(const Clip& clip, float seconds);

		/* 该帧在图集里的取图窗口（含上下翻转，与静态缩略图 / 预览的采样方向一致） */
		void FrameUv(const Clip& clip, int frame, ImVec2& uv0, ImVec2& uv1);
	}
}
