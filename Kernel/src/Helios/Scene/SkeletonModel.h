#pragma once
#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include "Model.h"

namespace Helios
{
	class MeshSegment;
	class Scene;
	class DeviceStorageBuffer;

	/* .mesh 末尾骨骼段的魔数（SkeletonData 的读写与模型类型探测共用） */
	inline constexpr char kSkeletonFileMagic[4] = { 'H', 'S', 'K', 'N' };

	/* GPU 蒙皮调色板的数组长度（与着色器里的 u_SkinMatrices[64] 对齐） */
	inline constexpr size_t kMaxGpuJoints = 64;

	/* 一根骨骼：名字 + 绑定姿态的逆矩阵（把顶点从模型空间换到骨骼空间）。
	 * 名字与场景里的关节节点对应，运行时靠它找回每个关节的世界变换。 */
	struct SkinJoint
	{
		std::string Name;
		glm::mat4 InverseBind{ 1.0f };
		/* 父关节在关节表里的下标；-1 = 根 */
		int Parent{ -1 };
		/* 静置姿态的局部变换（相对最近的关节祖先）：关节实体按它建，
		 * 动画通道写的也是这个空间的局部值 —— 全程不做任何坐标系转换 */
		glm::mat4 RestLocal{ 1.0f };
	};

	/* 每个顶点最多 4 个骨骼影响（与 glTF 的 JOINTS_0 / WEIGHTS_0 一致） */
	struct SkinWeight
	{
		std::array<uint16_t, 4> Joints{ 0, 0, 0, 0 };
		std::array<float, 4> Weights{ 0.0f, 0.0f, 0.0f, 0.0f };

		/* 有效权重之和（导入后应当约等于 1） */
		[[nodiscard]] float TotalWeight() const
		{
			return Weights[0] + Weights[1] + Weights[2] + Weights[3];
		}
	};

	/* 一个网格段的蒙皮数据：顶点顺序与 .mesh 里的段一致 */
	struct SkinSegment
	{
		std::string Name;
		std::vector<SkinWeight> Vertices;
	};

	/* 资产层的骨骼数据：关节表 + 每个段的顶点权重，直接写在 .mesh 末尾。
	 * 所有用这个模型的实体共享同一份（只读）。 */
	class SkeletonData
	{
	public:
		[[nodiscard]] bool IsEmpty() const { return m_Joints.empty(); }

		[[nodiscard]] const std::vector<SkinJoint>& GetJoints() const { return m_Joints; }
		std::vector<SkinJoint>& GetJoints() { return m_Joints; }

		/* 网格节点世界矩阵的逆：顶点在网格空间，调色板要先换到网格节点空间
		 * （glTF 规范 / three.js / glTF-Sample-Viewer 都是这个式子：Inverse(meshNodeWorld) * jointWorld * IB） */
		[[nodiscard]] const glm::mat4& GetMeshBindMatrix() const { return m_MeshBindMatrix; }
		void SetMeshBindMatrix(const glm::mat4& matrix) { m_MeshBindMatrix = matrix; }

		[[nodiscard]] const std::vector<SkinSegment>& GetSegments() const { return m_Segments; }
		std::vector<SkinSegment>& GetSegments() { return m_Segments; }

		/* 按名字找关节下标；找不到返回 -1 */
		[[nodiscard]] int FindJoint(const std::string& name) const;

		/* 骨骼矩阵调色板：palette[i] = 关节世界变换 * 绑定逆矩阵 */
		[[nodiscard]] std::vector<glm::mat4> BuildPalette(const std::vector<glm::mat4>& joint_world) const;

		/* CPU 蒙皮：把一个顶点按调色板算回模型空间 */
		[[nodiscard]] static glm::vec3 SkinPosition(const glm::vec3& position, const SkinWeight& weight,
			const std::vector<glm::mat4>& palette);

		/* 读写 .mesh 末尾的蒙皮段（读之前先比对魔数，没有这段返回 false 且不改已有数据） */
		bool Write(std::ostream& stream) const;
		bool Read(std::istream& stream);

	private:
		std::vector<SkinJoint> m_Joints;
		std::vector<SkinSegment> m_Segments;
		glm::mat4 m_MeshBindMatrix{ 1.0f };
	};

	/* 骨骼模型的实例数据：每个挂了蒙皮模型的实体一份（实体自己的动态网格与调色板）。
	 * 几何 / 材质仍来自共享的 Model 资产；骨骼相关的操作（建关节实体、取关节变换、
	 * 每帧重算上屏顶点）都收在这里，Scene 只需要调 UpdateScene。 */
	class SkeletonModel final : public Model
	{
	public:
		explicit SkeletonModel(std::string path);
		~SkeletonModel() override = default;

		[[nodiscard]] bool IsStaticModel() const override { return false; }

		/* 每个实体一份实例（不走共享缓存，运行时数据不会串） */
		static SharedPtr<SkeletonModel> Create(const std::string& path);

		/* 按源模型的段建动态网格段（第一次用到时调一次，已建好直接返回） */
		void EnsureSkinnedSegments();

		[[nodiscard]] const std::vector<SharedPtr<MeshSegment>>& GetSkinnedSegments() const { return m_SkinnedSegments; }
		/* 上屏段就是当前姿势的动态段；场景还没跑过一帧（动态段没建）时回落到源段 */
		[[nodiscard]] const std::vector<SharedPtr<MeshSegment>>& GetRenderSegments() const override
		{
			return m_SkinnedSegments.empty() ? Model::GetMeshSegments() : m_SkinnedSegments;
		}
		[[nodiscard]] std::vector<std::vector<float>>& GetSkinnedVertices() { return m_SkinnedVertices; }
		[[nodiscard]] std::vector<entt::entity>& GetJointEntities() { return m_Joints; }
		[[nodiscard]] const std::vector<entt::entity>& GetJointEntities() const { return m_Joints; }
		[[nodiscard]] std::vector<glm::mat4>& GetPalette() { return m_Palette; }

