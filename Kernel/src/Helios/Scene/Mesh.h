#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
	class DeviceVertexArray;

	/* 图元 */
	struct HELIOS_API MeshPrimitive
	{
		PrimitiveType PrimitiveType{ PrimitiveType::Triangles };
		SharedPtr<DeviceVertexArray> VertexArray{ nullptr };

		MeshPrimitive(const SharedPtr<class DeviceVertexArray>& vertex_array, enum PrimitiveType type = PrimitiveType::Triangles)
			: PrimitiveType(type), VertexArray(vertex_array)
		{
		}
	};

	/* 网格数据类：隶属模型，一个模型可有多个 MeshSegment。每个 Segment 声明自己属于哪个
	 * "材质槽"（几何只描述材质需求）；具体绑哪份材质，由模型槽表的默认和实体槽的覆盖决定。 */
	class MeshSegment
	{
	public:
		MeshSegment(const std::string& name, const MeshPrimitive& mesh_primitive, int slot_index);
		~MeshSegment() = default;

		/* 名称 */
		[[nodiscard]] const std::string& GetDebugName() const { return m_DebugName; }

		/* 设置图元 */
		void SetVertexArray(const MeshPrimitive& mesh_primitive) { m_MeshPrimitive = mesh_primitive; }
		[[nodiscard]] const MeshPrimitive& GetMeshPrimitive() const { return m_MeshPrimitive; }
		/* 所属材质槽索引（指向 Model 槽表的下标；-1 = 无槽，解析时兜底白模） */
		void SetSlotIndex(int slot_index) { m_SlotIndex = slot_index; }
		[[nodiscard]] int GetSlotIndex() const { return m_SlotIndex; }
		/* 设置AABB */
		[[nodiscard]] const glm::vec3& GetAABBMin() const { return m_AABBMin; }
		[[nodiscard]] const glm::vec3& GetAABBMax() const { return m_AABBMax; }
		void SetAABB(const glm::vec3& min, const glm::vec3& max) { m_AABBMin = min; m_AABBMax = max; }

		/* 蒙皮用：CPU 侧的绑定顶点（只在带蒙皮数据的模型上保留，静态模型为空） */
		[[nodiscard]] const std::vector<float>& GetSourceVertices() const { return m_SourceVertices; }
		void SetSourceVertices(std::vector<float> vertices) { m_SourceVertices = std::move(vertices); }
		/* 顶点步长（float 个数，从顶点布局推） */
		[[nodiscard]] uint32_t GetVertexStrideFloats() const;

		/* 创建网格数据对象 */
		static SharedPtr<MeshSegment> Create(const std::string& name, const MeshPrimitive& mesh_primitive, int slot_index);

	private:
		/* 标记名 */
		std::string m_DebugName{ "Unnamed MeshSegment" };
		/* 图元类型及顶点 */
		MeshPrimitive m_MeshPrimitive;
		/* 材质槽索引 */
		int m_SlotIndex{ -1 };
		/* 蒙皮用的绑定顶点（CPU 镜像；没有蒙皮时为空） */
		std::vector<float> m_SourceVertices;
		/* AABB */
		glm::vec3 m_AABBMin{};
		glm::vec3 m_AABBMax{};

		friend class Model;
	};
}
