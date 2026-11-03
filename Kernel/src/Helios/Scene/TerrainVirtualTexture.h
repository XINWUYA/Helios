#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "Helios/Common/Export.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/VirtualDevice/DeviceFrameBuffer.h"

namespace Helios
{
	class DeviceTexture;
	class TerrainField;
	struct TerrainPatch;

	/* 地形虚拟纹理（albedo 那一路）：64 米一页、256² 纹素，页池按层号存。
	 * 着色时查页表，驻留就采页池，不驻留回退到逐像素直接算 —— 两边混色公式同源，
	 * 差的只是采样密度，所以开关/调度不会出现观感断层。
	 * 法线不进页池：高度图本来就在手边，逐像素差分比重建一张法线页便宜。 */
	class HELIOS_API TerrainVirtualTexture
	{
	public:
		static constexpr float kPageWorldSize = 64.0f;
		static constexpr uint32_t kPageTexels = 256;
		/* 页表上限 64×64 = 4km 见方；超出的部分不驻留，走直接算 */
		static constexpr uint32_t kMaxGrid = 64;
		/* 页池层数：256² × 128 层 RGBA8 = 32MB */
		static constexpr uint32_t kPoolLayers = 128;

		/* 全局开关与每帧烘焙预算（editor.cfg 与检查程序共用） */
		static void SetEnabled(bool enabled);
		[[nodiscard]] static bool IsEnabled();
		static void SetBakeBudget(uint32_t pages_per_frame);
		[[nodiscard]] static uint32_t GetBakeBudget();

		/* 高度数据重建过：页表清空，页池不重建（下一帧重新烘） */
		void Reset();

		/* 按本帧 patch 覆盖挑页：缺页按相机距离从近到远排，取预算内的那批 */
		void Prepare(const TerrainField& field, const TerrainPatch* patches, size_t patch_count,
			const glm::vec3& eye);

		/* 按视锥挑页：patch 在 GPU 上选的时候 CPU 没有 patch 列表，
		 * 改用页网格与视锥求交，覆盖范围与按 patch 挑是同一片可见区域 */
		void PrepareFrustum(const TerrainField& field, const glm::vec4 (&planes)[6], const glm::vec3& eye);

		/* 本帧已烘完：把层号写进页表（一次上传），清掉待烘队列 */
		void Commit();

		/* 没启用 VT 时给页池采样器用的占位：必须是 2D 数组，绑 2D 白图 Metal 会断言 */
		[[nodiscard]] static const SharedPtr<DeviceTexture>& GetFallbackAtlas();

		/* 页池 / 页表（没启用或没图形上下文时为空） */
		[[nodiscard]] bool EnsureResources();
		[[nodiscard]] const SharedPtr<DeviceTexture>& GetPool() const { return m_Pool; }
		[[nodiscard]] const SharedPtr<DeviceTexture>& GetPageTable() const { return m_PageTable; }
		[[nodiscard]] const glm::vec2& GetGrid() const { return m_Grid; }
		[[nodiscard]] const std::vector<glm::uvec3>& GetPending() const { return m_Pending; }
		/* 烘页用的离屏帧缓冲（页池的一层当颜色附件），随页池一起建 */
		[[nodiscard]] const SharedPtr<DeviceFrameBuffer>& GetBakeFrameBuffer();
		[[nodiscard]] bool HasPending() const { return !m_Pending.empty(); }
		/* 页表里已驻留的页数（检查程序与面板读数用） */
		[[nodiscard]] uint32_t GetResidentPages() const;

	private:
		void ClearTable();
		/* 页表 / 网格就绪，返回本帧的地形左下角与网格尺寸 */
		bool BeginFrame(const TerrainField& field, glm::vec2& out_min_corner,
			uint32_t& out_grid_w, uint32_t& out_grid_h);
		/* 从标记好的缺页里按距离挑出预算内的，排进待烘队列 */
		void QueueMissing(const std::vector<uint8_t>& needed, const glm::vec2& min_corner,
			uint32_t grid_w, uint32_t grid_h, const glm::vec3& eye);

		SharedPtr<DeviceTexture> m_Pool;
		SharedPtr<DeviceTexture> m_PageTable;
		SharedPtr<DeviceFrameBuffer> m_BakeFrameBuffer;
		/* 页表 CPU 副本：0 = 未驻留，否则层号 + 1 */
		std::vector<float> m_Table;
		std::vector<glm::uvec2> m_LayerPages;   /* 层号 → 页坐标（覆盖时要把旧页的页表项清掉） */
		glm::vec2 m_Grid{ 0.0f };
		uint32_t m_NextLayer{ 0 };

		/* 待烘队列：x = 页 x，y = 页 z，z = 目标层号 */
		std::vector<glm::uvec3> m_Pending;
	};
}
