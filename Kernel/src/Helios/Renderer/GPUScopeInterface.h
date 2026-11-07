#pragma once

namespace Helios
{
	/* GPU 作用域接口：设备层在调试分组和通道的边界上回调它，实现方挂时间戳采样，
	 * 面板上的树才和抓帧分组一一对应。接口留在设备层，后端不 include profiler，
	 * 去掉计时就是不注入。native_descriptor 按后端解释，OpenGL 传 nullptr。 */
	class GPUScopeInterface
	{
	public:
		virtual ~GPUScopeInterface() = default;

		/* 调试分组成对事件：抓帧工具的分组同时也是计时作用域的边界 */
		virtual void OnGpuDebugGroupBegin(const char* name) = 0;
		virtual void OnGpuDebugGroupEnd() = 0;

		virtual void OnGpuRenderPassBegin(void* native_descriptor) = 0;
		virtual void OnGpuRenderPassEnd() = 0;
		virtual void OnGpuComputePassBegin(void* native_descriptor) = 0;
	};
}
