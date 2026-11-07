#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

#include "Helios/Common/Export.h"
#include "Helios/Core/Jobs.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Scene/Mesh.h"

namespace Helios
{
	struct GrassComponent;
	class TerrainField;
	class RenderView;
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
	 * 两条路径的公式一字不差，切换前后草地长得完全一样。
	 *
	 * 块号通过一张「压实块号 → 网格块号」的表还原：按观察范围裁剪之后，
	 * 有效块在网格里不再连续，只靠「序号 ÷ 每块株数」推不回块坐标。 */
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

		/* 一批：把连续的若干个块合成一次提交。
		 * 块用来剔除，批用来画 —— 按实例预算切，块多批少，draw call 就下来了。
		 * 批内块在实例缓冲里是连续的，所以一批就是一段区间 */
		struct Batch
		{
			glm::vec3 Center{ 0.0f };
			glm::vec3 Extent{ 0.0f };
			uint32_t First{ 0 };
			uint32_t Count{ 0 };
			uint32_t ChunkCount{ 0 };   /* 这批合了几块 */
		};

		/* GPU 放置的全局开关：关掉就退回 CPU 逐株生成 */
		static void SetGlobalGpuPlacementEnabled(bool enabled);
		[[nodiscard]] static bool IsGlobalGpuPlacementEnabled();

		/* 全局密度系数：画质档用，一次缩放所有草地的抽稀阈值。
		 * 只在绘制阶段生效，改它不触发重建 */
		static void SetGlobalDensityScale(float scale);
		[[nodiscard]] static float GetGlobalDensityScale();

		/* 间接绘制的全局开关：开 = GPU 剔除 + 压实后一笔画完；关 = 逐块提交。
		 * 需要计算着色器与可共享的存储缓冲，缺一样就自动退回逐块 */
		static void SetGlobalIndirectDrawEnabled(bool enabled);
		[[nodiscard]] static bool IsGlobalIndirectDrawEnabled();

		/* 块掩码剔除的开关：开 = 先压可见块表再逐株扫；关 = 每个实例自己测视锥。
		 * 关掉只是少了这一层，画面应当完全一致，用来对比两层剔除的收益 */
		static void SetGlobalChunkMaskEnabled(bool enabled);
		[[nodiscard]] static bool IsGlobalChunkMaskEnabled();

		/* 批表提交的全局开关：开 = 按批提交，关 = 逐块提交（对照组） */
		static void SetGlobalBatchingEnabled(bool enabled);
		[[nodiscard]] static bool IsGlobalBatchingEnabled();

		/* 一个间接剔除视图：相机与每一档级联各算一份，绘制端按 slot 取结果。
		 * 级联不是独立的 RenderView，所以除了视图指针还要带槽位和它自己的抽稀曲线 */
		struct IndirectView
		{
			const RenderView* View{ nullptr };
			glm::vec4 Planes[6]{};
			glm::vec3 Eye{ 0.0f };
			uint32_t Slot{ 0 };           /* 0 相机视图，1..4 级联 */
			/* 0 按组件的相机距离抽稀，1 按草影 full/fade 的水平距离抽稀 */
			uint32_t Curve{ 0 };
			/* Curve = 1 时的草影最远距离（已经跟阴影距离取过较小值） */
			float FadeRadius{ 0.0f };
			/* 这个视图覆盖的半径（米，绕观察点）：只用来估预算够不够。
			 * 0 表示按抽稀终点现算 */
			float Radius{ 0.0f };
		};

		/* 槽位与分段预算：相机要全量一段；级联按株数截断再切段，
		 * 缓冲不按整片株数照抄四份。多出来的株由抽稀丢掉，影子本来就稀 */
		static constexpr uint32_t kIndirectSlotCount = 5;
		static constexpr uint32_t kMaxIndirectSegments = 4;
		/* 每段株数：一档 4 段就是 98304 株，压实缓冲每档 8MB 左右。
		 * 再大近处的草影就撑不住，再小远处会稀到看不见 */
		static constexpr uint32_t kIndirectSegmentInstances = 24576;
		/* 每段参数占 8 个 uint：5 个给绘制，其余留出 32 字节对齐 */
		static constexpr uint32_t kIndirectArgsStride = 8;
		/* 参数块尾部：段发号计数、块表计数、完成计数，然后是间接派发参数（4 个） */
		static constexpr uint32_t kIndirectHeader = 8;

