#include "Pch.h"
#include "MeshInstanceIndex.h"

#include <algorithm>
#include <limits>
#include "Helios/Renderer/RenderView.h"

namespace Helios
{
	namespace
	{
		/* 格子坐标压进 21 位一格，够 ±100 万个格子（8 米格子 = ±800 万米） */
		constexpr int32_t kCellCoordBits = 21;
		constexpr int32_t kCellCoordLimit = (1 << (kCellCoordBits - 1)) - 1;
		constexpr uint64_t kCellCoordMask = (1ULL << kCellCoordBits) - 1;

		glm::ivec3 UnpackCell(int64_t key)
		{
			const uint64_t bits = static_cast<uint64_t>(key);
			const auto sign_extend = [](uint64_t value) -> int32_t
			{
				const uint32_t raw = static_cast<uint32_t>(value & kCellCoordMask);
				const uint32_t sign_bit = 1u << (kCellCoordBits - 1);
				return static_cast<int32_t>((raw ^ sign_bit) - sign_bit);
			};
			return { sign_extend(bits >> (kCellCoordBits * 2)),
				sign_extend(bits >> kCellCoordBits), sign_extend(bits) };
		}

		int64_t PackCell(int32_t x, int32_t y, int32_t z)
		{
			const uint64_t bits_x = static_cast<uint64_t>(x) & kCellCoordMask;
			const uint64_t bits_y = static_cast<uint64_t>(y) & kCellCoordMask;
			const uint64_t bits_z = static_cast<uint64_t>(z) & kCellCoordMask;
			return static_cast<int64_t>((bits_x << (kCellCoordBits * 2)) | (bits_y << kCellCoordBits) | bits_z);
		}

		/* 从太远的坐标夹回来：真到了这个量级，剔除精度已经不重要了 */
		int32_t QuantizeCell(float value, float cell_size)
		{
			const float scaled = std::floor(value / cell_size);
			if (!(scaled > -static_cast<float>(kCellCoordLimit)))
				return -kCellCoordLimit;
			if (!(scaled < static_cast<float>(kCellCoordLimit)))
				return kCellCoordLimit;
			return static_cast<int32_t>(scaled);
		}
	}

	void MeshInstanceIndex::SetCellSize(float cell_size)
	{
		if (cell_size <= 0.0f || cell_size == m_CellSize)
			return;

		m_CellSize = cell_size;
		/* 格子换了，全部实例下次 Sync 时重新落位（这里只把旧格子清空） */
		for (Entry& entry : m_Entries)
		{
			entry.Cells.clear();
			entry.Where = Placement::None;
			entry.World = glm::mat4(0.0f);
		}
		m_Cells.clear();
		m_Oversized.clear();
		m_Unbounded.clear();
	}

	void MeshInstanceIndex::BeginSync()
	{
		++m_SyncStamp;
		m_SyncedCount = 0;
		/* 0 是"从没同步过"，回绕时把戳清掉重来 */
		if (m_SyncStamp == 0)
		{
			for (Entry& entry : m_Entries)
				entry.SyncStamp = 0;
			m_SyncStamp = 1;
		}
	}

	uint32_t MeshInstanceIndex::AllocateSlot()
	{
		if (!m_FreeSlots.empty())
		{
			const uint32_t slot = m_FreeSlots.back();
			m_FreeSlots.pop_back();
			return slot;
		}

		m_Entries.emplace_back();
		return static_cast<uint32_t>(m_Entries.size() - 1);
	}

	void MeshInstanceIndex::UpdateBounds(Entry& entry)
	{
		if (!entry.LocalBounds.IsValid())
		{
			entry.HasBounds = false;
			return;
		}

		const glm::mat4& matrix = entry.World;
		const glm::vec3 half = (entry.LocalBounds.Max - entry.LocalBounds.Min) * 0.5f;
		entry.Center = glm::vec3(matrix * glm::vec4((entry.LocalBounds.Min + entry.LocalBounds.Max) * 0.5f, 1.0f));
		entry.Extent = glm::abs(glm::vec3(matrix[0])) * half.x
			+ glm::abs(glm::vec3(matrix[1])) * half.y
			+ glm::abs(glm::vec3(matrix[2])) * half.z;
		entry.HasBounds = true;

		m_BoundsMin = glm::min(m_BoundsMin, entry.Center - entry.Extent);
		m_BoundsMax = glm::max(m_BoundsMax, entry.Center + entry.Extent);
	}

