#pragma once
#include <glad/glad.h>
#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
	/* 映射到OpenGL对应的图元类型 */
	constexpr GLenum TranslateToOpenGLPrimitiveType(PrimitiveType type)
	{
		switch (type)
		{
		case PrimitiveType::Points:			return GL_POINTS;
		case PrimitiveType::Lines:			return GL_LINES;
		case PrimitiveType::Line_Strip:		return GL_LINE_STRIP;
		case PrimitiveType::Triangles:		return GL_TRIANGLES;
		case PrimitiveType::Triangle_Strip: return GL_TRIANGLE_STRIP;
		}
		return 0;
	}

	/* 映射到OpenGL对应的纹理格式 */
	constexpr GLenum TranslateToOpenGLTextureFormat(TextureFormat format)
	{
		switch (format)
		{
		case TextureFormat::R8:				 return GL_R8;
		case TextureFormat::R8I:			 return GL_R8I;
		case TextureFormat::R8UI:			 return GL_R8UI;
		case TextureFormat::R8_SNorm:		 return GL_R8_SNORM; 
		case TextureFormat::Stencil8:		 return GL_STENCIL_INDEX8;
		case TextureFormat::R16I:			 return GL_R16I;
		case TextureFormat::R16UI:			 return GL_R16UI;
		case TextureFormat::R16F:			 return GL_R16F;
		case TextureFormat::RG8:			 return GL_RG8;
		case TextureFormat::RG8I:			 return GL_RG8I;
		case TextureFormat::RG8UI:			 return GL_RG8UI;
		case TextureFormat::RG8_SNorm:		 return GL_RG8_SNORM;
		case TextureFormat::RGB565:			 return GL_RGB565;
		case TextureFormat::RGB9_E5:		 return GL_RGB9_E5;
		case TextureFormat::RGBA4:			 return GL_RGBA4;
		case TextureFormat::Depth16:		 return GL_DEPTH_COMPONENT16;
		case TextureFormat::RGB8:			 return GL_RGB8;
		case TextureFormat::RGB8I:			 return GL_RGB8I;
		case TextureFormat::RGB8UI:			 return GL_RGB8UI;
		case TextureFormat::s_RGB8:			 return GL_SRGB8;
		case TextureFormat::RGB8_SNorm:		 return GL_RGB8_SNORM;
		case TextureFormat::Depth24:		 return GL_DEPTH_COMPONENT24;
		case TextureFormat::R32I:			 return GL_R32I;
		case TextureFormat::R32UI:			 return GL_R32UI;
		case TextureFormat::R32F:			 return GL_R32F;
		case TextureFormat::RG16I:			 return GL_RG16I;
		case TextureFormat::RG16UI:			 return GL_RG16UI;
		case TextureFormat::RG16F:			 return GL_RG16F;
		case TextureFormat::R11G11B10F:		 return GL_R11F_G11F_B10F;
		case TextureFormat::RGBA8:			 return GL_RGBA8;
		case TextureFormat::s_RGBA8:		 return GL_SRGB8_ALPHA8;
		case TextureFormat::RGBA8_SNorm:	 return GL_RGBA8_SNORM;
		case TextureFormat::R10G10B10A2:	 return GL_RGB10_A2;
		case TextureFormat::Depth32:		 return GL_DEPTH_COMPONENT32;
		case TextureFormat::Depth32F:		 return GL_DEPTH_COMPONENT32F;
		case TextureFormat::Depth24Stencil8: return GL_DEPTH24_STENCIL8;
		case TextureFormat::RGBA16F:		 return GL_RGBA16F;
		case TextureFormat::RGB32F:			 return GL_RGB32F;
		case TextureFormat::RGBA32F:		 return GL_RGBA32F;
#if defined(GL_EXT_texture_compression_s3tc)
		case TextureFormat::DXT1_RGB:		 return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
		case TextureFormat::s_DXT1_RGB:		 return GL_COMPRESSED_SRGB_S3TC_DXT1_EXT;
		case TextureFormat::DXT1_RGBA:		 return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
		case TextureFormat::s_DXT1_RGBA:	 return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT;
		case TextureFormat::DXT3_RGBA:		 return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
		case TextureFormat::s_DXT3_RGBA:	 return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT;
		case TextureFormat::DXT5_RGBA:		 return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
		case TextureFormat::s_DXT5_RGBA:	 return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
#else
		case TextureFormat::DXT1_RGB:
		case TextureFormat::s_DXT1_RGB:
		case TextureFormat::DXT1_RGBA:
		case TextureFormat::s_DXT1_RGBA:
		case TextureFormat::DXT3_RGBA:
		case TextureFormat::s_DXT3_RGBA:
		case TextureFormat::DXT5_RGBA:
		case TextureFormat::s_DXT5_RGBA:
			return 0;
#endif
		}
		return 0;
	}

	/* 映射到OpenGL对应的像素格式 */
	constexpr GLenum TranslateToOpenGLPixelFormat(PixelFormat format)
	{
		switch (format)
		{
		case PixelFormat::R:				return GL_RED;
		case PixelFormat::R_Integer:		return GL_RED_INTEGER;
		case PixelFormat::RG:				return GL_RG;
		case PixelFormat::RG_Integer:		return GL_RG_INTEGER;
		case PixelFormat::RGB:				return GL_RGB;
		case PixelFormat::RGB_Integer:		return GL_RGB_INTEGER;
		case PixelFormat::RGBA:				return GL_RGBA;
		case PixelFormat::RGBA_Integer:		return GL_RGBA_INTEGER;
		case PixelFormat::Depth:			return GL_DEPTH_COMPONENT;
		case PixelFormat::Depth24_Stencil8: return GL_DEPTH_STENCIL;
		case PixelFormat::Alpha:			return GL_ALPHA;
		}
		return 0;
	}

	/* 映射到OpenGL对应的像素数据类型 */
	constexpr GLenum TranslateToOpenGLPixelType(PixelType type)
	{
		switch (type) {
		case PixelType::UnsignedByte:	return GL_UNSIGNED_BYTE;
		case PixelType::Byte:			return GL_BYTE;
		case PixelType::UnsignedShort:	return GL_UNSIGNED_SHORT;
		case PixelType::Short:			return GL_SHORT;
		case PixelType::UnsignedInt:	return GL_UNSIGNED_INT;
		case PixelType::Int:			return GL_INT;
		case PixelType::Half:			return GL_HALF_FLOAT;
		case PixelType::Float:			return GL_FLOAT;
		}
		return 0;
	}

	/* 映射到OpenGL对应的比较方式 */
	constexpr GLenum TranslateToOpenGLCompareFunc(CompareFunc func)
	{
		switch (func)
		{
		case CompareFunc::LessEqual:	return GL_LEQUAL;
		case CompareFunc::GreaterEqual: return GL_GEQUAL;
		case CompareFunc::Less:			return GL_LESS;
		case CompareFunc::Greater:		return GL_GREATER;
		case CompareFunc::Equal:		return GL_EQUAL;
		case CompareFunc::NotEqual:		return GL_NOTEQUAL;
		case CompareFunc::Always:		return GL_ALWAYS;
		case CompareFunc::Never:		return GL_NEVER;
		}
		return 0;
	}


	/* 映射到OpenGL对应的混合方程 */
	constexpr GLenum TranslateToOpenGLBlendEquation(BlendEquation equation)
	{
		switch (equation)
		{
		case BlendEquation::Add:				return GL_FUNC_ADD;
		case BlendEquation::Subtract:			return GL_FUNC_SUBTRACT;
		case BlendEquation::ReverseSubtract:	return GL_FUNC_REVERSE_SUBTRACT;
		case BlendEquation::Min:				return GL_MIN;
		case BlendEquation::Max:				return GL_MAX;
		}
		return 0;
	}

	/* 映射到OpenGL对应的混合方式 */
	constexpr GLenum TranslateToOpenGLBlendFunc(BlendFunc func)
	{
		switch (func)
		{
		case BlendFunc::Zero:				return GL_ZERO;
		case BlendFunc::One:				return GL_ONE;
		case BlendFunc::SrcColor:			return GL_SRC_COLOR;
		case BlendFunc::OneMinusSrcColor:	return GL_ONE_MINUS_SRC_COLOR;
		case BlendFunc::DstColor:			return GL_DST_COLOR;
		case BlendFunc::OneMinusDstColor:	return GL_ONE_MINUS_DST_COLOR;
		case BlendFunc::SrcAlpha:			return GL_SRC_ALPHA;
		case BlendFunc::OneMinusSrcAlpha:	return GL_ONE_MINUS_SRC_ALPHA;
		case BlendFunc::DstAlpha:			return GL_DST_ALPHA;
		case BlendFunc::OneMinusDstAlpha:	return GL_ONE_MINUS_DST_ALPHA;
		case BlendFunc::SrcAlphaSaturate:	return GL_SRC_ALPHA_SATURATE;
		}
		return 0;
	}

	/* 映射到OpenGL对应的纹理寻址方式 */
	constexpr GLint TranslateToOpenGLSamplerWrapMode(SamplerWrapMode mode)
	{
		switch (mode)
		{
		case SamplerWrapMode::ClampToEdge:		return GL_CLAMP_TO_EDGE;
		case SamplerWrapMode::Repeat:			return GL_REPEAT;
		case SamplerWrapMode::MirroredRepeat:	return GL_MIRRORED_REPEAT;
		}
		return 0;
	}

	/* 映射到OpenGL对应的纹理缩小过滤方式 */
	constexpr GLint TranslateToOpenGLSamplerMinFilter(SamplerMinFilter mode)
	{
		switch (mode)
		{
		case SamplerMinFilter::Nearest:					return GL_NEAREST;
		case SamplerMinFilter::Linear:					return GL_LINEAR;
		case SamplerMinFilter::NearestMipmapNearest:	return GL_NEAREST_MIPMAP_NEAREST;
		case SamplerMinFilter::LinearMipmapNearest:		return GL_LINEAR_MIPMAP_NEAREST;
		case SamplerMinFilter::NearestMipmapLinear:		return GL_NEAREST_MIPMAP_LINEAR;
		case SamplerMinFilter::LinearMipmapLinear:		return GL_LINEAR_MIPMAP_LINEAR;
		}
		return 0;
	}

	/* 映射到OpenGL对应的纹理放大过滤方式 */
	constexpr GLint TranslateToOpenGLSamplerMagFilter(SamplerMagFilter mode)
	{
		switch (mode)
		{
		case SamplerMagFilter::Nearest:		return GL_NEAREST;
		case SamplerMagFilter::Linear:		return GL_LINEAR;
		}
		return 0;
	}

	/* 根据纹理格式推导像素格式（用于glTexImage2D） */
	constexpr GLenum GetPixelFormatFromTextureFormat(TextureFormat format)
	{
		switch (format)
		{
		case TextureFormat::R8:
		case TextureFormat::R8_SNorm:
		case TextureFormat::R16F:
		case TextureFormat::R32F:
			return GL_RED;
		case TextureFormat::R8I:
		case TextureFormat::R16I:
		case TextureFormat::R32I:
			return GL_RED_INTEGER;
		case TextureFormat::R8UI:
		case TextureFormat::R16UI:
		case TextureFormat::R32UI:
			return GL_RED_INTEGER;
		case TextureFormat::RG8:
		case TextureFormat::RG8_SNorm:
		case TextureFormat::RG16F:
			return GL_RG;
		case TextureFormat::RG8I:
		case TextureFormat::RG16I:
			return GL_RG_INTEGER;
		case TextureFormat::RG8UI:
		case TextureFormat::RG16UI:
			return GL_RG_INTEGER;
		case TextureFormat::RGB8:
		case TextureFormat::RGB8_SNorm:
		case TextureFormat::s_RGB8:
		case TextureFormat::RGB565:
		case TextureFormat::RGB9_E5:
		case TextureFormat::R11G11B10F:
		case TextureFormat::RGB32F:
			return GL_RGB;
		case TextureFormat::RGB8I:
			return GL_RGB_INTEGER;
		case TextureFormat::RGB8UI:
			return GL_RGB_INTEGER;
		case TextureFormat::RGBA8:
		case TextureFormat::RGBA8_SNorm:
		case TextureFormat::s_RGBA8:
		case TextureFormat::RGBA4:
		case TextureFormat::R10G10B10A2:
		case TextureFormat::RGBA16F:
		case TextureFormat::RGBA32F:
			return GL_RGBA;
		case TextureFormat::Depth16:
		case TextureFormat::Depth24:
		case TextureFormat::Depth32:
		case TextureFormat::Depth32F:
			return GL_DEPTH_COMPONENT;
		case TextureFormat::Depth24Stencil8:
			return GL_DEPTH_STENCIL;
		case TextureFormat::Stencil8:
			return GL_STENCIL_INDEX;
		default:
			return GL_RGBA;
		}
	}

	/* 根据纹理格式推导像素数据类型（用于glTexImage2D） */
	constexpr GLenum GetPixelTypeFromTextureFormat(TextureFormat format)
	{
		switch (format)
		{
		case TextureFormat::R8:
		case TextureFormat::RG8:
		case TextureFormat::RGB8:
		case TextureFormat::RGBA8:
		case TextureFormat::R8UI:
		case TextureFormat::RG8UI:
		case TextureFormat::RGB8UI:
		case TextureFormat::s_RGB8:
		case TextureFormat::s_RGBA8:
			return GL_UNSIGNED_BYTE;
		case TextureFormat::R8_SNorm:
		case TextureFormat::RG8_SNorm:
		case TextureFormat::RGB8_SNorm:
		case TextureFormat::RGBA8_SNorm:
			return GL_BYTE;
		case TextureFormat::R8I:
		case TextureFormat::RG8I:
		case TextureFormat::RGB8I:
			return GL_BYTE;
		case TextureFormat::R16I:
		case TextureFormat::RG16I:
			return GL_SHORT;
		case TextureFormat::R16UI:
		case TextureFormat::RG16UI:
			return GL_UNSIGNED_SHORT;
		case TextureFormat::R16F:
		case TextureFormat::RG16F:
		case TextureFormat::RGBA16F:
			return GL_HALF_FLOAT;
		case TextureFormat::R32F:
		case TextureFormat::RGB32F:
		case TextureFormat::RGBA32F:
			return GL_FLOAT;
		case TextureFormat::R32I:
			return GL_INT;
		case TextureFormat::R32UI:
			return GL_UNSIGNED_INT;
		case TextureFormat::RGB565:
			return GL_UNSIGNED_SHORT_5_6_5;
		case TextureFormat::RGBA4:
			return GL_UNSIGNED_SHORT_4_4_4_4;
		case TextureFormat::R10G10B10A2:
			return GL_UNSIGNED_INT_2_10_10_10_REV;
		case TextureFormat::R11G11B10F:
			return GL_UNSIGNED_INT_10F_11F_11F_REV;
		case TextureFormat::RGB9_E5:
			return GL_UNSIGNED_INT_5_9_9_9_REV;
		case TextureFormat::Depth16:
			return GL_UNSIGNED_SHORT;
		case TextureFormat::Depth24:
		case TextureFormat::Depth32:
			return GL_UNSIGNED_INT;
		case TextureFormat::Depth32F:
			return GL_FLOAT;
		case TextureFormat::Depth24Stencil8:
			return GL_UNSIGNED_INT_24_8;
		case TextureFormat::Stencil8:
			return GL_UNSIGNED_BYTE;
		default:
			return GL_UNSIGNED_BYTE;
		}
	}

	/* OpenGL 错误检查 */#if HELIOS_DEBUG
	void CheckGLError(const char* file, const char* func_name, uint32_t line) noexcept;
	void CheckGLFrameBufferStatus(GLenum target, const char* func_name, uint32_t line) noexcept;
	#define CHECK_GL_ERROR { CheckGLError(__FILE__, __func__, __LINE__); }
	#define CHECK_GL_FRAMEBUFFER_STATUS(target) { CheckGLFrameBufferStatus(target, __func__, __LINE__); }
#else
	#define CHECK_GL_ERROR
	#define CHECK_GL_FRAMEBUFFER_STATUS(target)
#endif

}
