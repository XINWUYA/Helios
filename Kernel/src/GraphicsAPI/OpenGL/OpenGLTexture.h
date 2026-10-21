#pragma once
#include <string>
#include <glad/glad.h>
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	/**
	 * \brief 纹理类：可以是普通的纹理，也可以作为RenderBuffer
	 */
	class OpenGLTexture : public DeviceTexture
	{
	public:
		OpenGLTexture(const std::string& name, const TextureDesc& texture_desc);
		OpenGLTexture(const std::string& path, const TextureLoadConfig& load_config);
		~OpenGLTexture() override;

		/* 绑定当前纹理 */
		void Bind(uint32_t slot = 0) override;
		/* 解绑纹理 */
		void Unbind() override;
		/* 生成 mipmap */
		void GenerateMipmap() override;
		/* 设置纹理数据 */
		void SetData(void* data, const PixelDesc& pixel_desc, uint32_t level = 0,
			uint32_t offset_x = 0, uint32_t offset_y = 0, uint32_t offset_z = 0) override;

		/* 回读某一 mip / 面（layer）的原始 texel 数据 */
		bool ReadbackPixels(std::vector<uint8_t>& out_data, uint32_t mip_level = 0, uint32_t layer = 0) override;

		/* 按最新资产设置重建纹理；失败保留旧资源 */
		bool Reload() override;
		
		/* 获取纹理路径 */
		const std::string& GetPath() const override { return m_Path; }

		/* 纹理ID */
		uint32_t GetTextureID() const override { return m_TextureId; }
		/* 硬件对象是否是 RenderBuffer：不含 Sampleable 的纯附件资源（离屏深度、模板等）
		 * 在 GL 后端建的是 RBO，挂 FBO 时与纹理用的 API 不同，消费方需据此区分 */
		[[nodiscard]] bool IsRenderBuffer() const { return m_TextureTarget == GL_RENDERBUFFER; }
		/* 纹理是否可用：硬件对象（纹理 / RenderBuffer）已成功创建。
		 * 注意别退回"仅文件加载才为真"的旧语义（见 m_IsLoaded 的说明）。 */
		bool IsLoaded() const override { return m_IsLoaded; }

		/* ����operators */
		bool operator==(const DeviceTexture& other) const override
		{
			return ((OpenGLTexture&)other).GetTextureID() == m_TextureId;
		}

	private:
		/* 硬件层TextureId */
		uint32_t m_TextureId{ 0 };
		/* 硬件层TextureFormat */
		GLenum m_InternalFormat{};
		/* 硬件层Target */
		GLenum m_TextureTarget{};
		/* 纹理路径（从文件加载时有效） */
		std::string m_Path{};
		/* 纹理是否可用 —— 语义是"硬件对象创建成功"，与 MetalTexture 一致：
		 *   文件加载构造器：stbi_load 成功置真；
		 *   desc 构造器（离屏 RT / 字体图集等）：硬件对象建出来后置真。
		 * 不能只认文件加载 —— 离屏 RT（如 FrameGraph 的 LightingResult）走 desc 构造器，
		 * 那样会被误判成"未加载"，ImGuiRenderer 的绑定诊断会刷假阳性日志。 */
		bool m_IsLoaded{ false };

		friend class OpenGLFrameBuffer;
	};
}