	void MeshInstanceIndex::Place(Entry& entry, uint32_t slot)
	{
		if (!entry.HasBounds || !entry.Cullable)
		{
			entry.ListSlot = static_cast<uint32_t>(m_Unbounded.size());
			m_Unbounded.push_back(slot);
			entry.Where = Placement::Unbounded;
			return;
		}

		const glm::ivec3 cell_min(
			QuantizeCell(entry.Center.x - entry.Extent.x, m_CellSize),
			QuantizeCell(entry.Center.y - entry.Extent.y, m_CellSize),
			QuantizeCell(entry.Center.z - entry.Extent.z, m_CellSize));
		const glm::ivec3 cell_max(
			QuantizeCell(entry.Center.x + entry.Extent.x, m_CellSize),
			QuantizeCell(entry.Center.y + entry.Extent.y, m_CellSize),
			QuantizeCell(entry.Center.z + entry.Extent.z, m_CellSize));

		const int64_t span_x = static_cast<int64_t>(cell_max.x) - cell_min.x + 1;
		const int64_t span_y = static_cast<int64_t>(cell_max.y) - cell_min.y + 1;
		const int64_t span_z = static_cast<int64_t>(cell_max.z) - cell_min.z + 1;
		if (span_x * span_y * span_z > kMaxCellsPerEntry)
		{
			/* 铺满全场的物体（天空盒、地形）跨的格子太多，塞进格子反而拖慢查询 */
			entry.ListSlot = static_cast<uint32_t>(m_Oversized.size());
			m_Oversized.push_back(slot);
			entry.Where = Placement::Oversized;
			return;
		}

		entry.Cells.clear();
		entry.Cells.reserve(static_cast<size_t>(span_x * span_y * span_z));
		for (int32_t x = cell_min.x; x <= cell_max.x; ++x)
		{
			for (int32_t y = cell_min.y; y <= cell_max.y; ++y)
			{
				for (int32_t z = cell_min.z; z <= cell_max.z; ++z)
				{
					const int64_t key = PackCell(x, y, z);
					m_Cells[key].push_back(slot);
					entry.Cells.push_back(key);
				}
			}
		}
		entry.Where = Placement::Cells;
	}

	void MeshInstanceIndex::Unplace(Entry& entry, uint32_t slot)
	{
		switch (entry.Where)
		{
		case Placement::Cells:
			for (const int64_t key : entry.Cells)
			{
				const auto iter = m_Cells.find(key);
				if (iter == m_Cells.end())
					continue;

				std::vector<uint32_t>& slots = iter->second;
				const auto found = std::find(slots.begin(), slots.end(), slot);
				if (found != slots.end())
				{
					*found = slots.back();
					slots.pop_back();
				}
				if (slots.empty())
					m_Cells.erase(iter);
			}
			entry.Cells.clear();
			break;

		case Placement::Oversized:
		case Placement::Unbounded:
		{
			std::vector<uint32_t>& list = (entry.Where == Placement::Oversized) ? m_Oversized : m_Unbounded;
			if (entry.ListSlot < list.size())
			{
				const uint32_t moved = list.back();
				list[entry.ListSlot] = moved;
				list.pop_back();
				if (moved != slot)
					m_Entries[moved].ListSlot = entry.ListSlot;
			}
			break;
		}

		case Placement::None:
			break;
		}
		entry.Where = Placement::None;
	}

