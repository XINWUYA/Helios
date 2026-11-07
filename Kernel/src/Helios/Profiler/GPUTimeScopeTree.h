#pragma once
#include "Helios/Profiler/Profiler.h"
#include "Helios/Renderer/GPUScopeInterface.h"
#include "Helios/VirtualDevice/DeviceQueryNode.h"
#include "Helios/VirtualDevice/DeviceQueryBuffer.h"

namespace Helios
{
	/* 一帧的查询节点：预分配 32 个，不够再按需扩 */
	class GPUQueryContext
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

		/* 计算通道开始（派发前转发）：与渲染通道共用槽位，时间戳计入当前打开的作用域 */
		void OnComputePassBegin(void* native_compute_pass_descriptor, bool clear_only);

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

		friend class GPUTimeScopeTree;
	};

	/*
	 * 用于统计GPUPass耗时信息
	 */
	class HELIOS_API GPUTimeScopeTree : public GPUScopeInterface
	{
	public:
		~GPUTimeScopeTree();
		/* 单例 */
		static GPUTimeScopeTree& Instance();

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

		/* 计算通道即将开始（后端在建计算编码器前调用）：挂上时间戳采样点，
		 * 让纯计算的 Pass（如 GPU 蒙皮）也能统计到耗时。 */
		void OnComputePassBegin(void* native_compute_pass_descriptor);

		/* 解析已完成的查询并结算实时树；返回这一帧是否有新的 GPU 结果 */
		bool PrepareQueryResult();

		/* GPU 实时树（最近一次解析结果，EMA 平滑；面板与帧统计读这里） */
		const GPUTimeScopeNode& GetRealtimeRoot() const { return m_RealtimeRoot; }

		/* GPUScopeInterface：设备层在分组与通道边界回调，转发给查询上下文。
		 * 后端只认这个接口，不 include profiler。 */
		void OnGpuDebugGroupBegin(const char* name) override { BeginGPUScope(name); }
		void OnGpuDebugGroupEnd() override { EndGPUScope(); }
		void OnGpuRenderPassBegin(void* native_descriptor) override { OnRenderPassBegin(native_descriptor); }
		void OnGpuRenderPassEnd() override { OnRenderPassEnd(); }
		void OnGpuComputePassBegin(void* native_descriptor) override { OnComputePassBegin(native_descriptor); }

		/* 开关：控制RenderQuery是否工作 */
		void SetEnabled(bool enabled);
		bool IsEnabled() const { return m_IsEnabled; }

		/* GPU 耗时录制（面板 Record/Stop/Clear 控制；逐帧回放的数据源） */
		GPUTimeScopeRecorder& GetRecorder() { return m_Recorder; }

		/* 结果树属于哪一帧（查询异步解析，通常比当前帧晚一两帧）；0 = 还没拿到结果 */
		uint32_t GetResultFrameId() const { return m_ResultFrameId; }

	private:
		GPUTimeScopeTree();

		static constexpr uint8_t MAX_CONTEXTS = 4;
		std::vector<GPUQueryContext> m_QueryContexts;
		uint8_t m_ReadContextId{ 0 };
		uint8_t m_WriteContextId{ 1 };
		/* 默认开着：CPU 计时那边也是默认开，面板一进来两边都有数 */
		bool m_IsEnabled{ true };
		uint32_t m_ResultFrameId{ 0 };
		GPUTimeScopeNode m_RealtimeRoot{};
		GPUTimeScopeRecorder m_Recorder{};

	};

}
