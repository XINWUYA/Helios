#pragma once

#ifdef PLATFORM_MACOS

#include <Metal/Metal.hpp>
#include <cstdint>
#include "Helios/Core/Logger.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
    /* ============================ 纹理格式 ============================
     * 先把引擎的 TextureFormat 规范化成 Metal 原生格式（消除 RGB8 / RGBA4 / RGB565 / Depth24 这些
     * 没有直接对应项的），再映射 MTL::PixelFormat —— 创建 / 上传 / 附件绑定三处共用同一份结果。 */

    /* 把引擎纹理格式规范化为 Metal 原生格式 */
    [[nodiscard]] inline TextureFormat NormalizeTextureFormat(TextureFormat format)
    {
        switch (format)
        {
        /* Metal 无 3 通道格式：统一提升为 4 通道（上传数据按 RGBA 布局组织） */
        case TextureFormat::RGB8:        return TextureFormat::RGBA8;
        case TextureFormat::s_RGB8:      return TextureFormat::s_RGBA8;
        case TextureFormat::RGB8_SNorm:  return TextureFormat::RGBA8_SNorm;
        case TextureFormat::RGB32F:      return TextureFormat::RGBA32F;
        /* Metal 无 16 位打包格式：提升为 RGBA8 */
        case TextureFormat::RGB565:
        case TextureFormat::RGBA4:       return TextureFormat::RGBA8;
        /* Metal 无独立的 24 位深度格式 */
        case TextureFormat::Depth24:     return TextureFormat::Depth32;
        default:                         return format;
        }
    }

    /* 引擎纹理格式 → MTL::PixelFormat（输入会先被规范化） */
    [[nodiscard]] inline MTL::PixelFormat ToMetalPixelFormat(TextureFormat format)
    {
        switch (NormalizeTextureFormat(format))
        {
        case TextureFormat::R8:                return MTL::PixelFormatR8Unorm;
        case TextureFormat::R8I:               return MTL::PixelFormatR8Sint;
        case TextureFormat::R8UI:              return MTL::PixelFormatR8Uint;
        case TextureFormat::R8_SNorm:          return MTL::PixelFormatR8Snorm;
        case TextureFormat::Stencil8:          return MTL::PixelFormatStencil8;
        case TextureFormat::R16I:              return MTL::PixelFormatR16Sint;
        case TextureFormat::R16UI:             return MTL::PixelFormatR16Uint;
        case TextureFormat::R16F:              return MTL::PixelFormatR16Float;
        case TextureFormat::RG8:               return MTL::PixelFormatRG8Unorm;
        case TextureFormat::RG8I:              return MTL::PixelFormatRG8Sint;
        case TextureFormat::RG8UI:             return MTL::PixelFormatRG8Uint;
        case TextureFormat::RG8_SNorm:         return MTL::PixelFormatRG8Snorm;
        case TextureFormat::RGB9_E5:           return MTL::PixelFormatRGB9E5Float;
        case TextureFormat::Depth16:           return MTL::PixelFormatDepth16Unorm;
        case TextureFormat::R32I:              return MTL::PixelFormatR32Sint;
        case TextureFormat::R32UI:             return MTL::PixelFormatR32Uint;
        case TextureFormat::R32F:              return MTL::PixelFormatR32Float;
        case TextureFormat::RG16I:             return MTL::PixelFormatRG16Sint;
        case TextureFormat::RG16UI:            return MTL::PixelFormatRG16Uint;
        case TextureFormat::RG16F:             return MTL::PixelFormatRG16Float;
        case TextureFormat::R11G11B10F:        return MTL::PixelFormatRG11B10Float;
        case TextureFormat::RGBA8:             return MTL::PixelFormatRGBA8Unorm;
        case TextureFormat::s_RGBA8:           return MTL::PixelFormatRGBA8Unorm_sRGB;
        case TextureFormat::RGBA8_SNorm:       return MTL::PixelFormatRGBA8Snorm;
        /* 引擎格式表无 4 通道整型项，RGB8I/RGB8UI 直接落到 4 通道整型格式 */
        case TextureFormat::RGB8I:             return MTL::PixelFormatRGBA8Sint;
        case TextureFormat::RGB8UI:            return MTL::PixelFormatRGBA8Uint;
        case TextureFormat::R10G10B10A2:       return MTL::PixelFormatRGB10A2Unorm;
        case TextureFormat::Depth32:           return MTL::PixelFormatDepth32Float;
        case TextureFormat::Depth32F:          return MTL::PixelFormatDepth32Float;
        case TextureFormat::Depth24Stencil8:   return MTL::PixelFormatDepth32Float_Stencil8;
        case TextureFormat::RGBA16F:           return MTL::PixelFormatRGBA16Float;
        case TextureFormat::RGBA32F:           return MTL::PixelFormatRGBA32Float;
        case TextureFormat::DXT1_RGB:
        case TextureFormat::DXT1_RGBA:         return MTL::PixelFormatBC1_RGBA;
        case TextureFormat::s_DXT1_RGB:
        case TextureFormat::s_DXT1_RGBA:       return MTL::PixelFormatBC1_RGBA_sRGB;
        case TextureFormat::DXT3_RGBA:         return MTL::PixelFormatBC2_RGBA;
        case TextureFormat::s_DXT3_RGBA:       return MTL::PixelFormatBC2_RGBA_sRGB;
        case TextureFormat::DXT5_RGBA:         return MTL::PixelFormatBC3_RGBA;
        case TextureFormat::s_DXT5_RGBA:       return MTL::PixelFormatBC3_RGBA_sRGB;
        default:
            CORE_LOG_ERROR("Unsupported texture format for Metal: {}", static_cast<int>(format));
            return MTL::PixelFormatInvalid;
        }
    }

    /* TextureUsage 是位标志，需要按位判断 */
    [[nodiscard]] constexpr bool HasTextureUsage(TextureUsage value, TextureUsage flag) noexcept
    {
        return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
    }

    /* 把基础颜色格式转换为对应的 sRGB 变体；无 sRGB 变体的格式原样返回。
     * sRGB 纹理在采样时由硬件自动线性化，是引擎中“颜色纹理”的正确用法。 */
    [[nodiscard]] inline TextureFormat ToSrgbFormat(TextureFormat format)
    {
        switch (format)
        {
        case TextureFormat::RGBA8:      return TextureFormat::s_RGBA8;
        case TextureFormat::RGB8:       return TextureFormat::s_RGB8;
        case TextureFormat::DXT1_RGB:   return TextureFormat::s_DXT1_RGB;
        case TextureFormat::DXT1_RGBA:  return TextureFormat::s_DXT1_RGBA;
        case TextureFormat::DXT3_RGBA:  return TextureFormat::s_DXT3_RGBA;
        case TextureFormat::DXT5_RGBA:  return TextureFormat::s_DXT5_RGBA;
        default:                        return format;
        }
    }

    /* 是否为深度格式见 RenderCommon.h 的 IsDepthFormat（平台无关，单一来源） */

    /* 是否为模板格式 */
    [[nodiscard]] inline bool IsStencilFormat(TextureFormat format)
    {
        switch (NormalizeTextureFormat(format))
        {
        case TextureFormat::Stencil8:
        case TextureFormat::Depth24Stencil8:
            return true;
        default:
            return false;
        }
    }

    /* 是否为 sRGB 格式（采样时需自动线性化） */
    [[nodiscard]] inline bool IsSrgbFormat(TextureFormat format)
    {
        switch (format)
        {
        case TextureFormat::s_RGB8:
        case TextureFormat::s_RGBA8:
        case TextureFormat::s_DXT1_RGB:
        case TextureFormat::s_DXT1_RGBA:
        case TextureFormat::s_DXT3_RGBA:
        case TextureFormat::s_DXT5_RGBA:
            return true;
        default:
            return false;
        }
    }

    /* 是否为块压缩格式（上传时按 4x4 块组织数据） */
    [[nodiscard]] inline bool IsCompressedFormat(TextureFormat format)
    {
        switch (format)
        {
        case TextureFormat::DXT1_RGB:
        case TextureFormat::s_DXT1_RGB:
        case TextureFormat::DXT1_RGBA:
        case TextureFormat::s_DXT1_RGBA:
        case TextureFormat::DXT3_RGBA:
        case TextureFormat::s_DXT3_RGBA:
        case TextureFormat::DXT5_RGBA:
        case TextureFormat::s_DXT5_RGBA:
            return true;
        default:
            return false;
        }
    }

    /* 纹理格式的通道数（压缩格式返回其解压后的通道数） */
    [[nodiscard]] inline uint32_t GetTextureFormatComponents(TextureFormat format)
    {
        switch (NormalizeTextureFormat(format))
        {
        case TextureFormat::R8: case TextureFormat::R8I: case TextureFormat::R8UI:
        case TextureFormat::R8_SNorm: case TextureFormat::R16I: case TextureFormat::R16UI:
        case TextureFormat::R16F: case TextureFormat::R32I: case TextureFormat::R32UI:
        case TextureFormat::R32F: case TextureFormat::Stencil8:
            return 1;
        case TextureFormat::RG8: case TextureFormat::RG8I: case TextureFormat::RG8UI:
        case TextureFormat::RG8_SNorm: case TextureFormat::RG16I: case TextureFormat::RG16UI:
        case TextureFormat::RG16F: case TextureFormat::Depth16:
            return 2;
        case TextureFormat::DXT1_RGB: case TextureFormat::s_DXT1_RGB:
        case TextureFormat::RGB9_E5:
            return 3;
        default:
            return 4;
        }
    }

    /* ============================ 采样器 ============================ */

    [[nodiscard]] inline MTL::TextureType ToMetalSamplerType(SamplerType type)
    {
        switch (type)
        {
        case SamplerType::Sampler2D:      return MTL::TextureType2D;
        case SamplerType::Sampler2DArray: return MTL::TextureType2DArray;
        case SamplerType::SamplerCubeMap: return MTL::TextureTypeCube;
        case SamplerType::Sampler3D:      return MTL::TextureType3D;
        default:
            CORE_LOG_ERROR("Unknown sampler type: {}", static_cast<int>(type));
            return MTL::TextureType2D;
        }
    }

    [[nodiscard]] inline MTL::SamplerMinMagFilter ToMetalSamplerMinMagFilter(SamplerMinFilter filter)
    {
        return filter == SamplerMinFilter::Nearest ? MTL::SamplerMinMagFilterNearest
                                                   : MTL::SamplerMinMagFilterLinear;
    }

    [[nodiscard]] inline MTL::SamplerMinMagFilter ToMetalSamplerMagFilter(SamplerMagFilter filter)
    {
        return filter == SamplerMagFilter::Nearest ? MTL::SamplerMinMagFilterNearest
                                                   : MTL::SamplerMinMagFilterLinear;
    }

    /* 从 MinFilter 推导 mip 过滤方式 */
    [[nodiscard]] inline MTL::SamplerMipFilter ToMetalSamplerMipFilter(SamplerMinFilter filter)
    {
        switch (filter)
        {
        case SamplerMinFilter::Nearest:
        case SamplerMinFilter::Linear:
            return MTL::SamplerMipFilterNotMipmapped;
        case SamplerMinFilter::NearestMipmapNearest:
            return MTL::SamplerMipFilterNearest;
        case SamplerMinFilter::LinearMipmapNearest:
        case SamplerMinFilter::LinearMipmapLinear:
        case SamplerMinFilter::NearestMipmapLinear:
            return MTL::SamplerMipFilterLinear;
        default:
            return MTL::SamplerMipFilterNotMipmapped;
        }
    }

    [[nodiscard]] inline MTL::SamplerAddressMode ToMetalSamplerAddressMode(SamplerWrapMode mode)
    {
        switch (mode)
        {
        case SamplerWrapMode::ClampToEdge:    return MTL::SamplerAddressModeClampToEdge;
        case SamplerWrapMode::Repeat:         return MTL::SamplerAddressModeRepeat;
        case SamplerWrapMode::MirroredRepeat: return MTL::SamplerAddressModeMirrorRepeat;
        default:
            return MTL::SamplerAddressModeClampToEdge;
        }
    }

    /* ============================ 像素数据 ============================ */

    /* PixelFormat 的通道数 */
    [[nodiscard]] inline uint32_t GetPixelFormatComponents(PixelFormat format)
    {
        switch (format)
        {
        case PixelFormat::R: case PixelFormat::R_Integer:
        case PixelFormat::Alpha: case PixelFormat::Depth:
            return 1;
        case PixelFormat::RG: case PixelFormat::RG_Integer:
            return 2;
        case PixelFormat::RGB: case PixelFormat::RGB_Integer:
            return 3;
        case PixelFormat::Depth24_Stencil8:
            return 4;
        default:
            return 4;
        }
    }

    /* PixelType 的单分量字节数 */
    [[nodiscard]] inline uint32_t GetPixelTypeBytes(PixelType type)
    {
        switch (type)
        {
        case PixelType::UnsignedByte: case PixelType::Byte:  return 1;
        case PixelType::UnsignedShort: case PixelType::Short:
        case PixelType::Half:                                return 2;
        case PixelType::UnsignedInt: case PixelType::Int:
        case PixelType::Float:                               return 4;
        default:                                             return 1;
        }
    }

    /* 每像素字节数，用于 replaceRegion 的 bytesPerRow 计算 */
    [[nodiscard]] inline uint32_t GetPixelDescBytesPerPixel(const PixelDesc& desc)
    {
        return GetPixelFormatComponents(desc.Format) * GetPixelTypeBytes(desc.Type);
    }

    /* ============================ 绘制状态 ============================ */

    [[nodiscard]] inline MTL::PrimitiveType ToMetalPrimitiveType(PrimitiveType type)
    {
        switch (type)
        {
        case PrimitiveType::Points:         return MTL::PrimitiveTypePoint;
        case PrimitiveType::Lines:          return MTL::PrimitiveTypeLine;
        case PrimitiveType::Line_Strip:     return MTL::PrimitiveTypeLineStrip;
        case PrimitiveType::Triangles:      return MTL::PrimitiveTypeTriangle;
        case PrimitiveType::Triangle_Strip: return MTL::PrimitiveTypeTriangleStrip;
        default:
            CORE_LOG_ERROR("Unknown primitive type: {}", static_cast<int>(type));
            return MTL::PrimitiveTypeTriangle;
        }
    }

    [[nodiscard]] inline MTL::CompareFunction ToMetalCompareFunction(CompareFunc func)
    {
        switch (func)
        {
        case CompareFunc::LessEqual:    return MTL::CompareFunctionLessEqual;
        case CompareFunc::GreaterEqual: return MTL::CompareFunctionGreaterEqual;
        case CompareFunc::Less:         return MTL::CompareFunctionLess;
        case CompareFunc::Greater:      return MTL::CompareFunctionGreater;
        case CompareFunc::Equal:        return MTL::CompareFunctionEqual;
        case CompareFunc::NotEqual:     return MTL::CompareFunctionNotEqual;
        case CompareFunc::Always:       return MTL::CompareFunctionAlways;
        case CompareFunc::Never:        return MTL::CompareFunctionNever;
        default:
            CORE_LOG_ERROR("Unknown compare func: {}", static_cast<int>(func));
            return MTL::CompareFunctionAlways;
        }
    }

    /* Metal 不支持同时剔除正反面，此时退化为不剔除（仅损失性能，不影响正确性） */
    [[nodiscard]] inline MTL::CullMode ToMetalCullMode(CullMode mode)
    {
        switch (mode)
        {
        case CullMode::Cull_None:   return MTL::CullModeNone;
        case CullMode::Cull_Front:  return MTL::CullModeFront;
        case CullMode::Cull_Back:   return MTL::CullModeBack;
        case CullMode::Cull_Front_And_Back: return MTL::CullModeNone;
        default:
            CORE_LOG_ERROR("Unknown cull mode: {}", static_cast<int>(mode));
            return MTL::CullModeNone;
        }
    }

    [[nodiscard]] inline MTL::Winding ToMetalWinding(FrontFaceType type)
    {
        return type == FrontFaceType::CW ? MTL::WindingClockwise : MTL::WindingCounterClockwise;
    }

    [[nodiscard]] inline MTL::BlendFactor ToMetalBlendFactor(BlendFunc func)
    {
        switch (func)
        {
        case BlendFunc::Zero:             return MTL::BlendFactorZero;
        case BlendFunc::One:              return MTL::BlendFactorOne;
        case BlendFunc::SrcColor:         return MTL::BlendFactorSourceColor;
        case BlendFunc::OneMinusSrcColor: return MTL::BlendFactorOneMinusSourceColor;
        case BlendFunc::DstColor:         return MTL::BlendFactorDestinationColor;
        case BlendFunc::OneMinusDstColor: return MTL::BlendFactorOneMinusDestinationColor;
        case BlendFunc::SrcAlpha:         return MTL::BlendFactorSourceAlpha;
        case BlendFunc::OneMinusSrcAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
        case BlendFunc::DstAlpha:         return MTL::BlendFactorDestinationAlpha;
        case BlendFunc::OneMinusDstAlpha: return MTL::BlendFactorOneMinusDestinationAlpha;
        case BlendFunc::SrcAlphaSaturate: return MTL::BlendFactorSourceAlphaSaturated;
        default:
            CORE_LOG_ERROR("Unknown blend func: {}", static_cast<int>(func));
            return MTL::BlendFactorOne;
        }
    }

    [[nodiscard]] inline MTL::BlendOperation ToMetalBlendOperation(BlendEquation equation)
    {
        switch (equation)
        {
        case BlendEquation::Add:             return MTL::BlendOperationAdd;
        case BlendEquation::Subtract:        return MTL::BlendOperationSubtract;
        case BlendEquation::ReverseSubtract: return MTL::BlendOperationReverseSubtract;
        case BlendEquation::Min:             return MTL::BlendOperationMin;
        case BlendEquation::Max:             return MTL::BlendOperationMax;
        default:
            CORE_LOG_ERROR("Unknown blend equation: {}", static_cast<int>(equation));
            return MTL::BlendOperationAdd;
        }
    }

    [[nodiscard]] inline MTL::IndexType ToMetalIndexType(IndexType type)
    {
        switch (type)
        {
        case IndexType::UInt16: return MTL::IndexTypeUInt16;
        case IndexType::UInt32: return MTL::IndexTypeUInt32;
        default:
            CORE_LOG_ERROR("Unknown index type: {}", static_cast<int>(type));
            return MTL::IndexTypeUInt32;
        }
    }

    /* ============================ 顶点属性 ============================ */

    [[nodiscard]] inline MTL::VertexFormat ToMetalVertexFormat(BufferDataType type)
    {
        switch (type)
        {
        case BufferDataType::Float:  return MTL::VertexFormatFloat;
        case BufferDataType::Float2: return MTL::VertexFormatFloat2;
        case BufferDataType::Float3: return MTL::VertexFormatFloat3;
        case BufferDataType::Float4: return MTL::VertexFormatFloat4;
        case BufferDataType::Int:    return MTL::VertexFormatInt;
        case BufferDataType::Int2:   return MTL::VertexFormatInt2;
        case BufferDataType::Int3:   return MTL::VertexFormatInt3;
        case BufferDataType::Int4:   return MTL::VertexFormatInt4;
        case BufferDataType::Bool:   return MTL::VertexFormatInt;
        case BufferDataType::UByte4: return MTL::VertexFormatUChar4Normalized;
        /* 矩阵类型在顶点描述符中按列拆分为多个属性，由调用方处理 */
        case BufferDataType::Mat3:   return MTL::VertexFormatFloat3;
        case BufferDataType::Mat4:   return MTL::VertexFormatFloat4;
        default:
            CORE_LOG_ERROR("Unknown vertex data type: {}", static_cast<int>(type));
            return MTL::VertexFormatFloat;
        }
    }

    /* 顶点属性占据的连续属性槽位数（Mat3/Mat4 会占用多列） */
    [[nodiscard]] inline uint32_t GetVertexAttributeSlotCount(BufferDataType type)
    {
        switch (type)
        {
        case BufferDataType::Mat3: return 3;
        case BufferDataType::Mat4: return 4;
        default:                   return 1;
        }
    }

    [[nodiscard]] inline uint32_t GetComponentCount(BufferDataType type)
    {
        switch (type)
        {
        case BufferDataType::Float: case BufferDataType::Int: case BufferDataType::Bool:
            return 1;
        case BufferDataType::Float2: case BufferDataType::Int2:
            return 2;
        case BufferDataType::Float3: case BufferDataType::Int3: case BufferDataType::Mat3:
            return 3;
        case BufferDataType::Float4: case BufferDataType::Int4:
        case BufferDataType::UByte4: case BufferDataType::Mat4:
            return 4;
        default:
            return 1;
        }
    }

    /* 单个顶点属性的字节大小（矩阵按单列大小返回） */
    [[nodiscard]] inline uint32_t GetBufferDataTypeSize(BufferDataType type)
    {
        switch (type)
        {
        case BufferDataType::Float:  return 4;
        case BufferDataType::Float2: return 8;
        case BufferDataType::Float3: return 12;
        case BufferDataType::Float4: return 16;
        case BufferDataType::Int:    return 4;
        case BufferDataType::Int2:   return 8;
        case BufferDataType::Int3:   return 12;
        case BufferDataType::Int4:   return 16;
        case BufferDataType::Bool:   return 4;   /* MSL 中 bool 以 int 传递 */
        case BufferDataType::UByte4: return 4;
        case BufferDataType::Mat3:   return 12;  /* 按单列（float3）返回 */
        case BufferDataType::Mat4:   return 16;  /* 按单列（float4）返回 */
        default:
            CORE_LOG_ERROR("Unknown buffer data type: {}", static_cast<int>(type));
            return 0;
        }
    }
}

#endif /* PLATFORM_MACOS */
