#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "Helios/Common/Export.h"
#include "Helios/Scene/LayerMask.h"
#include "Helios/Scene/Mesh.h"

namespace Helios
{
	struct Frustum;

	/* 场景里所有可绘制实例的空间索引（均匀网格）。
	 * 目的：剔除不再每帧扫全场景 —— 视锥 / 阴影范围覆盖到哪些格子就只走哪些格子。
	 * 只存实体句柄、世界包围盒、层与显隐；材质、网格段解析留到真正进绘制列表时再做。
	 * 维护口子在 Scene::OnUpdate：BeginSync → 逐实例 Sync → EndSync（摘掉本帧没同步到的）。
	 * 世界矩阵没变的实例只更新标记，不碰格子，所以静止场景的维护成本接近一次比较。 */
	class HELIOS_API MeshInstanceIndex
	{
	public:
		/* 一帧的开始 / 结束（Sync 之间） */
		void BeginSync();
		/* 同步一个实例：local_bounds 无效（没设包围盒）或 cullable 为 false（蒙皮）时
		 * 只进"恒可见"表，查询原样返回 */
		void Sync(entt::entity entity, const glm::mat4& world, const MeshBounds& local_bounds,
			LayerMask layer, bool visible, bool cullable);
		void EndSync();

		/* 查询：层相交、可见、且与查询范围相交的实体（已去重，顺序不保证） */
		/* 查询只写各实例的去重戳（mutable），逻辑上是只读的 */
		void QueryFrustum(const Frustum& frustum, LayerMask mask, std::vector<entt::entity>& out) const;
		void QueryBounds(const glm::vec3& min, const glm::vec3& max, LayerMask mask,
			std::vector<entt::entity>& out) const;
		/* 不做空间判定，只要层相交且可见（关掉剔除 / 整体收集时用） */
		void QueryAll(LayerMask mask, std::vector<entt::entity>& out) const;

		void Clear();

		/* 格子边长：按场景尺度设一次（缺省 8） */
		void SetCellSize(float cell_size);
		[[nodiscard]] float GetCellSize() const { return m_CellSize; }
		[[nodiscard]] uint32_t GetEntryCount() const { return m_LiveCount; }
		[[nodiscard]] bool IsEmpty() const { return m_LiveCount == 0; }
		/* 所有实例的世界范围（没有实例时为退化盒） */
		[[nodiscard]] const glm::vec3& GetBoundsMin() const { return m_BoundsMin; }
		[[nodiscard]] const glm::vec3& GetBoundsMax() const { return m_BoundsMax; }

	private:
		/* 单个实例最多占多少个格子：再大就进 oversized 表，查询时恒参与判定 */
		static constexpr int64_t kMaxCellsPerEntry = 64;

		enum class Placement : uint8_t
		{
			None,
			Cells,
			Oversized,
			Unbounded,
		};

		struct Entry
		{
			entt::entity Entity{ entt::null };
			glm::mat4 World{ 0.0f };   /* 0 表示还没量过 */
			MeshBounds LocalBounds{};
			glm::vec3 Center{ 0.0f };
			glm::vec3 Extent{ 0.0f };
			LayerMask Layer{ Layers::Default };
			std::vector<int64_t> Cells{};
			uint32_t ListSlot{ 0 };    /* Oversized / Unbounded 表里的下标 */
			uint32_t SyncStamp{ 0 };
			mutable uint32_t QueryStamp{ 0 };   /* 去重戳：查询顺手写，不算逻辑状态 */
			Placement Where{ Placement::None };
			bool Visible{ true };
			bool Cullable{ true };
			bool HasBounds{ false };
			bool Alive{ false };
		};

		struct Query
		{
			const glm::vec4* Planes{ nullptr };
			uint32_t PlaneCount{ 0 };
			glm::vec3 Min{ 0.0f };
			glm::vec3 Max{ 0.0f };
			bool UseBounds{ false };
			LayerMask Mask{ Layers::All };
		};

		uint32_t AllocateSlot();
		void UpdateBounds(Entry& entry);
		void Place(Entry& entry, uint32_t slot);
		void Unplace(Entry& entry, uint32_t slot);
		void RunQuery(const Query& query, const glm::ivec3& cell_min, const glm::ivec3& cell_max,
			std::vector<entt::entity>& out) const;
		void Sweep();

		float m_CellSize{ 8.0f };
		std::vector<Entry> m_Entries{};
		/* 实体下标 → 槽位 + 1（0 = 没有） */
		std::vector<uint32_t> m_EntitySlots{};
		std::vector<uint32_t> m_FreeSlots{};
		std::unordered_map<int64_t, std::vector<uint32_t>> m_Cells{};
		/* 太大跨不了格子的、和没有包围盒 / 不可剔的：查询时都直接参与判定 */
		std::vector<uint32_t> m_Oversized{};
		std::vector<uint32_t> m_Unbounded{};

		glm::vec3 m_BoundsMin{ 0.0f };
		glm::vec3 m_BoundsMax{ 0.0f };
		uint32_t m_SyncStamp{ 0 };
		mutable uint32_t m_QueryStamp{ 0 };
		uint32_t m_LiveCount{ 0 };
		uint32_t m_SyncedCount{ 0 };
	};
}
