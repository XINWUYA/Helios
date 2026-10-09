#pragma once
#include "Helios/VirtualDevice/DeviceQueryNode.h"
#include "Helios/VirtualDevice/DeviceQueryBuffer.h"

namespace Helios
{
	/* Query Context类
	 * 包含一帧的QueryNode的信息
	 * 提前分配200个，当超过时，再根据需要增加
	 */
	class RenderQueryContext
	{
	public:
		void Init();
		void Destroy();

		void BeginFrame(uint32_t frame_id);
		void EndFrame();

		void BeginGPUScope(const char* label);
		void EndGPUScope();

		/* 渲染通道开始（由计时器转发；clear_only = 计时停用/不采样，只清理描述符） */
		void OnRenderPassBegin(void* native_render_pass_descriptor, bool clear_only);
		/* 渲染通道结束（编码器已关闭） */
		void OnRenderPassEnd();

		bool PrepareQueryResult();

	private:
		static constexpr uint8_t DEFAULT_QUERY_COUNT = 32; // 预留32个QuaryNode
		std::vector<DeviceQueryNode*> m_QueryNodes{};
		uint32_t m_RootNodeIndex{ INVALID_QUERY_NODE_INDEX };
		uint32_t m_CurrentNodeIndex{ INVALID_QUERY_NODE_INDEX };
		uint32_t m_UsedNodeIndex{ 0 };
		uint32_t m_FrameIndex{ 0 };
		bool m_IsValid{ false }; /* 标记当前Context是否已被使用 */

		/* 当前打开的作用域链（根 → 当前节点）：通道开始通知沿它逐个派发 */
		std::vector<uint32_t> m_OpenNodeIndices{};

		/* 通道采样缓冲（仅通道采样后端创建；OpenGL 为空） */
		DeviceQueryBuffer* m_pQueryBuffer{ nullptr };
		/* 本帧解析出的通道工作时长表（与 m_pQueryBuffer 配套） */
		std::vector<GPUPassTimestamp> m_PassTimestamps{};

		friend class RenderQueryProfiler;
	};

	/* 收集结果信息 */
	struct ResultGPUTimerNode
	{
		std::string Label{ "" };
		double GPUTime = 0.0;
		/* 顶点 / 片元阶段的细分（与 GPUTime 同系数平滑；其余后端保持 0） */
		double GPUTimeVertex = 0.0;
		double GPUTimeFragment = 0.0;
		/* 本帧是否有有效数据（无数据节点由 UI 以占位显示；GPUTime 保留上次平滑值） */
		bool HasData = false;
		uint32_t QueryIndex = INVALID_QUERY_NODE_INDEX;
		std::vector<ResultGPUTimerNode> Children{};
	};

	/*
	 * 用于统计GPUPass耗时信息
	 */
	class RenderQueryProfiler
	{
	public:
		~RenderQueryProfiler();
		/* 单例 */
		static RenderQueryProfiler& Instance();

		void Release();

		void BeginFrame(uint32_t frame_id);
		void EndFrame();

		void BeginGPUScope(const char* label);
		void EndGPUScope();

		/* 渲染通道即将开始（后端在创建编码器前调用；native 描述符由查询缓冲解释）。
		 * 通道采样后端必须调用 —— 无论是否在计时，后端实现都会把描述符上的旧附件清掉。 */
		void OnRenderPassBegin(void* native_render_pass_descriptor);

		/* 渲染通道结束（编码器已关闭；后端在每个渲染编码器结束处调用）。
		 * 用于清掉"当前打开的通道"标记 —— 作用域在通道内开启时要靠它做补绑。 */
		void OnRenderPassEnd();

		bool PrepareQueryResult(ResultGPUTimerNode& root_node);

		/* 开关：控制RenderQuery是否工作 */
		void SetEnabled(bool enabled) { m_IsEnabled = enabled; }
		bool IsEnabled() const { return m_IsEnabled; }

	private:
		RenderQueryProfiler();

		static constexpr uint8_t MAX_CONTEXTS = 4;
		std::vector<RenderQueryContext> m_QueryContexts;
		uint8_t m_ReadContextId{ 0 };
		uint8_t m_WriteContextId{ 1 };
		bool m_IsEnabled{ true }; /* 是否启用GPU耗时统计 */

	};

}
