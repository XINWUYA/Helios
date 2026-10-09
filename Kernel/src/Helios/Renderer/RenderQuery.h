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
		/* 顶点 / 片元阶段的细分（与 GPUTime 同系数平滑；仅通道采样后端提供，
		 * 其余后端保持 0 —— 是否可用看 HasStageSplit） */
		double GPUTimeVertex = 0.0;
		double GPUTimeFragment = 0.0;
		/* 本帧是否有有效数据（无数据节点由 UI 以占位显示；GPUTime 保留上次平滑值） */
		bool HasData = false;
		/* 本节点的 v/f 细分是否可用（随节点能力标记；桌面 GL 恒假，面板不显示细分） */
		bool HasStageSplit = false;
		uint32_t QueryIndex = INVALID_QUERY_NODE_INDEX;
		std::vector<ResultGPUTimerNode> Children{};
	};

	/* GPU 耗时录制（逐帧分析的数据源）：录制期间每解析出一帧就追加该帧的整棵结果树（逐帧原始
	 * 值、不做 EMA 平滑）；达到容量自动停止、已录内容保留；容量硬编码（600 帧 ≈ 1 MB 量级）。 */
	class GPUTimerRecorder
	{
	public:
		static constexpr uint32_t MAX_FRAMES = 600;

		/* 开始录制（清空已有内容）；capacity 供检查程序用小容量验证录满自动停 */
		void Start(uint32_t capacity = MAX_FRAMES)
		{
			m_Capacity = capacity < MAX_FRAMES ? capacity : MAX_FRAMES;
			m_Frames.clear();
			m_FrameIds.clear();
			m_Recording = true;
		}

		/* 手动停止（已录内容保留） */
		void Stop() { m_Recording = false; }

		void Clear()
		{
			m_Frames.clear();
			m_FrameIds.clear();
			m_Recording = false;
		}

		bool IsRecording() const { return m_Recording; }
		uint32_t GetFrameCount() const { return static_cast<uint32_t>(m_Frames.size()); }
		uint32_t GetFrameId(uint32_t index) const { return index < m_FrameIds.size() ? m_FrameIds[index] : 0; }
		const ResultGPUTimerNode& GetFrame(uint32_t index) const { return m_Frames[index]; }

		/* 录制回调（RenderQueryProfiler 每解析出一帧调用）：返回本帧的填写目标；
		 * 未在录制或已录满时返回 nullptr（录满的那一帧仍完成填充后自动停止）。 */
		ResultGPUTimerNode* BeginFrame(uint32_t frame_id)
		{
			if (!m_Recording || m_Frames.size() >= m_Capacity)
			{
				m_Recording = false;
				return nullptr;
			}

			m_FrameIds.emplace_back(frame_id);
			m_Frames.emplace_back();
			if (m_Frames.size() >= m_Capacity)
				m_Recording = false;

			return &m_Frames.back();
		}

	private:
		std::vector<ResultGPUTimerNode> m_Frames{};
		std::vector<uint32_t> m_FrameIds{};
		uint32_t m_Capacity{ MAX_FRAMES };
		bool m_Recording{ false };
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

		/* GPU 耗时录制（面板 Record/Stop/Clear 控制；逐帧回放的数据源） */
		GPUTimerRecorder& GetRecorder() { return m_Recorder; }

	private:
		RenderQueryProfiler();

		static constexpr uint8_t MAX_CONTEXTS = 4;
		std::vector<RenderQueryContext> m_QueryContexts;
		uint8_t m_ReadContextId{ 0 };
		uint8_t m_WriteContextId{ 1 };
		bool m_IsEnabled{ true }; /* 是否启用GPU耗时统计 */
		GPUTimerRecorder m_Recorder{};

	};

}
