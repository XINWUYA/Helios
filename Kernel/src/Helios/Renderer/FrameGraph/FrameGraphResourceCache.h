#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "Helios/Common/Common.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/VirtualDevice/DeviceFrameBuffer.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"

namespace Helios
{
	/* FrameGraph 的 transient 资源池（双缓存）：纹理按描述分池，资源用完就还回池里，
	 * 同帧后面的 Pass 能直接接着用；每份物理资源按帧号轮转两份，相邻帧不争用同一对象。
	 * 规则：池里有空闲的就复用、没有就新建；FrameBuffer 按名字复用；错过一整次轮转就淘汰。 */
	class FrameGraphResourceCache
	{
	public:
		/* 副本份数：2 = 双缓存（相邻帧各一份），语义上即"帧号轮转的槽位数" */
		static constexpr uint32_t kFrameCopyCount = 2;

		/* 帧入口：推进帧号、淘汰长期未使用的条目、复位占用标记 */
		void BeginFrame();

		/* 从当前槽位的池里取一张描述相同的空闲纹理，没有就新建（name 只用作新建时的标签） */
		[[nodiscard]] SharedPtr<DeviceTexture> AcquireTexture(const std::string& name, const TextureDesc& desc);
		/* 归还纹理：标记空闲，同帧后续 Pass 可以复用（未缓存的对象忽略） */
		void ReleaseTexture(const TextureDesc& desc, const SharedPtr<DeviceTexture>& texture);

		/* 获取（或创建）一个 FrameBuffer：同一槽位内名称相同且描述一致才复用。
		 * 注意描述包含附件纹理对象本身 —— 槽位对齐后，FrameBuffer 总是绑定
		 * 同槽位的那套附件纹理。 */
		[[nodiscard]] SharedPtr<DeviceFrameBuffer> AcquireFrameBuffer(const std::string& name, const FrameBufferDesc& desc);

		/* 从当前槽位的池里取一块同尺寸的空闲缓冲，没有就新建。
		 * 取到的缓冲带着上一份内容，调用方必须整块重写。 */
		[[nodiscard]] SharedPtr<DeviceStorageBuffer> AcquireStorageBuffer(const std::string& name, uint32_t size);
		/* 归还缓冲：标记空闲，同帧后续 Pass 可以复用 */
		void ReleaseStorageBuffer(uint32_t size, const SharedPtr<DeviceStorageBuffer>& buffer);

		/* 统计（验证用）：实际发生的 GPU 资源创建 / 复用次数 */
		struct Stats
		{
			uint32_t TextureCreations{ 0 };
			uint32_t FrameBufferCreations{ 0 };
			uint32_t StorageCreations{ 0 };
			uint32_t TextureReuses{ 0 };
			uint32_t FrameBufferReuses{ 0 };
			uint32_t StorageReuses{ 0 };
		};
		[[nodiscard]] const Stats& GetStats() const { return m_Stats; }

	private:
		/* 纹理池的键：描述的全部字段 */
		struct TextureKey
		{
			uint32_t Width, Height, Depth;
			uint8_t MipLevels, Samples;
			TextureFormat Format;
			SamplerType Sampler;
			TextureUsage Usage;

			explicit TextureKey(const TextureDesc& desc);
			bool operator==(const TextureKey& other) const;
		};
		struct TextureKeyHash
		{
			size_t operator()(const TextureKey& key) const;
		};

		struct TextureEntry
		{
			SharedPtr<DeviceTexture> Texture;
			uint64_t LastUsedFrame{ 0 };
			bool InUse{ false };
		};

		struct FrameBufferEntry
		{
			FrameBufferDesc Desc;
			SharedPtr<DeviceFrameBuffer> FrameBuffer;
			uint64_t LastUsedFrame{ 0 };
			bool InUse{ false };
		};

		struct StorageEntry
		{
			SharedPtr<DeviceStorageBuffer> Buffer;
			uint64_t LastUsedFrame{ 0 };
			bool InUse{ false };
		};

		/* 当前帧使用的副本槽位 */
		[[nodiscard]] uint32_t CurrentSlot() const
		{
			return static_cast<uint32_t>(m_FrameCounter % kFrameCopyCount);
		}

		/* FrameBuffer 描述逐字段比对（含附件纹理对象身份、清除值、保留标志） */
		[[nodiscard]] static bool IsFrameBufferDescCompatible(const FrameBufferDesc& lhs, const FrameBufferDesc& rhs);

		/* 每个槽位一套：纹理按描述分池，FrameBuffer 按名字存 */
		std::array<std::unordered_map<TextureKey, std::vector<TextureEntry>, TextureKeyHash>, kFrameCopyCount> m_TexturePools;
		std::array<std::unordered_map<std::string, FrameBufferEntry>, kFrameCopyCount> m_FrameBuffers;
		/* 计算缓冲按字节数分池 */
		std::array<std::unordered_map<uint32_t, std::vector<StorageEntry>>, kFrameCopyCount> m_StoragePools;
		uint64_t m_FrameCounter{ 0 };
		Stats m_Stats;
	};
}