		/* 开关关着，或槽位不是当帧剔的，都不能拿来画 */
		[[nodiscard]] bool CanDrawIndirect(const RenderView* view, uint32_t slot, uint32_t frame) const;
		[[nodiscard]] uint32_t GetIndirectSegments(const RenderView* view, uint32_t slot) const;
		[[nodiscard]] uint32_t GetIndirectSegmentCapacity(const RenderView* view, uint32_t slot) const;
		[[nodiscard]] const SharedPtr<DeviceStorageBuffer>& GetIndirectArgs(const RenderView* view, uint32_t slot) const;
		/* 压实后的实例数据（顶点视图），画的时候用它 */
		[[nodiscard]] const SharedPtr<DeviceVertexBuffer>& GetIndirectDrawBuffer(
			const RenderView* view, uint32_t slot) const;

		/* 逐视图剔除：可见实例压实进槽位，参数写进 args，并记下帧号。
		 * 必须在开始渲染这个视图之前调用 */
		void PrepareIndirect(const IndirectView& view, uint32_t frame);

		/* 重建实例数据（参数与变换都没变时直接返回）。ground 非空时叶片贴到地形表面，
		 * 块底面不再生成（草下就是地形）。
		 * focus 是观察点（相机 xz）：只对 EndCullDistance 范围内的块生成实例，相机移动超过
		 * 一个网格再重建 —— 全地形均匀铺满在 1km² 上根本放不下预算，必须跟着视野走 */
		void Rebuild(const GrassComponent& component, const glm::mat4& world,
			const TerrainField* ground = nullptr,
			const glm::vec2& focus_min = glm::vec2(0.0f), const glm::vec2& focus_max = glm::vec2(0.0f));

		/* GPU 放置且还没算过时派发一次计算。场景每帧调一次，赶在开始画之前 */
		void EnsurePlaced();

		/* 密度笔刷在草地局部平面落笔，按世界米给半径。返回是否真的改到。 */
		bool PaintDensity(GrassComponent& component, const glm::vec3& world_position,
			float radius, float amount);
		/* 供视口笔刷定位：贴地时打到地形，平地时打到草地所在平面。 */
		bool RaycastDensity(const glm::vec3& origin, const glm::vec3& direction,
			float radius, glm::vec3& out_position) const;

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
		/* 批表：块按实例预算压实后的提交单位，间接路径之外都按它画 */
		[[nodiscard]] const std::vector<Batch>& GetBatches() const { return m_Batches; }
		[[nodiscard]] uint32_t GetInstanceCount() const { return m_InstanceCount; }
		/* 最近一次生成时叶根的世界高度范围：贴地检查用 */
		[[nodiscard]] float GetInstanceMinY() const { return m_InstanceMinY; }
		[[nodiscard]] float GetInstanceMaxY() const { return m_InstanceMaxY; }
		/* 最近一次生成时退化掉的株数，包含坡度和生长权重。 */
		[[nodiscard]] uint32_t GetSlopeCulledCount() const { return m_SlopeCulledCount; }
		[[nodiscard]] bool IsEmpty() const { return m_InstanceCount == 0; }
		[[nodiscard]] bool UsesGpuPlacement() const { return m_UsingGpuPlacement; }
		[[nodiscard]] const TerrainField* GetGround() const { return m_Ground; }
		[[nodiscard]] glm::vec3 GetPaintAxisX() const { return m_Right; }
		[[nodiscard]] glm::vec3 GetPaintAxisZ() const { return m_Forward; }

		/* 脚印贴图与它覆盖的世界范围（xy = 最小角 xz，zw = 1 / 尺寸） */
		[[nodiscard]] const SharedPtr<DeviceTexture>& GetTrampleTexture() const { return m_TrampleTexture; }
		[[nodiscard]] const glm::vec4& GetTrampleRect() const { return m_TrampleRect; }
		[[nodiscard]] bool HasTrample() const { return m_TrampleTexture != nullptr; }

