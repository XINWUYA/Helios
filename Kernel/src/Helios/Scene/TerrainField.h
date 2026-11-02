#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "Helios/Common/Export.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Scene/Mesh.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"

namespace Helios
{
	struct TerrainComponent;
	struct Frustum;
	class DeviceTexture;

	/* 地形 patch：所有 patch 共用同一个 33×33 模板网格，世界位置与 LOD 由实例属性给出，
	 * 高度在顶点着色器里采样高度图。结构与 Terrain.glsl 的 location 5/6 逐字节对应。 */
	struct TerrainPatch
	{
		glm::vec4 Info{ 0.0f }; /* xyz = 左下角世界坐标，w = 覆盖边长（米） */
		glm::vec4 Lod{ 0.0f };  /* x = 级数（0 最细），y = 采样步长，zw 保留 */
	};

	/* 一块地形的高度数据与 patch 划分。
	 * 高度图按程序化噪声生成（M1 不做资产导入），顶点不落地：
	 * 换参数或换变换才重建，静止地形不产生每帧开销。 */
	class HELIOS_API TerrainField
	{
	public:
		/* 重建高度图（参数与变换没变时直接返回） */
		void Rebuild(const TerrainComponent& component, const glm::mat4& world);

		/* 按视锥生成 patch 列表：从根节点递归细分，节点边长与距离的比值够了就停。
		 * 只做视锥剔除，不含遮挡与背面锥（见方案文档 P4 之后的补充项）。 */
		void BuildPatches(const Frustum& frustum, const glm::vec3& eye,
			std::vector<TerrainPatch>& out) const;

		[[nodiscard]] const SharedPtr<DeviceTexture>& GetHeightMap() const { return m_HeightMap; }
		[[nodiscard]] glm::vec3 GetOrigin() const { return m_Origin; }
		[[nodiscard]] glm::vec2 GetExtent() const { return m_Extent; }
		[[nodiscard]] uint32_t GetResolution() const { return m_Resolution; }
		[[nodiscard]] float GetHeightScale() const { return m_HeightScale; }
		[[nodiscard]] bool IsEmpty() const { return m_HeightMap == nullptr; }

		/* 模板网格：33×33 顶点，顶点只有 UV（0..1）。所有地形共用一份。 */
		static const MeshPrimitive& GetPatchPrimitive();
		/* 实例数据布局，与 Terrain.glsl 的 location 5/6 对应 */
		static const VertexBufferLayout& GetInstanceLayout();
		/* 模板网格的索引数：间接参数里用得上 */
		static constexpr uint32_t kPatchQuads = 32;
		static constexpr uint32_t kPatchIndexCount = kPatchQuads * kPatchQuads * 6;

	private:
		void BuildHeightMap(const TerrainComponent& component);
		void CollectNodes(const Frustum& frustum, const glm::vec3& eye, const glm::vec3& center,
			float size, uint32_t lod, std::vector<TerrainPatch>& out) const;

		SharedPtr<DeviceTexture> m_HeightMap;
		glm::vec3 m_Origin{ 0.0f };
		glm::vec2 m_Extent{ 512.0f, 512.0f }; /* 半宽半长 */
		uint32_t m_Resolution{ 512 };
		float m_HeightScale{ 40.0f };
		/* 节点边长（米）与相机距离的比值，小于它才继续细分。8 表示最近处一个节点约 8 个节点宽 */
		float m_LodRatio{ 6.0f };
		uint32_t m_MinNodeSize{ 64 };
		uint64_t m_BuildKey{ 0 };
	};
}
