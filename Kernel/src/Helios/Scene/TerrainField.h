#pragma once
#include <cstdint>
#include <limits>
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
	class RenderView;

	/* 地形 patch：所有 patch 共用同一个 33×33 模板网格，位置与 LOD 由实例属性给出，
	 * 高度在顶点着色器里采样高度图。结构与 Terrain.glsl 的 location 5/6 逐字节对应。
	 * 坐标是地形自己的局部空间（原点在地形中心、xz 平面），世界位置由 u_TerrainModel 变换。 */
	struct TerrainPatch
	{
		glm::vec4 Info{ 0.0f }; /* xyz = 左下角局部坐标，w = 覆盖边长（米） */
		glm::vec4 Lod{ 0.0f };  /* x = 级数（0 最细），y = 采样步长，zw 保留 */
	};

	/* 一块地形的高度数据与 patch 划分。
	 * 高度图在局部空间生成，只跟参数与资产有关：换变换只更新矩阵，
	 * 高度图不动，拖 gizmo 不会重生成整座山。 */
	class HELIOS_API TerrainField
	{
	public:
		/* 参数没变只更新矩阵；换参数 / 资产才重建高度图 */
		void Rebuild(const TerrainComponent& component, const glm::mat4& world);

		/* 按视锥生成 patch 列表：从根节点递归细分，节点边长与距离的比值够了就停。
		 * 只做视锥剔除，不含遮挡与背面锥（见方案文档 P4 之后的补充项）。 */
		void BuildPatches(const Frustum& frustum, const glm::vec3& eye,
			std::vector<TerrainPatch>& out) const;
		/* 按球范围生成 patch 列表（探针捕获用）：没有固定视锥，超出范围的节点整块跳过 */
		void BuildPatchesAround(const glm::vec3& center, float radius,
			std::vector<TerrainPatch>& out) const;

		[[nodiscard]] const SharedPtr<DeviceTexture>& GetHeightMap() const { return m_HeightMap; }
		/* 地形中心的世界坐标（实体位置） */
		[[nodiscard]] glm::vec3 GetOrigin() const { return m_Origin; }
		/* 局部空间半宽半长（米），不含实体缩放 */
		[[nodiscard]] glm::vec2 GetExtent() const { return m_Extent; }
		[[nodiscard]] uint32_t GetResolution() const { return m_Resolution; }
		/* 局部空间的起伏幅度（米），不含实体缩放 */
		[[nodiscard]] float GetHeightScale() const { return m_HeightScale; }
		/* 实体到世界的变换与逆矩阵：patch 顶点走它，世界点反查局部也走它 */
		[[nodiscard]] const glm::mat4& GetWorldTransform() const { return m_World; }
		[[nodiscard]] const glm::mat4& GetWorldInverse() const { return m_WorldInverse; }
		/* 法线的变换矩阵（逆转置，顶点着色器按 3×3 用） */
		[[nodiscard]] const glm::mat4& GetNormalMatrix() const { return m_NormalMatrix; }
		/* 三个轴的世界长度：世界包围盒估算用 */
		[[nodiscard]] glm::vec3 GetWorldScale() const { return m_WorldScale; }
		/* 地形在世界 xz 平面上的半宽半长（轴对齐包围盒，含旋转） */
		[[nodiscard]] glm::vec2 GetWorldHalfExtent() const;
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
		/* 世界坐标反查局部 xz：绕 Y 轴的地形是精确的，带俯仰时按当前高度迭代几次 */
		[[nodiscard]] glm::vec2 ToLocalXZ(const glm::vec2& world_xz, float world_y = 0.0f) const;
		/* 局部 xz 与高度 → 世界坐标 */
		[[nodiscard]] glm::vec3 ToWorld(const glm::vec2& local_xz, float local_height) const;
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

		/* GPU 选择：计算着色器按同一套四叉树判据选 patch 并压实，顺带写好间接参数。
		 * 要在视图开始画之前派发（计算编码器会把当前渲染编码器收掉）。
		 * 一帧可能有多个视图（编辑器 / 游戏 / 工具相机），每块地形按视图各存一份结果，
		 * 否则后画的视图会把先准备的覆盖掉。不支持计算或没开开关时返回 false */
		bool PrepareSelection(const glm::vec4 (&planes)[6], const glm::vec3& eye, uint32_t frame,
			const RenderView* view);
		/* 这个视图的这一帧已经由 GPU 选过 patch，提交时直接走间接绘制 */
		[[nodiscard]] bool HasSelection(uint32_t frame, const RenderView* view) const;
		/* GPU 选完的实例缓冲与间接参数：找不到该视图的选择结果时为空 */
		[[nodiscard]] const SharedPtr<class DeviceVertexBuffer>& GetSelectedInstances(
			const RenderView* view) const;
		[[nodiscard]] const SharedPtr<class DeviceStorageBuffer>& GetSelectedArgs(
			const RenderView* view) const;
		/* 上一帧选出的 patch 数：参数块是 CPU 可见的，读它不用等回读 */
		[[nodiscard]] uint32_t GetSelectedPatchCount(const RenderView* view) const;
		/* 压实后的 patch 实例（CPU 可见，排查用） */
		[[nodiscard]] const SharedPtr<class DeviceStorageBuffer>& GetSelectedPatches(
			const RenderView* view) const;

		/* GPU 选择的开关：关掉走 CPU 四叉树；不支持计算的后端自动走 CPU */
		static void SetGpuSelectionEnabled(bool enabled);
		[[nodiscard]] static bool IsGpuSelectionEnabled();

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
		/* 模板网格的索引数：32 段本体 + 外圈裙边，一共 34×34 个四边形。
		 * 间接绘制的参数块按它填，漏掉裙边那圈就会在 LOD 交界露出裂缝 */
		static constexpr uint32_t kPatchQuads = 32;
		static constexpr uint32_t kPatchIndexCount = (kPatchQuads + 2) * (kPatchQuads + 2) * 6;

	private:
		void BuildHeightMap(const TerrainComponent& component);
		/* 节点表：按层摊平的满四叉树，节点只跟范围与最细节点边长有关，重建时生成一次 */
		void BuildNodeTable();
		/* 高度图上传：纹理 + CPU 副本，两种来源共用 */
		void UploadHeightMap(std::vector<float> heights);
		/* frustum 为空表示不做视锥剔除；max_distance 为 0 表示不限距离 */
		/* 标记哪些 patch 需要 morph：只有挨着更细节点的才做。
		 * 相邻两侧都往各自父级收敛会收敛到不同精度的表面，反而裂开 */
		static void MarkMorphPatches(std::vector<TerrainPatch>& patches);
		/* 2:1 平衡：相邻叶子最多差一档，否则 morph 对不上 */
		static void BalancePatches(std::vector<TerrainPatch>& patches);

		void CollectNodes(const Frustum* frustum, const glm::vec3& eye, float max_distance,
			const glm::vec2& local_center, float local_size, uint32_t lod,
			std::vector<TerrainPatch>& out) const;
		/* 世界 xz → 局部 xz：带旋转时高度会参与，按给定世界高度算一遍 */
		[[nodiscard]] glm::vec2 WorldToLocal(const glm::vec2& world_xz, float world_y = 0.0f) const;
		/* 地形局部包围盒（8 角点）变换到世界后的 y 范围 */
		void GetWorldHeightBounds(float& out_min, float& out_max) const;

		SharedPtr<DeviceTexture> m_HeightMap;
		/* 高度图的 CPU 副本（归一化 0..1）：贴地查询用，GPU 放置后续也读它 */
		std::vector<float> m_Heights;
		mutable SharedPtr<class DeviceStorageBuffer> m_HeightStorage;
		glm::vec3 m_Origin{ 0.0f };
		glm::mat4 m_World{ 1.0f };
		glm::mat4 m_WorldInverse{ 1.0f };
		glm::mat4 m_NormalMatrix{ 1.0f };
		glm::vec3 m_WorldScale{ 1.0f };
		glm::vec2 m_Extent{ 512.0f, 512.0f }; /* 局部半宽半长 */
		uint32_t m_Resolution{ 512 };
		float m_HeightScale{ 40.0f };
		/* 本次高度图的来源（资产路径；程序化时为空） */
		std::string m_AssetPath;
		TerrainVirtualTexture m_VirtualTexture;
		/* 上一次烘页时的材质版本：改了层颜色 / 层贴图就把页表作废 */
		uint32_t m_MaterialRevision{ 0 };

		/* GPU 选择：节点表（xy 局部中心、z 边长、w 级数）与它对应的显存 */
		std::vector<glm::vec4> m_NodeTable;
		uint32_t m_MaxNodeLevel{ 0 };
		mutable SharedPtr<class DeviceStorageBuffer> m_NodeStorage;
		uint32_t m_NodeTableRevision{ 0 };
		uint32_t m_NodeUploadedRevision{ 0 };

		/* 每个视图一份选择结果：压实后的实例、间接参数，以及它是哪一帧选的 */
		struct SelectionSlot
		{
			const RenderView* View{ nullptr };
			uint32_t Frame{ std::numeric_limits<uint32_t>::max() };
			SharedPtr<class DeviceStorageBuffer> Storage;
			SharedPtr<class DeviceVertexBuffer> ViewBuffer;
			SharedPtr<class DeviceStorageBuffer> Args;
		};
		/* 一个场景一帧里可能同时有游戏相机、编辑器相机与工具相机，留够 4 份 */
		static constexpr uint32_t kSelectionSlots = 4;
		SelectionSlot m_Selections[kSelectionSlots];
		[[nodiscard]] SelectionSlot* FindSelection(const RenderView* view);
		[[nodiscard]] const SelectionSlot* FindSelection(const RenderView* view) const;
		/* 节点边长（米）与相机距离的比值，小于它才继续细分。8 表示最近处一个节点约 8 个节点宽 */
		float m_LodRatio{ 6.0f };
		uint32_t m_MinNodeSize{ 64 };
		uint64_t m_BuildKey{ 0 };
	};
}
