#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "Helios/Common/Export.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Scene/Mesh.h"

namespace Helios
{
	struct GrassComponent;
	class DeviceVertexBuffer;
	class DeviceStorageBuffer;
	class DeviceTexture;

	/* 一片草地的实例数据。
	 * 叶片按块（8 米见方的网格单元）生成并连续存放，每块记一段区间：
	 * 绘制时按块做视锥剔除，按距离只画区间的前一段 —— 块内叶片顺序本来就是随机的，
	 * 取前缀等价于均匀抽稀，不用另存一套 LOD 数据。
	 * 参数或变换变了才重建，静止的草地不产生每帧开销。
	 *
	 * 叶片位置有两种生成方式，由全局开关选：
	 *   CPU —— 逐株算好写进实例缓冲（默认，两个后端都能跑）；
	 *   GPU —— 计算着色器按同样的哈希生成，CPU 只算块划分（要计算着色器支持）。
	 * 两条路径的公式一字不差，切换前后草地长得完全一样。 */
	class HELIOS_API GrassField
	{
	public:
		struct Chunk
		{
			glm::vec3 Center{ 0.0f };
			glm::vec3 Extent{ 0.0f };
			uint32_t First{ 0 };   /* 在实例缓冲里的起始下标 */
			uint32_t Count{ 0 };
		};

		/* GPU 放置的全局开关：关掉就退回 CPU 逐株生成 */
		static void SetGlobalGpuPlacementEnabled(bool enabled);
		[[nodiscard]] static bool IsGlobalGpuPlacementEnabled();

		/* 间接绘制的全局开关：开 = GPU 剔除 + 压实后一笔画完；关 = 逐块提交。
		 * 需要计算着色器与可共享的存储缓冲，缺一样就自动退回逐块 */
		static void SetGlobalIndirectDrawEnabled(bool enabled);
		[[nodiscard]] static bool IsGlobalIndirectDrawEnabled();

		/* 本片草地这次能不能走间接路径：资源齐了、并且这个视图已经剔除过。
		 * 开关关掉时 PrepareIndirect 不会把标记置起来，所以这里不用再判开关；
		 * 少了视图标记会拿上一个视图的压实结果去画 */
		[[nodiscard]] bool CanDrawIndirect() const
		{
			return m_IndirectPrepared && m_IndirectArgs != nullptr && m_IndirectDrawBuffer != nullptr;
		}

		/* 逐视图剔除：可见实例压实进绘制缓冲，参数写进 args。
		 * 会借当前帧的命令缓冲，必须在开始渲染这个视图之前调用 */
		void PrepareIndirect(const glm::vec4* planes, const glm::vec3& eye);

		[[nodiscard]] const SharedPtr<DeviceStorageBuffer>& GetIndirectArgs() const { return m_IndirectArgs; }
		/* 压实后的实例数据（顶点视图），画的时候用它 */
		[[nodiscard]] const SharedPtr<DeviceVertexBuffer>& GetIndirectDrawBuffer() const { return m_IndirectDrawView; }

		/* 重建实例数据（参数与变换都没变时直接返回） */
		void Rebuild(const GrassComponent& component, const glm::mat4& world);

		/* GPU 放置且还没算过时派发一次计算。场景每帧调一次，赶在开始画之前 */
		void EnsurePlaced();

		/* 脚印：物体踩过的地方弯下去，站着时保持，走开之后慢慢弹回。
		 * world_position 是物体的世界位置，radius 是影响半径（米） */
		void StampBend(const glm::vec3& world_position, float radius);

		/* 脚印衰减：每帧推一次，recover_seconds 是弹回所需的时间 */
		void TickBend(float delta_time, float recover_seconds);

		[[nodiscard]] const SharedPtr<DeviceVertexBuffer>& GetInstanceBuffer() const { return m_InstanceBuffer; }
		/* 块底面：每块一张水平四边形，铺在叶片下面。
		 * 叶片稀疏的地方（远距离抽稀之后、块边缘）不会露出裸地，颜色一直连到远处。 */
		[[nodiscard]] const SharedPtr<DeviceVertexBuffer>& GetPadBuffer() const { return m_PadBuffer; }

