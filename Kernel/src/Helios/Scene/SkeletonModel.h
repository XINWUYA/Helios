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

	/* .mesh 末尾骨骼段的魔数（SkeletonData 的读写与模型类型探测共用） */
	inline constexpr char kSkeletonFileMagic[4] = { 'H', 'S', 'K', 'N' };

	/* 一根骨骼：名字 + 绑定姿态的逆矩阵（把顶点从模型空间换到骨骼空间）。
	 * 名字与场景里的关节节点对应，运行时靠它找回每个关节的世界变换。 */
	struct SkinJoint
	{
		std::string Name;
		glm::mat4 InverseBind{ 1.0f };
		/* 父关节在关节表里的下标；-1 = 根。建关节实体与算局部绑定变换要用 */
		int Parent{ -1 };
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
		[[nodiscard]] std::vector<std::vector<float>>& GetSkinnedVertices() { return m_SkinnedVertices; }
		[[nodiscard]] std::vector<entt::entity>& GetJointEntities() { return m_Joints; }
		[[nodiscard]] std::vector<glm::mat4>& GetPalette() { return m_Palette; }

		/* 按关节表在 root 下建一套关节实体（名字 / 父子关系 / 绑定姿态的局部变换），
		 * 返回与关节表同下标的实体列表 */
		static std::vector<entt::entity> CreateJointEntities(Scene& scene, entt::entity root, const SkeletonData& data);

		/* 取每个关节相对 root 的变换（绑定逆矩阵是模型空间的，要除掉模型自身的变换） */
		static std::vector<glm::mat4> CollectJointTransforms(const Scene& scene,
			const std::vector<entt::entity>& joints, entt::entity root);

		/* 场景每帧调用：所有挂蒙皮模型的实体重算上屏顶点（开关关了跳过；调色板没变跳过） */
		static void UpdateScene(Scene& scene);

		/* 丢掉某个实体的运行时数据（换模型 / 手工解绑时用） */
		static void Clear(Scene& scene, entt::entity entity);

	private:
		/* 单个实体：解析关节、算调色板、重写动态顶点 */
		void UpdatePose(Scene& scene, entt::entity owner);

		/* 关节：从模型实体往下整棵子树按名字对上关节表 */
		[[nodiscard]] std::vector<entt::entity> ResolveJoints(Scene& scene, entt::entity owner) const;

		/* 上屏用的动态网格段（顶点缓冲可写） */
		std::vector<SharedPtr<MeshSegment>> m_SkinnedSegments;
		/* 每段的 CPU 顶点镜像（上传源，也是检查读取处） */
		std::vector<std::vector<float>> m_SkinnedVertices;
		/* 按关节表顺序绑定的关节实体 */
		std::vector<entt::entity> m_Joints;
		/* 上一帧的调色板：没变就不重算顶点 */
		std::vector<glm::mat4> m_Palette;
	};
}
