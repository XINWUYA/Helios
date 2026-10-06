#pragma once
#include <optional>
#include <Helios/Renderer/RenderCommon.h>

namespace Helios
{
	class DeviceTexture;
	struct PixelDesc;

	/* RenderBuffer信息 */
	struct RenderBufferInfo
	{
		SharedPtr<DeviceTexture> RenderTarget{ nullptr };	/* 经CreateTexture创建的RenderBuffer */
		uint16_t Level{ 0 };								/* Mip level */
		uint16_t Layer{ 0 };								/* CubeMap's face or 3DTexture's slice */
	};

	/* 绑定目标附件类型：指明层绑定模式下要定位到哪个附件。 */
	enum class FrameBufferAttachment : uint8_t
	{
		Depth,		/* 深度附件 */
		Stencil,	/* 模板附件 */
		Color		/* 颜色附件（需配合 ColorIndex 指定下标） */
	};

	/* 绑定模式 */
	enum class FrameBufferBindMode : uint8_t
	{
		Whole,	/* 绑定整个FrameBuffer */
		Layered	/* 将某个附件重新绑定到指定层/面，需配合 TargetAttachment / ColorIndex / LayerIndex / MipLevel */
	};

	/* 绑定目标描述：描述本次Bind如何定位渲染目标
	 * 默认为Whole模式；
	 * Layered模式：CSM多级联渲染到同一Texture2DArray的不同切片、烘焙到CubeMap的某个face、或分层渲染到Color数组纹理等场景 */
	struct FrameBufferBindInfo
	{
		/* 绑定模式：Whole-忽略其余定位字段；Layered-使用下方字段设置具体绑定 */
		FrameBufferBindMode Mode{ FrameBufferBindMode::Whole };
		/* Layered模式下要定位的目标附件类型 */
		FrameBufferAttachment TargetAttachment{ FrameBufferAttachment::Depth };
		/* 目标Color附件下标：仅当TargetAttachment为Color时有效（对应ColorRenderBuffers的索引） */
		uint16_t ColorIndex{ 0 };
		/* 目标Layer索引：对Texture2DArray附件为slice索引；对CubeMap附件为face 索引（0~5） */
		uint16_t LayerIndex{ 0 };
		/* 目标Miplevel（默认使用附件描述中的Level） */
		uint16_t MipLevel{ 0 };

		/* 便捷构造：Layered模式下，将指定附件定位到某一层/面
		 * layer_index：Texture2DArray中slice索引；CubeMap中face索引（0~5） */
		static FrameBufferBindInfo ToLayer(FrameBufferAttachment attachment, uint16_t layer_index, uint16_t color_index = 0, uint16_t mip_level = 0)
		{
			return FrameBufferBindInfo{ FrameBufferBindMode::Layered, attachment, color_index, layer_index, mip_level };
		}

		/* 便捷构造：Layered模式下，将Depth附件定位到某一层（CSM 常用） */
		static FrameBufferBindInfo ToDepthLayer(uint16_t layer_index, uint16_t mip_level = 0)
		{
			return ToLayer(FrameBufferAttachment::Depth, layer_index, 0, mip_level);
		}

		/* 便捷构造：Layered模式下，将某个Color附件定位到某一层 */
		static FrameBufferBindInfo ToColorLayer(uint16_t color_index, uint16_t layer_index, uint16_t mip_level = 0)
		{
			return ToLayer(FrameBufferAttachment::Color, layer_index, color_index, mip_level);
		}
	};

	/* FrameBuffer信息描述 */
	struct FrameBufferDesc
	{
		uint32_t Samples{ 1 };								/* 采样次数 */
		ViewportRegion ViewportRegion;						/* 视域范围 */
		RenderBufferUsage Usage;							/* RenderBuffer使用情况 */
		std::vector<RenderBufferInfo> ColorRenderBuffers;	/* Color RenderBuffers */
		std::vector<std::optional<glm::vec4>> ColorClearValues;	/* Optional per-color-attachment clear values */
		RenderBufferInfo DepthRenderBuffer;					/* Depth RenderBuffer */
		RenderBufferInfo StencilRenderBuffer;				/* Stencil RenderBuffer */
	};

	/* 根据描述创建FrameBuffer */
	class DeviceFrameBuffer
	{
	public:
		virtual ~DeviceFrameBuffer() = default;

		/* 绑定：默认绑定整个FrameBuffer；传入bind_info可指定渲染到某一层 */
		virtual void Bind(const FrameBufferBindInfo& bind_info = {}) = 0;
		virtual void Unbind() = 0;

		virtual void Resize(uint32_t width, uint32_t height) = 0;

		/* 读取指定纹理（x, y）像素位置的颜色值 */
		virtual void ReadPixel(uint32_t attachment_index, int x, int y, const PixelDesc& pixel_desc, void* data) = 0;
		/* 指定数据清理Attachment */
		virtual void ClearAttachment(uint32_t attachment_index, int level, const PixelDesc& pixel_desc, void* data) = 0;

		/* 获取FrameBuffer描述信息 */
		[[nodiscard]] const FrameBufferDesc& GetDescription() const { return m_FrameBufferDesc; }

		/* 创建FrameBuffer */
		static SharedPtr<DeviceFrameBuffer> Create(const std::string& name, const FrameBufferDesc& desc);

	protected:
		DeviceFrameBuffer() = default;
		DeviceFrameBuffer(std::string name, FrameBufferDesc desc);

		/* 名称 */
		std::string m_DebugName;
		/* FrameBuffer描述 */
		FrameBufferDesc m_FrameBufferDesc;
	};
}
