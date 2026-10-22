#pragma once
#include "Helios/Renderer/RenderAPI.h"

namespace Helios
{
	class OpenGLRenderAPI : public RenderAPI
	{
	public:
		~OpenGLRenderAPI() override = default;

		void Init() override;
		void SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height) override;
		void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;

		void SetClearColor(const glm::vec4& color) override;
		void Clear() override;

		/* 应用光栅化状态 */
		void ApplyRasterState(RenderRasterState raster_state) override;

		/* 绘制调用 */
		void DrawIndexed(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array, uint32_t index_count = 0, uint32_t index_offset = 0) override;
		void DrawArrays(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array) override;

		/* 计算着色器要 GL 4.3+：桌面上下文起步就是 4.5，非 macOS 一定有；
		 * macOS 只有 4.1，不支持 */
		[[nodiscard]] bool SupportsCompute() const override
		{
#ifdef __APPLE__
			return false;
#else
			return true;
#endif
		}
		bool DispatchCompute(const SharedPtr<DeviceShader>& shader,
			const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>>& buffers,
			uint32_t param_binding, const void* param_data, uint32_t param_size,
			uint32_t group_count) override;

		void Flush() override;

		/* OpenGL 的 mip 生成是立即执行的，直接转交纹理自身实现
		 * （见 RenderAPI::GenerateMipmap 关于帧内时序的说明） */
		void GenerateMipmap(const SharedPtr<DeviceTexture>& texture) override;

		/* 默认目标（窗口画布）快照：glReadPixels 回读默认帧缓冲（GL_BACK）当前
		 * 视口区域，上传到常驻预览纹理（随视口尺寸自动重建、跨帧复用）；
		 * 行序 = glReadPixels 口径（底行在前，top_down = false，与离屏纹理一致） */
		SharedPtr<DeviceTexture> AcquireDefaultTargetSnapshot(bool& top_down) override;

		/* 纹理拷贝（逐 Pass 抓取中间渲染结果用，见 RenderAPI 的说明）：临时 FBO +
		 * glBlitFramebuffer 逐层直拷（4.1 兼容，不用 4.3+ 的 glCopyImageSubData）；
		 * 深度格式按深度附件走。调用点的 FBO 绑定会保存 / 还原。 */
		void CopyTexture(const SharedPtr<DeviceTexture>& src, const SharedPtr<DeviceTexture>& dst,
			uint32_t src_layer) override;

		/* 压入DebugGroup，以便在RenderDoc抓帧时使用Pipeline结构组织DebugMarker */
		void PushDebugGroup(const char* name) override;
		void PopDebugGroup() override;

		/* OpenGL 默认已绑定并直接绘制到默认帧缓冲，无需显式开始/结束通道 */
		void BeginDefaultRenderPass(bool /*preserve_content*/) override {}
		void EndDefaultRenderPass() override {}

	private:
		/* 获取OpenGL支持的扩展 */
		void InitOpenGLExtensions();

		/* 默认目标快照的常驻预览纹理（AcquireDefaultTargetSnapshot 用；随视口
		 * 尺寸变化重建、跨帧复用） */
		SharedPtr<DeviceTexture> m_DefaultTargetSnapshot;

		/* 计算派发的常量块暂存缓冲（std140，用多少分多少，之后只增不减） */
		uint32_t m_ComputeParamBuffer{ 0 };
		uint32_t m_ComputeParamBufferSize{ 0 };

		/* GL 是单线程状态机，记一份已下发的光栅状态就够：重复的设置直接跳过。
		 * 视口与裁剪框的 applied 记录收在 RenderAPI 基类，两个后端共用。 */
		RenderRasterState m_AppliedRasterState;
		bool m_HasAppliedRasterState{ false };

		/* 扩展支持标记 */
		struct
		{
			bool KHR_debug{ false };
			bool EXT_shader_framebuffer_fetch{ false };
			bool EXT_texture_filter_anisotropic{ false };
			bool EXT_texture_sRGB{ false };
		} m_SupportedExtensions;
	};
}
