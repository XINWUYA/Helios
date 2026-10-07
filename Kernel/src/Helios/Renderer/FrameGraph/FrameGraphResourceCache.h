#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include "Helios/Common/Common.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/VirtualDevice/DeviceFrameBuffer.h"

namespace Helios
{
	/* FrameGraph 的跨帧资源缓存（双缓存）：transient 资源（纹理 / FrameBuffer）帧间复用、不再
	 * 每帧分配；同一逻辑资源保留 kFrameCopyCount 份、按帧号轮转，避免相邻帧争用被 Metal 串行化。
	 * 规则：命中当前槽位且没被占用 → 复用；描述变了 → 换新；同帧重名 → nullptr；错过一整次轮转就淘汰。 */
	class FrameGraphResourceCache
	{
	public:
		/* 副本份数：2 = 双缓存（相邻帧各一份），语义上即"帧号轮转的槽位数" */
		static constexpr uint32_t kFrameCopyCount = 2;

		/* 帧入口：推进帧号、淘汰长期未使用的条目、复位占用标记 */
		void BeginFrame();

		/* 获取（或创建）一张纹理：命中当前槽位的副本返回；同帧重名占用时返回 nullptr */
		[[nodiscard]] SharedPtr<DeviceTexture> AcquireTexture(const std::string& name, const TextureDesc& desc);
		/* 归还纹理（仅当归还对象就是当前槽位条目时标记空闲；未缓存对象忽略） */
		void ReleaseTexture(const std::string& name, const TextureDesc& desc, const SharedPtr<DeviceTexture>& texture);

		/* 获取（或创建）一个 FrameBuffer：同一槽位内名称相同且描述一致才复用。
		 * 注意描述包含附件纹理对象本身 —— 槽位对齐后，FrameBuffer 总是绑定
		 * 同槽位的那套附件纹理。 */
		[[nodiscard]] SharedPtr<DeviceFrameBuffer> AcquireFrameBuffer(const std::string& name, const FrameBufferDesc& desc);

		/* 统计（验证用）：实际发生的 GPU 资源创建 / 复用次数 */
		struct Stats
		{
			uint32_t TextureCreations{ 0 };
			uint32_t FrameBufferCreations{ 0 };
			uint32_t TextureReuses{ 0 };
			uint32_t FrameBufferReuses{ 0 };
		};
		[[nodiscard]] const Stats& GetStats() const { return m_Stats; }

	private:
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

		/* 当前帧使用的副本槽位 */
		[[nodiscard]] uint32_t CurrentSlot() const
		{
			return static_cast<uint32_t>(m_FrameCounter % kFrameCopyCount);
		}

		/* 纹理键：名称 + 全量描述字段 + 槽位（任一变化都换新纹理） */
		[[nodiscard]] static std::string MakeTextureKey(const std::string& name, const TextureDesc& desc, uint32_t slot);
		/* FrameBuffer 键：名称 + 槽位 */
		[[nodiscard]] static std::string MakeFrameBufferKey(const std::string& name, uint32_t slot);
		/* FrameBuffer 描述逐字段比对（含附件纹理对象身份、清除值、保留标志） */
		[[nodiscard]] static bool IsFrameBufferDescCompatible(const FrameBufferDesc& lhs, const FrameBufferDesc& rhs);

		std::unordered_map<std::string, TextureEntry> m_TextureCache;
		std::unordered_map<std::string, FrameBufferEntry> m_FrameBufferCache;
		uint64_t m_FrameCounter{ 0 };
		Stats m_Stats;
	};
}
