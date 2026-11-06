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
	class DeviceStorageBuffer;
	class FrameGraphResourceCache;

	/* FrameGraph中一个RenderPassNode中使用的资源 */
	class HELIOS_API FrameGraphResources
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
		/* 外部导入的纹理：由导入方持有与创建，Create 不取池、Destroy 不还池。
		 * 跨帧的时域历史（TAA 的上一帧颜色）走这条 —— 池子按描述分池、同帧先到先得，
		 * 历史纹理放进去会被别的 Pass 抢走。 */
		bool Imported{ false };

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

		/* 创建（cache 非空时先从资源池取同描述的空闲纹理） */
		void Create(const std::string& name, const Descriptor& desc, Usage usage, FrameGraphResourceCache* cache);
		/* 销毁（还回资源池，同帧后续 Pass 可接着用） */
		void Destroy(const std::string& name, const Descriptor& desc, Usage usage, FrameGraphResourceCache* cache);
	};

	/* FrameGraph中使用的StorageBuffer：计算着色器的输入与输出。
	 * 缓冲没有子资源，一整块就是一个资源，读写靠 Usage 区分。 */
	struct FrameGraphBuffer
	{
		SharedPtr<DeviceStorageBuffer> Buffer;
		/* 外部导入的缓冲：由导入方持有与创建，Create 不向池子要、Destroy 不还池。
		 * 跨帧保留的数据都走这条，池子里的资源下一帧可能被别的 Pass 拿走。 */
		bool Imported{ false };

		/* 描述 */
		struct Descriptor
		{
			uint32_t Size{ 0 }; /* 字节数 */
		};

		/* 缓冲没有切片概念，占位以满足 Resource 模板的类型要求 */
		struct SubDescriptor
		{
		};

		/* 使用方式 */
		using Usage = BufferAccess;
		static constexpr Usage DefaultReadUsage = BufferAccess::Read;
		static constexpr Usage DefaultWriteUsage = BufferAccess::Write;

		/* 创建（cache 非空时先从资源池取同尺寸的空闲缓冲） */
		void Create(const std::string& name, const Descriptor& desc, Usage usage, FrameGraphResourceCache* cache);
		/* 销毁（还回资源池） */
		void Destroy(const std::string& name, const Descriptor& desc, Usage usage, FrameGraphResourceCache* cache);
	};
}