		/* 叶片模板网格（4 段共 8 个三角面）：所有草地共用，第一次绘制时创建 */
		static const MeshPrimitive& GetBladePrimitive();
		/* 块底面模板：XZ 平面上的单位四边形，范围 [-1, 1] */
		static const MeshPrimitive& GetPadPrimitive();

	private:
		/* 生成实例所需的全部输入：同步路径与后台任务共用一份值拷贝，
		 * 后台任务不持有 GrassField 与 TerrainField 的指针 */
		struct BuildParams
		{
			glm::vec3 Origin{ 0.0f };
			glm::vec3 Right{ 1.0f, 0.0f, 0.0f };
			glm::vec3 Forward{ 0.0f, 0.0f, 1.0f };
			float HalfX{ 0.0f };
			float HalfZ{ 0.0f };
			float ChunkWidth{ 0.0f };
			float ChunkDepth{ 0.0f };
			float Height{ 0.0f };
			float Width{ 0.0f };
			float ClumpSize{ 1.5f };
			float ClumpStrength{ 0.0f };
			float ScaleX{ 1.0f };
			float ScaleY{ 1.0f };
			float SurfaceAlign{ 1.0f };
			float MaxSlope{ 42.0f };
			bool UseTerrainGrowth{ false };
			glm::vec3 TerrainGrowth{ 0.45f, 0.06f, 0.62f };
			uint32_t ChunksX{ 0 };
			uint32_t ChunksZ{ 0 };
			uint32_t PerChunk{ 0 };
			uint32_t InstanceCount{ 0 };
			uint32_t Seed{ 0 };
			bool StablePlacement{ false };
			std::vector<uint8_t> DensityMask;
			bool ReceiveShadow{ true };

			/* 每块在实例缓冲里的区间（First, Count）：Count 为 0 的块不生成 */
			std::vector<glm::uvec2> ChunkRanges;

			bool HasGround{ false };
			glm::vec2 GroundExtent{ 0.0f };
			uint32_t GroundResolution{ 0 };
			float GroundHeightScale{ 0.0f };
			/* 地形几何在局部空间：贴地采样要把世界点拉回去，法线再变换到世界 */
			glm::mat4 GroundWorld{ 1.0f };
			glm::mat4 GroundInverse{ 1.0f };
			glm::mat4 GroundNormalMatrix{ 1.0f };
			std::vector<float> GroundHeights;
		};

		/* 后台重建的中间状态：后台算完置 Done，主线程下一帧取走 */
		struct AsyncBuild
		{
			uint64_t Key{ 0 };
			std::atomic<bool> Done{ false };
			std::vector<GrassInstanceData> Instances;
		};

		[[nodiscard]] BuildParams CaptureBuildParams() const;
		/* 块表 → 批表：按实例预算合并连续块，块数远多于批数 */
		void BuildBatches();
		/* should_cancel 非空时每 64 块查一次：后台任务被取消后能立刻退出，不白占 CPU */
		static std::vector<GrassInstanceData> GenerateInstances(const BuildParams& params,
			const std::function<bool()>& should_cancel = {});
		void UploadInstances(std::vector<GrassInstanceData>& instances,
			const SharedPtr<DeviceStorageBuffer>& storage);
		void BuildCpuInstances(const SharedPtr<DeviceStorageBuffer>& storage);
		void UploadTrample();

		SharedPtr<DeviceVertexBuffer> m_InstanceBuffer;
		/* 实例数据的存储缓冲：顶点缓冲是它的一块视图（CreateShared），
		 * GPU 放置与间接剔除都吃这一份，省一次拷贝 */
		SharedPtr<DeviceStorageBuffer> m_InstanceStorage;
		/* 间接绘制：每个视图槽位一份压实后的实例与参数块。
		 * 参数块按段排布，每段自带一套间接参数；块表存这一档可见的块号 */
		struct IndirectSlot
		{
			const RenderView* View{ nullptr };
			/* 哪一帧剔的：不是当帧就作废，别拿旧结果画 */
			uint32_t Frame{ std::numeric_limits<uint32_t>::max() };
			SharedPtr<DeviceStorageBuffer> Storage;
			SharedPtr<DeviceVertexBuffer> ViewBuffer;
			SharedPtr<DeviceStorageBuffer> Args;
			SharedPtr<DeviceStorageBuffer> ChunkList;
			uint32_t SegmentCount{ 0 };
			uint32_t SegmentCapacity{ 0 };
		};
		IndirectSlot m_Indirect[kIndirectSlotCount];
		[[nodiscard]] IndirectSlot* FindIndirectSlot(const RenderView* view, uint32_t slot);
		[[nodiscard]] const IndirectSlot* FindIndirectSlot(const RenderView* view, uint32_t slot) const;
		SharedPtr<DeviceVertexBuffer> m_GpuVertexView;   /* 同一块显存的顶点视图 */
		SharedPtr<DeviceVertexBuffer> m_PadBuffer;
		SharedPtr<DeviceTexture> m_TrampleTexture;
		std::vector<uint8_t> m_Trample;

