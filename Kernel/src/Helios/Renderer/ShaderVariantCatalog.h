#pragma once
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
	 * 变体清单。
	 * 回答"这套内容到底用到哪些组合"：可从资源缓存现收，也能落成文本再读回来。
	 * 预热交给 ShaderVariantStreamer 的异步队列，不在这里攒批次。
	 */
	class ShaderVariantCatalog
	{
	public:
		/* 从资源缓存收当前已加载的全部组合（按路径与 define 排序，帧间稳定） */
		[[nodiscard]] static std::vector<ShaderVariantItem> CollectLoaded();

		/* 清单文件：每行 "<相对路径>|A=1,B=0"，没维度写 "-" */
		static bool Save(const std::string& filepath, const std::vector<ShaderVariantItem>& items);
		[[nodiscard]] static std::vector<ShaderVariantItem> Load(const std::string& filepath);

		/* 按清单预热：编过的跳过，其余交给异步编译，不会卡住当前帧 */
		static void Enqueue(const std::vector<ShaderVariantItem>& items);
	};
}
