#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
	/* 「Shader Variants」面板：按着色器列出源码声明的变体维度、已经加载的变体、
	 * 各自的 define 与加载耗时。数据直接取资源缓存，不另存统计。
	 * 顶部一行搜索与筛选，着色器按卡片分组，组与变体都能按耗时排序。 */
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
			double TotalMs{ 0.0 };    /* 上面两项之和，排序用 */
			uint32_t Generation{ 1 };
			bool IsCompute{ false };
		};

		/* 一个着色器的展示分组 */
		struct ShaderGroup
		{
			std::string Path;
			std::string DeclaredDims; /* "A (draw), B (view), C (u_NormalTexture)" */
			/* 组内全部变体的耗时之和，用来找最贵的那份着色器 */
			double TotalMs{ 0.0 };
			std::vector<VariantRow> Variants;
		};

		/* 筛选条件：变体数按组判，计算 / 驱动编译按变体逐行筛 */
		struct FilterOptions
		{
			bool MultiVariantOnly{ false };
			bool ComputeOnly{ false };
			bool DeviceBuiltOnly{ false };
		};

		/* define 表拼成可读串（键有序），空表返回空串 */
		static std::string FormatDefines(const ShaderVariantDefines& defines);

		/* 按搜索词与筛选条件裁剪（组 / 变体两级），再按选定顺序排好，返回保留的变体总数。
		 * 空搜索词 + 全关 = 只做排序。 */
		static size_t ApplyFilter(std::vector<ShaderGroup>& groups, const FilterOptions& options,
			const std::string& lowercase_needle, bool sort_by_time);

	private:
		/* 从资源缓存收集并按路径分组（排好序，帧间顺序稳定） */
		static void Collect(std::vector<ShaderGroup>& groups);

		void ShowToolbar();
		void ShowFilterPopup();
		void ShowSummary(size_t shown_shaders, size_t total_shaders, size_t shown_variants,
			size_t total_variants, size_t multi_variant);
		void ShowGroupCard(const ShaderGroup& group, bool force_open);

		[[nodiscard]] bool HasActiveFilters() const;

		/* 搜索词：原样留在输入框里，小写副本给过滤用 */
		char m_Search[128]{};
		std::string m_Filter;
		FilterOptions m_Filters;
		/* 筛选弹层的锚点 = 按钮右下角（弹层贴它正下方、右缘对齐） */
		float m_FilterMenuAnchorX{ 0.0f };
		float m_FilterMenuAnchorY{ 0.0f };
		/* 按耗时从大到小排，方便先看最贵的；默认还是按路径 */
		bool m_SortByTime{ false };
	};
}
