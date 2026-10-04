#pragma once
#include <glm/glm.hpp>
#include "RenderCommon.h"
#include <Helios/VirtualDevice/DeviceVertexArray.h>

namespace Helios
{
	class DeviceTexture;

	class RenderAPI
	{
	public:
		enum : int
		{
			None = 0,
			OpenGL = 1,
			Metal = 2,
		};

		virtual ~RenderAPI() = default;

		virtual void Init() = 0;

		/* 设置视口（左上角 (x_start, y_start)、宽高为正）。两个后端的裁剪空间都是 y 向上，但
		 * framebuffer 原点相反（Metal 左上 / GL 左下），约定：离屏按 OpenGL 行序落盘、屏幕保持窗口
		 * 原始行序 —— 着色器共用同一套 uv 公式。Metal 后端在 ApplyViewport 里完成转换。 */
		virtual void SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height) = 0;

		/* 设置裁剪矩形。约定与 SetViewport 一致：调用方按 OpenGL 的"原点在左下角"
		 * 给出 (x, y, width, height)，Y 轴翻转由后端内部完成，调用方无需区分平台。 */
		virtual void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
		virtual void SetClearColor(const glm::vec4& color) = 0;
		virtual void Clear() = 0;

		/* 应用光栅化状态 */
		virtual void ApplyRasterState(RenderRasterState raster_state) = 0;
		/* 绘制调用 */
		virtual void DrawIndexed(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array, uint32_t index_count = 0, uint32_t index_offset = 0) = 0;
		virtual void DrawArrays(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array) = 0;
		/* Flush */
		virtual void Flush() = 0;

		/* 给纹理生成 mipmap 并记录进当前帧的命令流。帧内调用（比如探针烘焙之后）必须用它 ——
		 * DeviceTexture::GenerateMipmap 会独立提交命令缓冲区，先于还没提交的烘焙绘制执行、读到空纹理。 */
		virtual void GenerateMipmap(const SharedPtr<DeviceTexture>& /*texture*/) {}

		/* 等最近提交的 GPU 工作完成（阻塞）。用在必须拿到 GPU 写入结果的操作上（比如烘焙回读落盘）：
		 * 命令提交后 GPU 是异步执行的。回读本身就同步的后端不用实现。 */
		virtual void WaitForGPU() {}

		/* GroupMarker */
		virtual void PushDebugGroup(const char* name) = 0;
		virtual void PopDebugGroup() = 0;

	/* 默认RT上开始/结束一帧渲染通道（Metal等显式RenderPass后端需要实现）。
	 * preserve_content = true 表示不加载清除色、保留颜色附件已有内容，
	 * 供叠加层（如 ImGui）在前序 Pass 的绘制结果之上继续渲染。 */
	virtual void BeginDefaultRenderPass(bool /*preserve_content*/ = false) {}
	virtual void EndDefaultRenderPass() {}

	/* 准备本帧的默认渲染目标（交换链）。显式交换链的后端（Metal）需要在此
	 * 取得下一个 drawable 并挂到默认渲染通道上；直接绘制到默认帧缓冲的后端
	 * （OpenGL）无需实现，由默认空实现覆盖。 */
	virtual void PrepareNextFrame() {}

	/* 使用平台 */
	static int GetAPI() { return m_API; }
	/* 创建当前API */
	static SharedPtr<RenderAPI> Create();

	private:
		static int m_API;
	};
}


