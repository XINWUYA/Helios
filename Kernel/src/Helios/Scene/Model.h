#pragma once
#include <glm/glm.hpp>
#include "Material.h"
#include "SceneObject.h"

namespace Helios
{
	class MeshSegment;

	/* 内建模型类型 */
	enum class BuiltinModelType : uint8_t
	{
		Cube,
		Sphere,
		Plane
	};

	/* 内建模型的身份名（BuiltinCube / BuiltinSphere / BuiltinPlane）：
	 * 既是 Model::GetPath()，也是场景序列化里的身份标记 —— 内建模型没有资产文件，
	 * 保存 / 加载都靠这个名字往返（见组件注册表的 SaveModelPath / LoadModelPath）。 */
	[[nodiscard]] const char* BuiltinModelName(BuiltinModelType type);

	/* 反查：身份名 -> 类型（非内建名返回 false） */
	[[nodiscard]] bool TryParseBuiltinModelName(const std::string& name, BuiltinModelType& out_type);

	/* 模型类：默认静态模型，仅支持 .mesh（由 ModelEditor 导出）。几何和材质解耦 —— 模型只描述
	 * 几何与"材质槽表"；具体绑哪份材质由实体槽覆盖与模型默认绑定的解析链决定（见 ResolveSlotMaterial）。 */
	class Model : public SceneObject
	{
	public:
		Model(std::string path);
		virtual ~Model() = default;

		/* 标记是否为静态模型 */
		[[nodiscard]] virtual bool IsStaticModel() const { return true; }

		/* 模型路径 */
		[[nodiscard]] const std::string& GetPath() const { return m_Path; }
		/* 内建模型类型（无资产文件、程序化生成）；非内建模型返回 false。
		 * UI（属性面板 / 场景树图标）与序列化判断"要不要当路径处理"都走它。 */
		[[nodiscard]] bool TryGetBuiltinType(BuiltinModelType& out_type) const;
		/* 模型的AABB */
		[[nodiscard]] const glm::vec3& GetAABBMin() const { return m_AABBMin; }
		[[nodiscard]] const glm::vec3& GetAABBMax() const { return m_AABBMax; }

		/* 模型缩放 */
		[[nodiscard]] const glm::vec3& GetScale() const { return m_Scale; }
		void SetScale(const glm::vec3& scale) { m_Scale = scale; }

		/* 由世界变换矩阵写入位置、旋转与缩放 */
		void SetTransform(const glm::mat4& transform) override;

		/* 添加一个MeshSegment到模型 */
		void AddMeshSegment(const SharedPtr<MeshSegment>& mesh_segment);
		/* 获取所有MeshSegments */
		[[nodiscard]] const std::vector<SharedPtr<MeshSegment>>& GetMeshSegments() const { return m_MeshSegments; }

		[[nodiscard]] const SharedPtr<MaterialGroup>& GetMaterialGroup() const { return m_pMaterialGroup; }

		/* 从路径加载一个模型， 仅支持.mesh文件 */
		static SharedPtr<Model> Create(const std::string& path);
		/* 创建内建模型 */
		static SharedPtr<Model> Create(BuiltinModelType type, const SharedPtr<Material>& material = Material::Default());

	private:
		/* 加载模型时，加载材质 */
		void LoadMaterial(const std::string& path);

		/* 文件路径 */
		std::string m_Path{};
		/* 一个模型中包含的子模型 */
		std::vector<SharedPtr<MeshSegment>> m_MeshSegments{};
		/* 模型对应的材质 */
		SharedPtr<MaterialGroup> m_pMaterialGroup{ nullptr };
		/* AABB */
		glm::vec3 m_AABBMin;
		glm::vec3 m_AABBMax;
		/* 缩放 */
		glm::vec3 m_Scale{ 1.0f };

		friend class SkeletonModel;
	};

	/* 骨骼模型类 */
	class SkeletonModel final : public Model
	{
	public:
		explicit SkeletonModel(const std::string& path);
		~SkeletonModel() override = default;

		/* 标记是否为静态模型 */
		[[nodiscard]] bool IsStaticModel() const override { return false; }

		/* 从路径加载一个骨骼模型 */
		static SharedPtr<SkeletonModel> Create(const std::string& path);
	};
}
