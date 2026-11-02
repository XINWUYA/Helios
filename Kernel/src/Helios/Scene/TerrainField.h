#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "Helios/Common/Export.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Scene/Mesh.h"
#include "Helios/Scene/TerrainVirtualTexture.h"
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
		/* 按球范围生成 patch 列表（探针捕获用）：没有固定视锥，超出范围的节点整块跳过 */
		void BuildPatchesAround(const glm::vec3& center, float radius,
			std::vector<TerrainPatch>& out) const;

		[[nodiscard]] const SharedPtr<DeviceTexture>& GetHeightMap() const { return m_HeightMap; }
		[[nodiscard]] glm::vec3 GetOrigin() const { return m_Origin; }
		[[nodiscard]] glm::vec2 GetExtent() const { return m_Extent; }
		[[nodiscard]] uint32_t GetResolution() const { return m_Resolution; }
		[[nodiscard]] float GetHeightScale() const { return m_HeightScale; }
		[[nodiscard]] bool IsEmpty() const { return m_HeightMap == nullptr; }
		/* 高度数据来自 .terrain 资产时给出资产路径，程序化生成时为空 */
		[[nodiscard]] bool IsFromAsset() const { return !m_AssetPath.empty(); }
		[[nodiscard]] const std::string& GetAssetPath() const { return m_AssetPath; }

		/* 世界高度采样（CPU 侧）：草地与其它物体贴地时用。
		 * 点不在地形范围内返回 false，out_height 不动 */
		bool SampleHeightWorld(const glm::vec2& world_xz, float& out_height) const;
		/* 表面法线（CPU 侧，与着色器的邻居差分同口径）：坡度过滤与贴面对齐用 */
		bool SampleNormalWorld(const glm::vec2& world_xz, glm::vec3& out_normal) const;
		/* 世界 xz 是否落在地形覆盖范围内（不管高度） */
		[[nodiscard]] bool ContainsPoint(const glm::vec2& world_xz) const;
		/* 地形表面的世界高度范围（含实体位置的 y） */
		[[nodiscard]] float GetMinWorldHeight() const;
		[[nodiscard]] float GetMaxWorldHeight() const;
		/* 一块 XZ 矩形内的表面高度范围：贴地对象的包围盒按它算，比整张地形的范围紧
		 * （矩形完全在地形外返回 false）。max_samples 给范围估计用：每轴最多取这么多个点，
		 * 0 表示逐纹素扫 */
		bool GetHeightRange(const glm::vec2& min_xz, const glm::vec2& max_xz,
			float& out_min, float& out_max, uint32_t max_samples = 0) const;
		/* 最细节点边长（米）：草地块网格跟它对齐时用 */
		[[nodiscard]] uint32_t GetMinNodeSize() const { return m_MinNodeSize; }
		/* 高度数组的拷贝：后台重建任务拿一份快照，不跟地形共用指针 */
		[[nodiscard]] std::vector<float> CopyHeights() const { return m_Heights; }
		/* 高度重建的版本号：贴地对象据此判断要不要重新采样 */
		[[nodiscard]] uint64_t GetRevision() const { return m_BuildKey; }

		/* 虚拟纹理：高度重建时页表作废，几何不动时每帧只补缺页 */
		[[nodiscard]] TerrainVirtualTexture& GetVirtualTexture() { return m_VirtualTexture; }
		[[nodiscard]] const TerrainVirtualTexture& GetVirtualTexture() const { return m_VirtualTexture; }

		/* 高度数据的 GPU 副本（float 数组，归一化 0..1）：计算着色器里的贴地采样用。
		 * 第一次要的时候才建，CPU 侧不改高度就不会重建 */
		const SharedPtr<class DeviceStorageBuffer>& GetHeightStorage() const;

		/* morph 带宽缩放：1 = 默认（边长的四分之一），0 = 关掉。
		 * 只给检查程序做开关对照，正常渲染别动它 */
		static void SetGlobalMorphScale(float scale);
		[[nodiscard]] static float GetGlobalMorphScale();

		/* 模板网格：33×33 顶点，顶点只有 UV（0..1）。所有地形共用一份。 */
		static const MeshPrimitive& GetPatchPrimitive();
		/* 实例数据布局，与 Terrain.glsl 的 location 5/6 对应 */
		static const VertexBufferLayout& GetInstanceLayout();
		/* 模板网格的索引数：间接参数里用得上 */
		static constexpr uint32_t kPatchQuads = 32;
		static constexpr uint32_t kPatchIndexCount = kPatchQuads * kPatchQuads * 6;

	private:
		void BuildHeightMap(const TerrainComponent& component);
		/* 高度图上传：纹理 + CPU 副本，两种来源共用 */
		void UploadHeightMap(std::vector<float> heights);
		/* frustum 为空表示不做视锥剔除；max_distance 为 0 表示不限距离 */
		void CollectNodes(const Frustum* frustum, const glm::vec3& eye, float max_distance,
			const glm::vec3& center, float size, uint32_t lod, std::vector<TerrainPatch>& out) const;

		SharedPtr<DeviceTexture> m_HeightMap;
		/* 高度图的 CPU 副本（归一化 0..1）：贴地查询用，GPU 放置后续也读它 */
		std::vector<float> m_Heights;
		mutable SharedPtr<class DeviceStorageBuffer> m_HeightStorage;
		glm::vec3 m_Origin{ 0.0f };
		glm::vec2 m_Extent{ 512.0f, 512.0f }; /* 半宽半长 */
		uint32_t m_Resolution{ 512 };
		float m_HeightScale{ 40.0f };
		/* 本次高度图的来源（资产路径；程序化时为空） */
		std::string m_AssetPath;
		TerrainVirtualTexture m_VirtualTexture;
		/* 节点边长（米）与相机距离的比值，小于它才继续细分。8 表示最近处一个节点约 8 个节点宽 */
		float m_LodRatio{ 6.0f };
		uint32_t m_MinNodeSize{ 64 };
		uint64_t m_BuildKey{ 0 };
	};
}
