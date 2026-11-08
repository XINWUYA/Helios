#pragma once
#include <deque>
#include <string>
#include <vector>
#include "Helios/Common/Common.h"
#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
	/* 清单条目：一份源码加一组 define */
	struct ShaderVariantItem
	{
		std::string Path;   /* 相对 EngineRes 的路径，Load 时补成绝对路径 */
		ShaderVariantDefines Defines{};
	};

	/*
	 * 变体清单与预热。
	 * 清单回答"这套内容到底用到哪些组合"，可从资源缓存现收，也能落成文本；
	 * 预热按清单逐帧补编，避免真机首次进场景卡在编译上。
	 */
	class ShaderVariantCatalog
	{
	public:
		static ShaderVariantCatalog& Instance();

		/* 从资源缓存收当前已加载的全部组合（按路径与 define 排序，帧间稳定） */
		[[nodiscard]] static std::vector<ShaderVariantItem> CollectLoaded();

		/* 清单文件：每行 "<相对路径>|A=1,B=0"，没维度写 "-" */
		static bool Save(const std::string& filepath, const std::vector<ShaderVariantItem>& items);
		[[nodiscard]] static std::vector<ShaderVariantItem> Load(const std::string& filepath);

		/* 排进预热队列：已有的组合直接跳过，只补没编过的 */
		void Enqueue(const std::vector<ShaderVariantItem>& items);
		/* 每帧最多编几个：一个变体几十到几百毫秒，默认一个 */
		void SetFrameBudget(uint32_t count) { m_FrameBudget = count; }

		/* 推一轮预热，Renderer::Update 每帧调一次 */
		void Tick();

		[[nodiscard]] size_t Pending() const { return m_Pending.size(); }
		[[nodiscard]] uint32_t Completed() const { return m_Completed; }
		[[nodiscard]] bool Empty() const { return m_Pending.empty(); }

	private:
		ShaderVariantCatalog() = default;

		std::deque<ShaderVariantItem> m_Pending;
		uint32_t m_FrameBudget{ 1 };
		uint32_t m_Completed{ 0 };
	};
}
