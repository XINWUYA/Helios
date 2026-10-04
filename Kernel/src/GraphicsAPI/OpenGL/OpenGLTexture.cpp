#include "Pch.h"
#include "OpenGLTexture.h"
#include "OpenGLCommon.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace Helios
{
	OpenGLTexture::OpenGLTexture(const std::string& name, const TextureDesc& texture_desc)
		: DeviceTexture(name, texture_desc), m_InternalFormat(TranslateToOpenGLTextureFormat(texture_desc.Format))
	{
		PROFILE_FUNCTION();

		if (!!(m_TextureDesc.Usage & TextureUsage::Sampleable))
		{
			/* 创建一个普通纹理，作为颜色rt */

			switch (m_TextureDesc.SamplerType)
			{
			case SamplerType::Sampler2D:
				m_TextureTarget = m_TextureDesc.Samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
				break;
			case SamplerType::Sampler2DArray:
				m_TextureTarget = GL_TEXTURE_2D_ARRAY;
				break;
			case SamplerType::SamplerCubeMap:
				m_TextureTarget = GL_TEXTURE_CUBE_MAP;
				break;
			case SamplerType::Sampler3D:
				m_TextureTarget = GL_TEXTURE_3D;
				break;
			}

			/* 创建纹理 */
			glGenTextures(1, &m_TextureId);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(m_TextureTarget, m_TextureId);
#ifdef HELIOS_DEBUG
			glObjectLabel(GL_TEXTURE, m_TextureId, -1, name.c_str());
#endif

			/* 分配存储 */
			switch (m_TextureTarget)
			{
			case GL_TEXTURE_2D:
			case GL_TEXTURE_CUBE_MAP:
#ifdef __APPLE__
				// macOS only supports OpenGL 4.1, use glTexImage2D for compatibility
				{
					GLenum pixel_format = GetPixelFormatFromTextureFormat(m_TextureDesc.Format);
					GLenum pixel_type = GetPixelTypeFromTextureFormat(m_TextureDesc.Format);
					GLuint width = m_TextureDesc.Width;
					GLuint height = m_TextureDesc.Height;
					for (GLuint level = 0; level < m_TextureDesc.MipLevels; ++level)
					{
						if (m_TextureTarget == GL_TEXTURE_CUBE_MAP)
						{
							for (GLuint face = 0; face < 6; ++face)
							{
								glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, m_InternalFormat, 
									width, height, 0, pixel_format, pixel_type, nullptr);
							}
						}
						else
						{
							glTexImage2D(m_TextureTarget, level, m_InternalFormat, 
								width, height, 0, pixel_format, pixel_type, nullptr);
						}
						width = std::max(1u, width / 2);
						height = std::max(1u, height / 2);
					}
				}
#else
				// Windows/Linux: Use modern glTexStorage2D (OpenGL 4.2+)
				glTexStorage2D(m_TextureTarget, (GLsizei)m_TextureDesc.MipLevels, m_InternalFormat, (GLsizei)m_TextureDesc.Width, (GLsizei)m_TextureDesc.Height);
#endif
				break;
			case GL_TEXTURE_3D:
			case GL_TEXTURE_2D_ARRAY:
#ifdef __APPLE__
				// macOS only supports OpenGL 4.1, use glTexImage3D for compatibility
				{
					GLenum pixel_format = GetPixelFormatFromTextureFormat(m_TextureDesc.Format);
					GLenum pixel_type = GetPixelTypeFromTextureFormat(m_TextureDesc.Format);
					GLuint width = m_TextureDesc.Width;
					GLuint height = m_TextureDesc.Height;
					GLuint depth = m_TextureDesc.Depth;
					for (GLuint level = 0; level < m_TextureDesc.MipLevels; ++level)
					{
						glTexImage3D(m_TextureTarget, level, m_InternalFormat, 
							width, height, depth, 0, pixel_format, pixel_type, nullptr);
						width = std::max(1u, width / 2);
						height = std::max(1u, height / 2);
						if (m_TextureTarget == GL_TEXTURE_3D)
							depth = std::max(1u, depth / 2);
					}
				}
#else
				// Windows/Linux: Use modern glTexStorage3D (OpenGL 4.2+)
				glTexStorage3D(m_TextureTarget, (GLsizei)m_TextureDesc.MipLevels, m_InternalFormat, (GLsizei)m_TextureDesc.Width, (GLsizei)m_TextureDesc.Height, (GLsizei)m_TextureDesc.Depth);
#endif
				break;
			case GL_TEXTURE_2D_MULTISAMPLE:
				glTexImage2DMultisample(m_TextureTarget, m_TextureDesc.Samples, m_InternalFormat, (GLsizei)m_TextureDesc.Width, (GLsizei)m_TextureDesc.Height, GL_TRUE);
				break;
			default:
				break;
			}

			/* 设置采样参数：深度纹理不可被线性过滤须显式设为GL_NEAREST；其余格式保留GL_LINEAR */
			const bool is_depth = (m_TextureDesc.Format == TextureFormat::Depth16 || m_TextureDesc.Format == TextureFormat::Depth24 || m_TextureDesc.Format == TextureFormat::Depth32);
			const GLenum filter = is_depth ? GL_NEAREST : GL_LINEAR;
			glTexParameteri(m_TextureTarget, GL_TEXTURE_MIN_FILTER, filter);
			glTexParameteri(m_TextureTarget, GL_TEXTURE_MAG_FILTER, filter);
			glTexParameteri(m_TextureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(m_TextureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(m_TextureTarget, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

			CHECK_GL_ERROR;
		}
		else
		{
			/* 作为rt，包含Depth/Stencil信息时，需创建一个RenderBuffer */

			ASSERT(m_TextureDesc.Usage & (TextureUsage::ColorAttachment | TextureUsage::DepthAttachment | TextureUsage::StencilAttachment));
			ASSERT(m_TextureDesc.MipLevels == 1);
			ASSERT(m_TextureDesc.SamplerType == SamplerType::Sampler2D);

			m_TextureTarget = GL_RENDERBUFFER;
			glGenRenderbuffers(1, &m_TextureId);
			glBindRenderbuffer(GL_RENDERBUFFER, m_TextureId);

			if (m_TextureDesc.Samples > 1)
				glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_TextureDesc.Samples, m_InternalFormat, (GLsizei)m_TextureDesc.Width, (GLsizei)m_TextureDesc.Height);
			else
				glRenderbufferStorage(GL_RENDERBUFFER, m_InternalFormat, (GLsizei)m_TextureDesc.Width, (GLsizei)m_TextureDesc.Height);

			/* 解绑以免出错，可销毁此RenderBuffer */
			glBindRenderbuffer(GL_RENDERBUFFER, 0);

			CHECK_GL_ERROR;
		}
	}

	OpenGLTexture::OpenGLTexture(const std::string& path, const TextureLoadConfig& load_config)
		: m_Path(path)
	{
		PROFILE_FUNCTION();

		stbi_set_flip_vertically_on_load(load_config.IsFlipV);

		switch (load_config.SamplerType)
		{
		case SamplerType::Sampler2D:
			m_TextureTarget = GL_TEXTURE_2D;
			{
				int width, height, channels;
				stbi_uc* data = stbi_load(path.c_str(), &width, &height, &channels, 0);

				if (data)
				{
					m_IsLoaded = true;
					m_TextureDesc.Width = width;
					m_TextureDesc.Height = height;

					GLenum data_format = 0;
					if (channels == 4)
					{
						m_InternalFormat = load_config.IsHdr ? GL_RGBA16F : GL_RGBA8;
						data_format = GL_RGBA;
					}
					else if (channels == 3)
					{
						m_InternalFormat = load_config.IsHdr ? GL_RGB16F : GL_RGB8;
						data_format = GL_RGB;
					}
					else if (channels == 2)
					{
						m_InternalFormat = load_config.IsHdr ? GL_RG16F : GL_RG8;
						data_format = GL_RG;
					}
					else if (channels == 1)
					{
						m_InternalFormat = load_config.IsHdr ? GL_R16F : GL_R8;
						data_format = GL_RED;
					}

					ASSERT(m_InternalFormat & data_format, "Format not supported!");

					glGenTextures(1, &m_TextureId);
					glBindTexture(m_TextureTarget, m_TextureId);
					glTexImage2D(m_TextureTarget, 0, m_InternalFormat, width, height, 0, data_format, GL_UNSIGNED_BYTE, data);

				if (load_config.IsGenMips)
					{
						glGenerateMipmap(m_TextureTarget);
					}
					
					glTexParameteri(m_TextureTarget, GL_TEXTURE_MIN_FILTER, TranslateToOpenGLSamplerMinFilter(load_config.SamplerMinFilter));
					glTexParameteri(m_TextureTarget, GL_TEXTURE_MAG_FILTER, TranslateToOpenGLSamplerMagFilter(load_config.SamplerMagFilter));
					glTexParameteri(m_TextureTarget, GL_TEXTURE_WRAP_S, TranslateToOpenGLSamplerWrapMode(load_config.SamplerWrapMode));
					glTexParameteri(m_TextureTarget, GL_TEXTURE_WRAP_T, TranslateToOpenGLSamplerWrapMode(load_config.SamplerWrapMode));

					stbi_image_free(data);
				}
				else
				{
					CORE_LOG_ERROR("Failed to load texture: {}.", path);
				}
			}
			break;
		case SamplerType::Sampler2DArray: 
			ASSERT(false, "Not implemented!")
			break;
		case SamplerType::SamplerCubeMap:
			m_TextureTarget = GL_TEXTURE_CUBE_MAP;

			ASSERT(false, "Not implemented!")
			break;
		case SamplerType::Sampler3D: 
			ASSERT(false, "Not implemented!")
			break;
		}

		m_DebugName = ExtractFileBaseName(path);
		m_TextureLoadConfig = load_config;
	}

	OpenGLTexture::~OpenGLTexture()
	{
		PROFILE_FUNCTION();

		if (!!(m_TextureDesc.Usage & TextureUsage::Sampleable)) /* 作为普通Texture */
		{
			glDeleteTextures(1, &m_TextureId);
		}
		else /* 作为RenderBuffer */
		{
			ASSERT(m_TextureTarget == GL_RENDERBUFFER);
			glDeleteRenderbuffers(1, &m_TextureId);
		}
	}

	/* 绑定当前纹理 */
	void OpenGLTexture::Bind(uint32_t slot)
	{
		PROFILE_FUNCTION();

		glActiveTexture(GL_TEXTURE0 + slot);
		glBindTexture(m_TextureTarget, m_TextureId);

		/* 检测到 cubemap 时自动开启无缝采样，消除面边界处的线性过滤缝隙 */
		if (m_TextureTarget == GL_TEXTURE_CUBE_MAP)
			glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	}
	/* 解绑纹理 */
	void OpenGLTexture::Unbind()
	{
		PROFILE_FUNCTION();

		glBindTexture(m_TextureTarget, 0);
	}

	/* 生成 mipmap */
	void OpenGLTexture::GenerateMipmap()
	{
		PROFILE_FUNCTION();

		/* glGenerateMipmap 作用于"当前绑定在该 target 上的纹理"，因此必须先绑定自身：
		 * 帧内被 RenderAPI::GenerateMipmap 调用时，当前绑定的很可能是别的纹理。 */
		glBindTexture(m_TextureTarget, m_TextureId);
		glGenerateMipmap(m_TextureTarget);
	}

	void OpenGLTexture::SetData(void* data, const PixelDesc& pixel_desc, uint32_t level, uint32_t offset_x, uint32_t offset_y, uint32_t offset_z)
	{
		PROFILE_FUNCTION();

		glBindTexture(m_TextureTarget, m_TextureId);

		/* 设置格式 */
		switch (m_TextureTarget)
		{
		case GL_TEXTURE_2D:
			glTexSubImage2D(m_TextureTarget, (GLint)level, (GLint)offset_x, (GLint)offset_y, (GLsizei)m_TextureDesc.Width, (GLsizei)m_TextureDesc.Height,
				TranslateToOpenGLPixelFormat(pixel_desc.Format), TranslateToOpenGLPixelType(pixel_desc.Type), data);
			break;
		case GL_TEXTURE_CUBE_MAP:
			ASSERT(false, "Not implemented!")
			// todo
			//const auto face_data_size = m_TextureDesc.Width * m_TextureDesc.Height * sizeof(float) * 4/*ChannelNum*/;
			//for (auto i = 0; i < 6; ++i)
			//{
			//	glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, (GLint)level, 0, 0, (GLsizei)m_TextureDesc.Width, (GLsizei)m_TextureDesc.Height,
			//		TranslateToOpenGLPixelFormat(pixel_desc.Format), TranslateToOpenGLPixelType(pixel_desc.Type), static_cast<uint8_t const*>(data) + face_data_size * i);
			//}
			break;
		case GL_TEXTURE_3D:
		case GL_TEXTURE_2D_ARRAY:
			glTexSubImage3D(m_TextureTarget, (GLint)level, (GLint)offset_x, (GLint)offset_y, (GLint)offset_z, (GLsizei)m_TextureDesc.Width, (GLsizei)m_TextureDesc.Height, (GLsizei)m_TextureDesc.Depth,
				TranslateToOpenGLPixelFormat(pixel_desc.Format), TranslateToOpenGLPixelType(pixel_desc.Type), data);
			break;
		default:
			break;
		}

		CHECK_GL_ERROR;
	}}
