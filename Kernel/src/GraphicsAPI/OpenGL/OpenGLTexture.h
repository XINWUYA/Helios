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
		
		/* 获取纹理路径 */
		const std::string& GetPath() const override { return m_Path; }

		/* 纹理ID */
		uint32_t GetTextureID() const override { return m_TextureId; }
		/* 纹理是否成功加载 */
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
		/* 纹理是否加载完成（从文件加载时有效） */
		bool m_IsLoaded{ false };

		friend class OpenGLFrameBuffer;
	};
}
