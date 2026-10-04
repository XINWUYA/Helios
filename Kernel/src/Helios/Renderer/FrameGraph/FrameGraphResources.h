#pragma once
#include "FrameGraphResourceHandle.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Renderer/FrameGraph/Resource.h"

namespace Helios
{
	class FrameGraph;
	class RenderPassNode;
	class IResource;
	class DeviceFrameBuffer;

	/* FrameGraph中一个RenderPassNode中使用的资源 */
	class FrameGraphResources
	{
	public:
		FrameGraphResources(FrameGraph& frame_graph, RenderPassNode& render_pass_node);
		~FrameGraphResources() = default;

		/* 获取当前RenderPassNode名 */
		[[nodiscard]] const std::string& GetPassName() const;

		/* 获取指定资源 */
		template<typename ResourceType>
		const ResourceType& Get(FrameGraphResourceHandleTyped<ResourceType> handle) const
		{
			return StaticPtrCast<const Resource<ResourceType>>(GetResource(handle))->GetResource();
		}

		/* 获取指定资源的描述 */
		template <typename ResourceType>
		const typename ResourceType::Descriptor& GetDescriptor(FrameGraphResourceHandleTyped<ResourceType> handle) const
		{
			return StaticPtrCast<const Resource<ResourceType>>(GetResource(handle))->GetDescriptor();
		}

		/* 获取指定资源的用途 */
		template <typename ResourceType>
		const typename ResourceType::Usage& GetUsage(FrameGraphResourceHandleTyped<ResourceType> handle) const
		{
			return StaticPtrCast<const Resource<ResourceType>>(GetResource(handle))->GetUsage();
		}

		/* 获取当前RenderPass的RenderTarget */
		[[nodiscard]] SharedPtr<DeviceFrameBuffer> GetPassRenderTarget(uint32_t idx = 0) const;

	private:
		/* 从FrameGraph中获取资源 */
		[[nodiscard]] SharedPtr<IResource> GetResource(FrameGraphResourceHandle handle) const;

		/* 所属FrameGraph */
		FrameGraph& m_FrameGraph;
		/* 所属的FrameGraphPass */
		RenderPassNode& m_RenderPassNode;
	};

	class DeviceTexture;

	/* FrameGraph中使用的Texture */
	struct FrameGraphTexture
	{
		SharedPtr<DeviceTexture> Texture;

		/* 描述 */
		struct Descriptor
		{
			uint32_t Width{ 1 };
			uint32_t Height{ 1 }; 
			uint32_t Depth{ 1 }; /* 可表示3D纹理 */
			uint8_t  MipLevels{ 1 }; /* 生成的Mip层级数 */
			uint8_t  Samples{ 0 }; /* 0：自动采样；1：不开启多重采样；>1：不可采样 */

			TextureFormat TextureFormat{ TextureFormat::RGBA8 };
			SamplerType   SamplerType{ SamplerType::Sampler2D };
		};

		/* 子资源描述 */
		struct SubDescriptor
		{
			/* Mip level */
			uint16_t Level{ 0 };
			/* Face or slice */
			uint16_t Layer{ 0 };
		};

		/* 使用方式 */
		using Usage = TextureUsage;
		static constexpr Usage DefaultReadUsage = TextureUsage::Sampleable;
		static constexpr Usage DefaultWriteUsage = TextureUsage::ColorAttachment;

		/* 创建 */
		void Create(const std::string& name, const Descriptor& desc, Usage usage);
		/* 销毁 */
		void Destroy();
	};
}