	void MeshInstanceIndex::Sync(entt::entity entity, const glm::mat4& world, const MeshBounds& local_bounds,
		LayerMask layer, bool visible, bool cullable)
	{
		const uint32_t entity_index = static_cast<uint32_t>(entt::to_entity(entity));
		if (entity_index >= m_EntitySlots.size())
			m_EntitySlots.resize(entity_index + 1, 0);

		uint32_t slot = m_EntitySlots[entity_index];
		Entry* entry = nullptr;
		if (slot != 0)
		{
			/* 表里存的是槽位 + 1（0 表示没有），换成真槽位再用 */
			--slot;
			Entry& candidate = m_Entries[slot];
			/* 句柄比对：实体销毁后下标可能被新实体复用 */
			if (candidate.Alive && candidate.Entity == entity)
				entry = &candidate;
		}

		if (entry == nullptr)
		{
			slot = AllocateSlot();
			m_EntitySlots[entity_index] = slot + 1;
			entry = &m_Entries[slot];
			entry->Entity = entity;
			entry->World = glm::mat4(0.0f);
			entry->Alive = true;
			entry->Where = Placement::None;
			++m_LiveCount;
		}

		if (entry->SyncStamp != m_SyncStamp)
		{
			entry->SyncStamp = m_SyncStamp;
			++m_SyncedCount;
		}
		entry->Layer = layer;
		entry->Visible = visible;
		entry->Cullable = cullable;

		/* 没动过的实例只更新标记：包围盒与格子都不用碰 */
		if (entry->World == world && entry->LocalBounds.Min == local_bounds.Min && entry->LocalBounds.Max == local_bounds.Max)
			return;

		Unplace(*entry, slot);
		entry->World = world;
		entry->LocalBounds = local_bounds;
		UpdateBounds(*entry);
		Place(*entry, slot);
	}

	void MeshInstanceIndex::EndSync()
	{
		if (m_SyncedCount < m_LiveCount)
			Sweep();
	}

	/* 本帧没同步到的实例：实体被删 / 模型被摘掉，整批摘出去 */
	void MeshInstanceIndex::Sweep()
	{
		for (uint32_t slot = 0; slot < static_cast<uint32_t>(m_Entries.size()); ++slot)
		{
			Entry& entry = m_Entries[slot];
			if (!entry.Alive || entry.SyncStamp == m_SyncStamp)
				continue;

			Unplace(entry, slot);
			const uint32_t entity_index = static_cast<uint32_t>(entt::to_entity(entry.Entity));
			if (entity_index < m_EntitySlots.size() && m_EntitySlots[entity_index] == slot + 1)
				m_EntitySlots[entity_index] = 0;

			entry.Alive = false;
			entry.Entity = entt::null;
			entry.World = glm::mat4(0.0f);
			entry.LocalBounds = {};
			m_FreeSlots.push_back(slot);
			--m_LiveCount;
		}
	}

	void MeshInstanceIndex::Clear()
	{
		m_Entries.clear();
		m_EntitySlots.clear();
		m_FreeSlots.clear();
		m_Cells.clear();
		m_Oversized.clear();
		m_Unbounded.clear();
		m_BoundsMin = glm::vec3(0.0f);
		m_BoundsMax = glm::vec3(0.0f);
		m_LiveCount = 0;
		m_SyncedCount = 0;
		m_SyncStamp = 0;
		m_QueryStamp = 0;
	}

