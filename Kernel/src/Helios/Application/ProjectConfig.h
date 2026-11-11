#pragma once

#include "Helios/Common/Export.h"
#include <cstdint>
#include <filesystem>
#include <string>

namespace Helios
{
	/* 画质开关：跟项目走，换台机器打开还是同一套。
	 * 存在 <项目根>/Project.xml，编辑器 Rendering 菜单与游戏示例读的是同一份。 */
	struct ProjectRendering
	{
		bool MSAA;
		bool GPUSkinning;
		/* 草叶位置由计算着色器生成（关掉退回 CPU 逐株生成） */
		bool GpuGrassPlacement;
		/* 草地走 GPU 剔除 + 间接绘制（关掉退回逐块提交） */
		bool GpuGrassIndirect;
		/* 顺带开启块掩码剔除：先压可见块表再逐株扫 */
		bool GpuGrassChunkMask;
		/* 草地按批提交：连续块按实例预算合成一次绘制 */
		bool GrassBatching;
		/* 全局草地密度系数：0 是全不画，1 是各片草地自己的设置 */
		float GrassDensityScale;
		/* 地形虚拟纹理：页池缓存地表颜色（关掉逐像素直接算） */
		bool TerrainVirtualTexture;
		/* 页池档位：页边长（纹素），128 / 256 / 512 */
		uint32_t TerrainVirtualTextureTexels;
	};

	/* 项目描述：<项目根>/Project.xml
	 * 只放跨机器要对齐的字段，本机的编辑器状态在 .helios/editor.cfg。 */
	struct ProjectConfig
	{
		std::string Name;
		std::string StartupScene; /* 相对项目根，空表示不自动打开 */
		ProjectRendering Rendering; /* 文件里没写的项取引擎当前值 */
	};

	/* 读当前项目的 Project.xml；文件缺失或解析失败时用目录名兜底，不影响启动 */
	HELIOS_API ProjectConfig LoadProjectConfig();

	/* 读指定目录的 Project.xml（启动页列最近工程时用，不必先切项目根） */
	HELIOS_API ProjectConfig LoadProjectConfig(const std::filesystem::path& project_dir);

	/* 写回 Project.xml：只改名字、启动场景与画质开关，文件里其它内容原样保留 */
	HELIOS_API bool SaveProjectConfig(const ProjectConfig& config);

	/* 把画质开关推给引擎：开项目、改开关都走这里 */
	HELIOS_API void ApplyProjectSettings(const ProjectConfig& config);

	/* 新建工程：建目录并写一份最小 Project.xml。目录已存在返回 false，不动现有内容。 */
	HELIOS_API bool CreateProject(const std::filesystem::path& project_dir, const std::string& name);
}