		std::vector<Chunk> m_Chunks;
		std::vector<Batch> m_Batches;
		/* 压实块号 → 网格块号：裁剪后块号不连续，GPU 放置靠它还原块坐标 */
		std::vector<uint32_t> m_PackedChunkIndices;
		SharedPtr<DeviceStorageBuffer> m_ChunkIndexBuffer;
		/* 压实块号 → 块包围球（xyz 中心、w 半径）：GPU 剔除靠它，
		 * 中心带地形高度，不能按草地平面现推 —— 贴地时两者差几十米 */
		std::vector<glm::vec4> m_PackedChunkBounds;
		SharedPtr<DeviceStorageBuffer> m_ChunkBoundsBuffer;

		/* 贴合的地形：非空时叶片高度取地形采样，块底面不生成 */
		const TerrainField* m_Ground{ nullptr };
		/* 观察点范围（多相机取并集）：只对范围内的块生成实例 */
		glm::vec2 m_FocusMin{ 0.0f };
		glm::vec2 m_FocusMax{ 0.0f };

		/* 重建时算好、GPU 放置与脚印都要用的现场数据 */
		glm::vec3 m_Origin{ 0.0f };
		glm::vec3 m_Right{ 1.0f, 0.0f, 0.0f };
		glm::vec3 m_Forward{ 0.0f, 0.0f, 1.0f };
		glm::vec4 m_TrampleRect{ 0.0f };
		float m_HalfX{ 0.0f };
		float m_HalfZ{ 0.0f };
		float m_ChunkSize{ 8.0f };
		float m_ChunkWidth{ 0.0f };
		float m_ChunkDepth{ 0.0f };
		float m_Height{ 0.0f };
		float m_Width{ 0.0f };
		float m_ClumpSize{ 1.5f };
		float m_ClumpStrength{ 0.0f };
		float m_ScaleX{ 1.0f };
		float m_ScaleY{ 1.0f };
		float m_SurfaceAlign{ 1.0f };
		float m_MaxSlope{ 42.0f };
		bool m_UseTerrainGrowth{ false };
		glm::vec3 m_TerrainGrowth{ 0.45f, 0.06f, 0.62f };
		float m_StartCullDistance{ 25.0f };
		float m_EndCullDistance{ 90.0f };
		float m_MinDensityScale{ 0.1f };
		uint32_t m_ChunksX{ 0 };
		uint32_t m_ChunksZ{ 0 };
		uint32_t m_PerChunk{ 0 };
		uint32_t m_Seed{ 0 };
		std::vector<uint8_t> m_DensityMask;
		SharedPtr<DeviceStorageBuffer> m_DensityMaskStorage;
		bool m_StablePlacement{ false };
		uint32_t m_InstanceCount{ 0 };
		float m_InstanceMinY{ 0.0f };
		float m_InstanceMaxY{ 0.0f };
		uint32_t m_SlopeCulledCount{ 0 };
		/* 后台重建：同 key 只起一个任务，结果在主线程上传 */
		std::shared_ptr<AsyncBuild> m_AsyncBuild;
		uint64_t m_AsyncKey{ 0 };
		Jobs::Task m_AsyncTask;
		uint32_t m_CapacityBytes{ 0 };
		bool m_ReceiveShadow{ true };
		bool m_GpuDirty{ false };
		bool m_UsingGpuPlacement{ false };
		uint64_t m_BuildKey{ 0 };
	};
}
