#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
	/* 「Shader Variants」面板：按着色器列出源码声明的变体维度、已经加载的变体、
	 * 各自的 define 与加载耗时。数据直接取资源缓存，不另存统计。
	 * 变体数量、拼错的维度名、某次改 shader 后是不是全量重编，都在这里看。 */
	class ShaderVariantPanel
	{
	public:
		void OnImGuiRender();

		/* 一个已加载变体的展示行（纯数据，headless 可断言） */
		struct VariantRow
		{
			std::string Defines;   /* "A=1 B=0"；全默认时显示 (default) */
			std::string Hash;      /* 内容哈希前 8 位 */
			double ConvertMs{ 0.0 };  /* 读文件加 GLSL 转目标语言；命中磁盘缓存时接近 0 */
			double DeviceBuildMs{ 0.0 }; /* 驱动侧编译（MTLLibrary / 计算管线），首次绘制才发生 */
			uint32_t Generation{ 1 };
			bool IsCompute{ false };
		};

		/* 一个着色器的展示分组 */
		struct ShaderGroup
		{
			std::string Path;
			std::string DeclaredDims; /* "A (external), B (u_NormalTexture)" */
			std::vector<VariantRow> Variants;
		};

		/* define 表拼成可读串（键有序），空表返回空串 */
		static std::string FormatDefines(const ShaderVariantDefines& defines);

	private:
		/* 从资源缓存收集并按路径分组（排好序，帧间顺序稳定） */
		static void Collect(std::vector<ShaderGroup>& groups);
	};
}