	void MeshInstanceIndex::RunQuery(const Query& query, const glm::ivec3& cell_min, const glm::ivec3& cell_max,
		std::vector<entt::entity>& out) const
	{
		++m_QueryStamp;
		if (m_QueryStamp == 0)
		{
			for (const Entry& entry : m_Entries)
				entry.QueryStamp = 0;
			m_QueryStamp = 1;
		}

		const auto consider = [&](uint32_t slot)
		{
			const Entry& entry = m_Entries[slot];
			if (!entry.Alive || entry.QueryStamp == m_QueryStamp)
				return;
			entry.QueryStamp = m_QueryStamp;

			if (!entry.Visible || !Layers::IsVisible(entry.Layer, query.Mask))
				return;

			if (entry.HasBounds && entry.Cullable)
			{
				if (query.UseBounds)
				{
					if (entry.Center.x + entry.Extent.x < query.Min.x || entry.Center.x - entry.Extent.x > query.Max.x
						|| entry.Center.y + entry.Extent.y < query.Min.y || entry.Center.y - entry.Extent.y > query.Max.y
						|| entry.Center.z + entry.Extent.z < query.Min.z || entry.Center.z - entry.Extent.z > query.Max.z)
						return;
				}
				else
				{
					for (uint32_t plane_index = 0; plane_index < query.PlaneCount; ++plane_index)
					{
						const glm::vec4& plane = query.Planes[plane_index];
						const glm::vec3 normal(plane);
						if (glm::dot(normal, entry.Center) + plane.w + glm::dot(glm::abs(normal), entry.Extent) < 0.0f)
							return;
					}
				}
			}

			out.push_back(entry.Entity);
		};

		/* 两种走法取便宜的那个：查询范围里的格子数 vs 已占用的格子数。
		 * 大视锥（远平面 400、格子 8 米就是一百万格）逐格查表会把空转全花在哈希查找上 */
		const int64_t span_x = static_cast<int64_t>(cell_max.x) - cell_min.x + 1;
		const int64_t span_y = static_cast<int64_t>(cell_max.y) - cell_min.y + 1;
		const int64_t span_z = static_cast<int64_t>(cell_max.z) - cell_min.z + 1;
		const int64_t range_cells = span_x * span_y * span_z;

		if (range_cells > static_cast<int64_t>(m_Cells.size()))
		{
			for (const auto& [key, slots] : m_Cells)
			{
				const glm::ivec3 cell = UnpackCell(key);
				if (cell.x < cell_min.x || cell.x > cell_max.x
					|| cell.y < cell_min.y || cell.y > cell_max.y
					|| cell.z < cell_min.z || cell.z > cell_max.z)
					continue;
				for (const uint32_t slot : slots)
					consider(slot);
			}
		}
		else
		{
			for (int32_t x = cell_min.x; x <= cell_max.x; ++x)
			{
				for (int32_t y = cell_min.y; y <= cell_max.y; ++y)
				{
					for (int32_t z = cell_min.z; z <= cell_max.z; ++z)
					{
						const auto iter = m_Cells.find(PackCell(x, y, z));
						if (iter == m_Cells.end())
							continue;
						for (const uint32_t slot : iter->second)
							consider(slot);
					}
				}
			}
		}

		/* 跨格子太多或没有包围盒的：不看格子，直接判定 */
		for (const uint32_t slot : m_Oversized)
			consider(slot);
		for (const uint32_t slot : m_Unbounded)
			consider(slot);
	}

	void MeshInstanceIndex::QueryFrustum(const Frustum& frustum, LayerMask mask, std::vector<entt::entity>& out) const
	{
		if (m_LiveCount == 0)
			return;

		/* 视锥的世界包围盒：查格子范围用，判定仍然逐平面 */
		glm::vec3 bounds_min(0.0f);
		glm::vec3 bounds_max(0.0f);
		frustum.GetWorldBounds(bounds_min, bounds_max);

		Query query;
		query.Planes = frustum.Planes;
		query.PlaneCount = static_cast<uint32_t>(std::size(frustum.Planes));
		query.Mask = mask;

		RunQuery(query,
			{ QuantizeCell(bounds_min.x, m_CellSize), QuantizeCell(bounds_min.y, m_CellSize),
				QuantizeCell(bounds_min.z, m_CellSize) },
			{ QuantizeCell(bounds_max.x, m_CellSize), QuantizeCell(bounds_max.y, m_CellSize),
				QuantizeCell(bounds_max.z, m_CellSize) },
			out);
	}

	void MeshInstanceIndex::QueryAll(LayerMask mask, std::vector<entt::entity>& out) const
	{
		++m_QueryStamp;
		if (m_QueryStamp == 0)
		{
			for (const Entry& entry : m_Entries)
				entry.QueryStamp = 0;
			m_QueryStamp = 1;
		}

		for (const Entry& entry : m_Entries)
		{
			if (!entry.Alive || entry.QueryStamp == m_QueryStamp)
				continue;
			entry.QueryStamp = m_QueryStamp;
			if (!entry.Visible || !Layers::IsVisible(entry.Layer, mask))
				continue;
			out.push_back(entry.Entity);
		}
	}

	void MeshInstanceIndex::QueryBounds(const glm::vec3& min, const glm::vec3& max, LayerMask mask,
		std::vector<entt::entity>& out) const
	{
		if (m_LiveCount == 0)
			return;

		Query query;
		query.UseBounds = true;
		query.Min = min;
		query.Max = max;
		query.Mask = mask;

		RunQuery(query,
			{ QuantizeCell(min.x, m_CellSize), QuantizeCell(min.y, m_CellSize), QuantizeCell(min.z, m_CellSize) },
			{ QuantizeCell(max.x, m_CellSize), QuantizeCell(max.y, m_CellSize), QuantizeCell(max.z, m_CellSize) },
			out);
	}
}