		/* GPU 蒙皮是否就绪（后端支持计算着色器才可能为真；不成立时走 CPU） */
		[[nodiscard]] bool IsGpuSkinned() const { return m_GpuReady; }
		/* 开关 GPU 蒙皮（默认开；检查里可以关掉跑 CPU 路径做对照） */
		void SetGpuSkinningEnabled(bool enabled) { m_GpuSkinningEnabled = enabled; }
		[[nodiscard]] bool IsGpuSkinningEnabled() const { return m_GpuSkinningEnabled; }

		/* 全局开关（编辑器菜单）：关掉后所有骨骼模型都走 CPU 蒙皮 */
		static void SetGlobalGpuSkinningEnabled(bool enabled);
		[[nodiscard]] static bool IsGlobalGpuSkinningEnabled();
		/* 读回 GPU 蒙皮的结果（检查用；读之前要 WaitForGPU） */
		[[nodiscard]] void* GetGpuSkinnedData(size_t segment) const;

		/* 按关节表在 root 下建一套关节实体（名字 / 父子关系 / 绑定姿态的局部变换），
		 * 返回与关节表同下标的实体列表 */
		static std::vector<entt::entity> CreateJointEntities(Scene& scene, entt::entity root, const SkeletonData& data);

		/* 取每个关节相对 root 的变换（绑定逆矩阵是模型空间的，要除掉模型自身的变换） */
		static std::vector<glm::mat4> CollectJointTransforms(const Scene& scene,
			const std::vector<entt::entity>& joints, entt::entity root);

		/* 重新导入骨骼：销毁实体下现有的关节子实体，按当前模型的关节表重建（换模型 / 重新导入后用） */
		void Rebind(Scene& scene, entt::entity owner);

		/* 场景每帧调用：所有挂蒙皮模型的实体重算上屏顶点（开关关了跳过；调色板没变跳过） */
		static void UpdateScene(Scene& scene);

		/* 把这一帧改过姿势的骨骼顶点交给渲染图的蒙皮 Pass（排在所有绘制 Pass 之前） */
		static void AddGpuSkinPass(class RenderView& render_view);

		/* 派发这一帧的 GPU 蒙皮：渲染图的蒙皮 Pass 每帧调一次，
		 * 没有待算的直接返回 false（多视图时后面的 Pass 就是空跑） */
		bool DispatchGpuSkin();

		/* 丢掉某个实体的运行时数据（换模型 / 手工解绑时用） */
		static void Clear(Scene& scene, entt::entity entity);

	private:
		/* 单个实体：解析关节、算调色板、重写动态顶点 */
		void UpdatePose(Scene& scene, entt::entity owner);

		/* CPU 蒙皮：按当前调色板重算一遍动态顶点并上传 */
		void SkinOnCpu();

		/* 关节：从模型实体往下整棵子树按名字对上关节表 */
		[[nodiscard]] std::vector<entt::entity> ResolveJoints(Scene& scene, entt::entity owner) const;

		/* 找到的关节是不是本来就跟当前关节表配套（数量 / 父链都对得上） */
		[[nodiscard]] bool JointTreeMatches(Scene& scene, entt::entity owner,
			const std::vector<entt::entity>& joints) const;

		/* 上屏用的动态网格段（顶点缓冲可写） */
		std::vector<SharedPtr<MeshSegment>> m_SkinnedSegments;
		/* 每段的 CPU 顶点镜像（上传源，也是检查读取处） */
		std::vector<std::vector<float>> m_SkinnedVertices;
		/* 按关节表顺序绑定的关节实体 */
		std::vector<entt::entity> m_Joints;
		/* 上一帧的调色板：没变就不重算顶点 */
		std::vector<glm::mat4> m_Palette;
		/* 自动建关节只尝试一次（用户删掉关节后不再偷偷建回来） */
		bool m_AutoBindTried{ false };

		/* GPU 蒙皮的一整套缓冲：绑定顶点 / 权重（8 float 每顶点）/ 上屏顶点 / 调色板 */
		struct GpuSegment
		{
			SharedPtr<DeviceStorageBuffer> BindVertices;
			SharedPtr<DeviceStorageBuffer> Weights;
			SharedPtr<DeviceStorageBuffer> Skinned;
			uint32_t VertexCount{ 0 };
		};
		std::vector<GpuSegment> m_GpuSegments;
		SharedPtr<DeviceStorageBuffer> m_GpuPalette;
		bool m_GpuReady{ false };
		bool m_GpuSkinningEnabled{ true };
		/* 这一帧的姿势还没算到 GPU 缓冲里（等渲染图的蒙皮 Pass 派发） */
		bool m_GpuSkinPending{ false };
		/* 上一帧走的哪条路径：切换全局开关时据此让调色板缓存失效、重新算一遍 */
		bool m_LastGpuActive{ false };
	};

	/* 非静态模型按约定就是骨骼模型（Model::Create 按 .mesh 有没有骨骼段定类型）：
	 * 转换统一收在这里，调用方不用到处 static_cast。 */
	[[nodiscard]] SkeletonModel* AsSkeletonModel(Model* model);
	[[nodiscard]] const SkeletonModel* AsSkeletonModel(const Model* model);
}