		[[nodiscard]] const std::vector<Chunk>& GetChunks() const { return m_Chunks; }
		[[nodiscard]] uint32_t GetInstanceCount() const { return m_InstanceCount; }
		[[nodiscard]] bool IsEmpty() const { return m_InstanceCount == 0; }
		[[nodiscard]] bool UsesGpuPlacement() const { return m_UsingGpuPlacement; }

		/* 脚印贴图与它覆盖的世界范围（xy = 最小角 xz，zw = 1 / 尺寸） */
		[[nodiscard]] const SharedPtr<DeviceTexture>& GetTrampleTexture() const { return m_TrampleTexture; }
		[[nodiscard]] const glm::vec4& GetTrampleRect() const { return m_TrampleRect; }
		[[nodiscard]] bool HasTrample() const { return m_TrampleTexture != nullptr; }

		/* 叶片模板网格（4 段共 8 个三角面）：所有草地共用，第一次绘制时创建 */
		static const MeshPrimitive& GetBladePrimitive();
		/* 块底面模板：XZ 平面上的单位四边形，范围 [-1, 1] */
		static const MeshPrimitive& GetPadPrimitive();

	private:
		void BuildCpuInstances(const SharedPtr<DeviceStorageBuffer>& storage);
		void UploadTrample();

		SharedPtr<DeviceVertexBuffer> m_InstanceBuffer;
		/* 实例数据的存储缓冲：顶点缓冲是它的一块视图（CreateShared），
		 * GPU 放置与间接剔除都吃这一份，省一次拷贝 */
		SharedPtr<DeviceStorageBuffer> m_InstanceStorage;
		/* 间接绘制：压实后的实例 + 参数块 */
		SharedPtr<DeviceStorageBuffer> m_IndirectDrawBuffer;
		SharedPtr<DeviceVertexBuffer> m_IndirectDrawView;
		SharedPtr<DeviceStorageBuffer> m_IndirectArgs;
		SharedPtr<DeviceVertexBuffer> m_GpuVertexView;   /* 同一块显存的顶点视图 */
		SharedPtr<DeviceVertexBuffer> m_PadBuffer;
		SharedPtr<DeviceTexture> m_TrampleTexture;
		std::vector<uint8_t> m_Trample;

		std::vector<Chunk> m_Chunks;

		/* 重建时算好、GPU 放置与脚印都要用的现场数据 */
		glm::vec3 m_Origin{ 0.0f };
		glm::vec3 m_Right{ 1.0f, 0.0f, 0.0f };
		glm::vec3 m_Forward{ 0.0f, 0.0f, 1.0f };
		glm::vec4 m_TrampleRect{ 0.0f };
		float m_HalfX{ 0.0f };
		float m_HalfZ{ 0.0f };
		float m_ChunkWidth{ 0.0f };
		float m_ChunkDepth{ 0.0f };
		float m_Height{ 0.0f };
		float m_Width{ 0.0f };
		float m_ScaleX{ 1.0f };
		float m_ScaleY{ 1.0f };
		float m_LodDistance{ 25.0f };
		float m_CullDistance{ 90.0f };
		float m_MinDensityScale{ 0.1f };
		uint32_t m_ChunksX{ 0 };
		uint32_t m_ChunksZ{ 0 };
		uint32_t m_PerChunk{ 0 };
		uint32_t m_Seed{ 0 };
		uint32_t m_InstanceCount{ 0 };
		uint32_t m_CapacityBytes{ 0 };
		bool m_ReceiveShadow{ true };
		bool m_GpuDirty{ false };
		bool m_UsingGpuPlacement{ false };
		bool m_IndirectPrepared{ false };
		uint64_t m_BuildKey{ 0 };
	};
}
