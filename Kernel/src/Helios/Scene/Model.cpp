#include "Pch.h"
#include "Model.h"
#include "SkeletonModel.h"
#include "ModelManifest.h"
#include "Mesh.h"
#include "SceneCommon.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Common/Math.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	/* 内建模型的身份名：单一来源 —— 创建时命名（GetPath）、序列化存取、
	 * UI 判定都走这里，改名字只改这一个地方。 */
	const char* BuiltinModelName(BuiltinModelType type)
	{
		switch (type)
		{
		case BuiltinModelType::Cube:   return "BuiltinCube";
		case BuiltinModelType::Sphere: return "BuiltinSphere";
		case BuiltinModelType::Plane:  return "BuiltinPlane";
		}
		return "";
	}

	bool TryParseBuiltinModelName(const std::string& name, BuiltinModelType& out_type)
	{
		for (BuiltinModelType type : { BuiltinModelType::Cube, BuiltinModelType::Sphere, BuiltinModelType::Plane })
		{
			if (name == BuiltinModelName(type))
			{
				out_type = type;
				return true;
			}
		}
		return false;
	}

	/* ==================== 内置几何的顶点契约 ====================
	 * 跟导入的 .mesh 一致（元素声明顺序即 location 0..4）—— 这是 GBuffer / PBRStandard 能建出
	 * 管线的前提（Metal 要求用到的 attribute 都在顶点描述符里）。切线按 UV 的 U 方向推导
	 * （B = cross(T,N)），防法线贴图翻面。 */
	static void SetBuiltinVertexLayout(const SharedPtr<DeviceVertexBuffer>& vertex_buffer)
	{
		vertex_buffer->SetLayout({
			{ "a_Position", BufferDataType::Float3 },
			{ "a_Normal",   BufferDataType::Float3 },
			{ "a_Color",    BufferDataType::Float4 },
			{ "a_TexCoord", BufferDataType::Float3 },
			{ "a_Tangent",  BufferDataType::Float3 },
		});
	}

	/* 创建一个Cube类型的模型 */
	static SharedPtr<Model> CreateCube(const SharedPtr<Material>& material)
	{
		const char* model_name = BuiltinModelName(BuiltinModelType::Cube);
		auto model = CreateSharedPtr<Model>(model_name);

		/* 仅需准备一次顶点数据 */
		static SharedPtr<DeviceVertexArray> vertex_array = nullptr;
		if (!vertex_array)
		{
			/* Cube vertices：位置 f3 | 法线 f3 | 颜色 f4 | UV f3 | 切线 f3。
			 * 每个面按"从外侧看 CCW"排两个三角形（Cull_Back 才剔得对），
			 * 法线与切线都贴着各自面的实际朝向（切线 = 该面 UV 的 U 轴）。 */
			static constexpr float vertices[] = {
				/* Front (+Z)：法线 (0, 0, 1)，切线 = +X */
				-0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				 0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 0.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				 0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				-0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				 0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				-0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 1.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				/* Back (-Z)：法线 (0, 0, -1)，切线 = -X */
				 0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f, -1.0f,  0.0f,  0.0f,
				-0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 0.00f, 0.0f, -1.0f,  0.0f,  0.0f,
				-0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f, -1.0f,  0.0f,  0.0f,
				 0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f, -1.0f,  0.0f,  0.0f,
				-0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f, -1.0f,  0.0f,  0.0f,
				 0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 1.00f, 0.0f, -1.0f,  0.0f,  0.0f,
				/* Right (+X)：法线 (1, 0, 0)，切线 = -Z */
				 0.5f, -0.5f,  0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  0.0f,  0.0f, -1.0f,
				 0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 0.00f, 0.0f,  0.0f,  0.0f, -1.0f,
				 0.5f,  0.5f, -0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  0.0f,  0.0f, -1.0f,
				 0.5f, -0.5f,  0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  0.0f,  0.0f, -1.0f,
				 0.5f,  0.5f, -0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  0.0f,  0.0f, -1.0f,
				 0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 1.00f, 0.0f,  0.0f,  0.0f, -1.0f,
				/* Left (-X)：法线 (-1, 0, 0)，切线 = +Z */
				-0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  0.0f,  0.0f,  1.0f,
				-0.5f, -0.5f,  0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 0.00f, 0.0f,  0.0f,  0.0f,  1.0f,
				-0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  0.0f,  0.0f,  1.0f,
				-0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  0.0f,  0.0f,  1.0f,
				-0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  0.0f,  0.0f,  1.0f,
				-0.5f,  0.5f, -0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 1.00f, 0.0f,  0.0f,  0.0f,  1.0f,
				/* Top (+Y)：法线 (0, 1, 0)，切线 = +X */
				-0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				 0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 0.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				 0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				-0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				 0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				-0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 1.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				/* Bottom (-Y)：法线 (0, -1, 0)，切线 = +X */
				-0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				 0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 0.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				 0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				-0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				 0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  1.0f,  0.0f,  0.0f,
				-0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 1.00f, 0.0f,  1.0f,  0.0f,  0.0f,
			};

			/* Vertex Buffer */
			SharedPtr<DeviceVertexBuffer> vertex_buffer = DeviceVertexBuffer::Create(
				std::string(model_name) + "_VertexBuffer", vertices, sizeof(vertices));
			SetBuiltinVertexLayout(vertex_buffer);
			/* Vertex Array */
			vertex_array = DeviceVertexArray::Create(std::string(model_name) + "_VertexArray");
			vertex_array->Bind();
			vertex_array->AddVertexBuffer(vertex_buffer);
		}

		/* 内建模型的顶点范围是固定的（±0.5 的立方体），显式写入局部 AABB。
		 * 阴影视锥拟合、视锥体剔除等逻辑都依赖 MeshSegment 的 AABB，
		 * 缺省值 (0,0,0) 是退化包围盒，会让这些逻辑把物体当成一个点。 */
		auto mesh_segment = CreateSharedPtr<MeshSegment>(model_name, vertex_array, 0);
		mesh_segment->SetAABB(glm::vec3(-0.5f), glm::vec3(0.5f));
		model->AddMeshSegment(mesh_segment);
		model->AddMaterialSlot("Default", material);
		return model;
	}

	/* 创建一个Sphere类型的网格 */
	static SharedPtr<Model> CreateSphere(const SharedPtr<Material>& material)
	{
		const char* model_name = BuiltinModelName(BuiltinModelType::Sphere);
		auto model = CreateSharedPtr<Model>(model_name);

		/* 仅需准备一次顶点数据 */
		static SharedPtr<DeviceVertexArray> vertex_array = nullptr;
		if (!vertex_array)
		{
			constexpr int16_t segments_x = 32;
			constexpr int16_t segments_y = 32;

			/* 交错数据：位置 f3 | 法线 f3 | 颜色 f4 | UV f3 | 切线 f3（元素顺序即 location） */
			constexpr int32_t vertex_count = (segments_x + 1) * (segments_y + 1);
			constexpr int32_t floats_per_vertex = 16;

			std::vector<float> vertex_data;
			vertex_data.reserve(static_cast<size_t>(vertex_count) * floats_per_vertex);

			for (int32_t x = 0; x <= segments_x; ++x)
			{
				const float s_x = static_cast<float>(x) / segments_x;
				/* 切线 = dP/dU 的方向（经度方向，与 v 无关）——极点处也不退化，
				 * 不像"差分法"那样在 sin(vπ)=0 时归零到 NaN */
				const float tangent_u = -std::sin(s_x * 2.0f * PI);
				const float tangent_w = std::cos(s_x * 2.0f * PI);

				for (int32_t y = 0; y <= segments_y; ++y)
				{
					const float s_y = static_cast<float>(y) / segments_y;

					const glm::vec3 position(
						std::cos(s_x * 2.0f * PI) * std::sin(s_y * PI),
						std::cos(s_y * PI),
						std::sin(s_x * 2.0f * PI) * std::sin(s_y * PI)
					);

					/* 位置 / 法线（对球心在原点的单位球，normal 与 position 相等） */
					vertex_data.emplace_back(position.x);
					vertex_data.emplace_back(position.y);
					vertex_data.emplace_back(position.z);
					vertex_data.emplace_back(position.x);
					vertex_data.emplace_back(position.y);
					vertex_data.emplace_back(position.z);
					/* 颜色：白（乘进材质是 no-op） */
					vertex_data.emplace_back(1.0f);
					vertex_data.emplace_back(1.0f);
					vertex_data.emplace_back(1.0f);
					vertex_data.emplace_back(1.0f);
					/* UV（w 位补 0） */
					vertex_data.emplace_back(s_x);
					vertex_data.emplace_back(s_y);
					vertex_data.emplace_back(0.0f);
					/* 切线 */
					vertex_data.emplace_back(tangent_u);
					vertex_data.emplace_back(0.0f);
					vertex_data.emplace_back(tangent_w);
				}
			}

			/* Calculate indices：显式三角形列表（每格两个三角形，绕序为"从外侧看 CCW"，
			 * 与 Cube / Plane 同一约定）。不用 Triangle_Strip：条带的奇偶绕序在
			 * 后端之间语义微妙，曾导致一半三角形被背面剔除、球面镂空。 */
			std::vector<uint16_t> indices;
			indices.reserve(static_cast<size_t>(segments_x) * segments_y * 6);
			for (int16_t y = 0; y < segments_y; ++y)
			{
				for (int16_t x = 0; x < segments_x; ++x)
				{
					const uint16_t top_left = static_cast<uint16_t>(y * (segments_x + 1) + x);
					const uint16_t bottom_left = static_cast<uint16_t>((y + 1) * (segments_x + 1) + x);
					const uint16_t bottom_right = static_cast<uint16_t>((y + 1) * (segments_x + 1) + x + 1);
					const uint16_t top_right = static_cast<uint16_t>(y * (segments_x + 1) + x + 1);

					indices.emplace_back(top_left);
					indices.emplace_back(bottom_left);
					indices.emplace_back(bottom_right);

					indices.emplace_back(top_left);
					indices.emplace_back(bottom_right);
					indices.emplace_back(top_right);
				}
			}

			vertex_array = DeviceVertexArray::Create(std::string(model_name) + "_VertexArray");
			vertex_array->Bind();
			/* vertices */
			{
				auto vertex_buffer = DeviceVertexBuffer::Create(std::string(model_name) + "_VertexBuffer",
					vertex_data.data(), static_cast<uint32_t>(vertex_data.size() * sizeof(float)));
				SetBuiltinVertexLayout(vertex_buffer);
				vertex_array->AddVertexBuffer(vertex_buffer);
			}
			/* indices */
			{
				auto index_buffer = IndexBuffer::Create(std::string(model_name) + "_IndexBuffer", indices.data(), static_cast<uint32_t>(indices.size()), IndexType::UInt16);
				vertex_array->SetIndexBuffer(index_buffer);
			}
		}

		/* 内建球为半径 1 的单位球（球心在局部原点） */
		auto mesh_segment = MeshSegment::Create(model_name, { vertex_array, PrimitiveType::Triangles }, 0);
		mesh_segment->SetAABB(glm::vec3(-1.0f), glm::vec3(1.0f));
		model->AddMeshSegment(mesh_segment);
		model->AddMaterialSlot("Default", material);
		return model;
	}

	/* 创建一个Plane类型的网格 */
	static SharedPtr<Model> CreatePlane(const SharedPtr<Material>& material)
	{
		const char* model_name = BuiltinModelName(BuiltinModelType::Plane);
		auto model = CreateSharedPtr<Model>(model_name);
		/* 仅需准备一次顶点数据 */
		static SharedPtr<DeviceVertexArray> vertex_array = nullptr;
		if (!vertex_array)
		{
			/* Plane Vertices：位置 f3 | 法线 f3 | 颜色 f4 | UV f3 | 切线 f3，
			 * 切线 = UV 的 U 轴（+X），与法线、UV 朝向自洽 */
			static constexpr float vertices[] = {
				-0.5f, 0.0f, -0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  1.0f, 0.0f, 0.0f,
				-0.5f, 0.0f,  0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 1.00f, 0.0f,  1.0f, 0.0f, 0.0f,
				 0.5f, 0.0f,  0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  1.0f, 0.0f, 0.0f,
				-0.5f, 0.0f, -0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.00f, 0.00f, 0.0f,  1.0f, 0.0f, 0.0f,
				 0.5f, 0.0f,  0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 1.00f, 0.0f,  1.0f, 0.0f, 0.0f,
				 0.5f, 0.0f, -0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.00f, 0.00f, 0.0f,  1.0f, 0.0f, 0.0f,
			};

			/* Vertex Buffer */
			auto vertex_buffer = DeviceVertexBuffer::Create(std::string(model_name) + "_VertexBuffer",
				vertices, sizeof(vertices));
			SetBuiltinVertexLayout(vertex_buffer);

			/* Vertex Array */
			vertex_array = DeviceVertexArray::Create(std::string(model_name) + "_VertexArray");
			vertex_array->Bind();
			vertex_array->AddVertexBuffer(vertex_buffer);
		}

		/* 内建平面位于局部空间的 XZ 平面，范围 ±0.5（零厚度） */
		auto mesh_segment = CreateSharedPtr<MeshSegment>(model_name, vertex_array, 0);
		mesh_segment->SetAABB(glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.0f, 0.5f));
		model->AddMeshSegment(mesh_segment);
		model->AddMaterialSlot("Default", material);
		return model;
	}

	Model::Model(std::string path)
		: m_Path(std::move(path))
		, m_AABBMin(glm::vec3(std::numeric_limits<float>::max()))
		, m_AABBMax(glm::vec3(-std::numeric_limits<float>::max()))
	{
	}

	/* 由世界变换矩阵写入位置、旋转与缩放 */
	void Model::SetTransform(const glm::mat4& transform)
	{
		/* 位置与旋转由基类统一处理 */
		SceneObject::SetTransform(transform);

		/* 缩放是模型特有属性，额外分解写入 */
		const glm::mat3 rotation = glm::mat3(transform);
		m_Scale = glm::vec3(
			glm::length(glm::vec3(rotation[0])),
			glm::length(glm::vec3(rotation[1])),
			glm::length(glm::vec3(rotation[2])));
	}

	/* 添加一个MeshSegment到模型 */
	void Model::AddMeshSegment(const SharedPtr<MeshSegment>& mesh_segment)
	{
		m_MeshSegments.emplace_back(mesh_segment);

		/* 更新AABB */
		m_AABBMin.x = std::min(m_AABBMin.x, mesh_segment->m_AABBMin.x);
		m_AABBMin.y = std::min(m_AABBMin.y, mesh_segment->m_AABBMin.y);
		m_AABBMin.z = std::min(m_AABBMin.z, mesh_segment->m_AABBMin.z);
		m_AABBMax.x = std::max(m_AABBMax.x, mesh_segment->m_AABBMax.x);
		m_AABBMax.y = std::max(m_AABBMax.y, mesh_segment->m_AABBMax.y);
		m_AABBMax.z = std::max(m_AABBMax.z, mesh_segment->m_AABBMax.z);
	}

	namespace
	{
		/* 顺着 .mesh 的结构走一遍，看末尾有没有骨骼段（只读文件、不建 GPU 对象）。
		 * 骨骼模型和静态模型共用 Model::Create 这一个入口，靠它决定造哪个子类。 */
		bool ReadLengthPrefixedString(std::istream& stream, std::string& out_text)
		{
			size_t length = 0;
			stream.read(reinterpret_cast<char*>(&length), sizeof(length));
			if (!stream || length > (1u << 20))
				return false;

			out_text.resize(length);
			stream.read(out_text.data(), static_cast<std::streamsize>(length));
			return static_cast<bool>(stream);
		}

		bool MeshFileHasSkeleton(const std::string& mesh_path)
		{
			std::ifstream stream(PathFromUtf8(mesh_path), std::ios::in | std::ios::binary);
			if (!stream)
				return false;

			size_t segment_count = 0;
			stream.read(reinterpret_cast<char*>(&segment_count), sizeof(segment_count));
			if (!stream || segment_count > (1u << 16))
				return false;

			for (size_t segment_index = 0; segment_index < segment_count; ++segment_index)
			{
				std::string name;
				if (!ReadLengthPrefixedString(stream, name))
					return false;

				uint32_t vertex_count = 0;
				stream.read(reinterpret_cast<char*>(&vertex_count), sizeof(vertex_count));
				size_t index_count = 0;
				stream.read(reinterpret_cast<char*>(&index_count), sizeof(index_count));
				if (!stream)
					return false;
				stream.seekg(static_cast<std::streamoff>(index_count * sizeof(uint32_t)), std::ios::cur);

				size_t buffer_count = 0;
				stream.read(reinterpret_cast<char*>(&buffer_count), sizeof(buffer_count));
				if (!stream)
					return false;

				for (size_t buffer_index = 0; buffer_index < buffer_count; ++buffer_index)
				{
					uint32_t stride = 0;
					stream.read(reinterpret_cast<char*>(&stride), sizeof(stride));
					stream.seekg(static_cast<std::streamoff>(vertex_count) * stride * sizeof(float), std::ios::cur);

					size_t element_count = 0;
					stream.read(reinterpret_cast<char*>(&element_count), sizeof(element_count));
					if (!stream)
						return false;

					for (size_t element_index = 0; element_index < element_count; ++element_index)
					{
						std::string element_name;
						if (!ReadLengthPrefixedString(stream, element_name))
							return false;

						uint8_t element_type = 0;
						size_t element_offset = 0;
						bool element_normalized = false;
						stream.read(reinterpret_cast<char*>(&element_type), sizeof(element_type));
						stream.read(reinterpret_cast<char*>(&element_offset), sizeof(element_offset));
						stream.read(reinterpret_cast<char*>(&element_normalized), sizeof(element_normalized));
						if (!stream)
							return false;
					}
				}

				int material_index = 0;
				glm::vec3 aabb_min, aabb_max;
				stream.read(reinterpret_cast<char*>(&material_index), sizeof(material_index));
				stream.read(reinterpret_cast<char*>(&aabb_min), sizeof(glm::vec3));
				stream.read(reinterpret_cast<char*>(&aabb_max), sizeof(glm::vec3));
				if (!stream)
					return false;
			}

			char magic[4]{};
			stream.read(magic, sizeof(magic));
			return stream && std::memcmp(magic, kSkeletonFileMagic, sizeof(magic)) == 0;
		}
	}

	bool Model::HasSkin() const
	{
		return m_SkinData != nullptr && !m_SkinData->IsEmpty();
	}

	namespace
	{
		/* 把条目的参数覆盖叠到已克隆的材质上：只覆盖写过的参数；
		 * RasterState 只有在覆盖里显式带了才替换 */
		void ApplyMaterialOverride(Material& target, const MaterialSlot& entry)
		{
			if (entry.pOverride == nullptr)
				return;

			for (const auto& [id, param] : entry.pOverride->GetAllParameters())
			{
				if (param.Type == ParamType::Texture)
				{
					const auto& texture = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(param.Value);
					target.SetTexture(param.Name, texture.first);
				}
				else
				{
					target.SetParameters(param.Type, param.Name, param.Value);
				}
			}

			if (entry.HasRasterOverride)
				target.SetRasterState(entry.pOverride->GetRasterState());
		}

		/* 清单条目 -> 模型槽：引用条目在这里解析成默认材质（覆盖叠在克隆体上），
		 * 内嵌条目直接用文件里的定义 */
		MaterialSlot ResolveManifestSlot(const ModelManifest::Slot& entry)
		{
			MaterialSlot slot;
			slot.Name = entry.Name;
			slot.SourcePath = entry.AssetPath;
			slot.pOverride = entry.pOverride;
			slot.HasRasterOverride = entry.HasRasterOverride;

			if (entry.AssetPath.empty())
			{
				slot.pDefault = entry.pEmbedded;
				return slot;
			}

			const SharedPtr<Material> base = AssetManager::Instance().GetOrLoad<Material>(entry.AssetPath);
			if (base == nullptr)
				return slot;

			if (entry.pOverride == nullptr)
			{
				slot.pDefault = base;
				return slot;
			}

			auto resolved = base->Clone();
			ApplyMaterialOverride(*resolved, slot);
			slot.pDefault = std::move(resolved);
			return slot;
		}
	}

	/* 从路径加载一个模型 */
	SharedPtr<Model> Model::Create(const std::string& path)
	{
		/* 内建模型的路径即身份名（BuiltinCube…）：直接路由到程序化创建，不落盘、不查资产。
		 * 序列化加载与代码创建共用这一个入口 —— 存盘写身份名、读盘反查类型。 */
		BuiltinModelType builtin_type{};
		if (TryParseBuiltinModelName(path, builtin_type))
			return Create(builtin_type);

		/* 文件模型统一归一为绝对路径（相对形态 = 场景文件里的存储形态）：
		 * 对象内部路径恒为绝对，保存 / 预览换算才不依赖进程工作目录。 */
		const std::string model_path = ABSOLUTE_PATH(path);

		/* 获取文件后缀名 */
		std::string suffix = ExtractFileSuffix(model_path);
		std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);

		/* .model 清单：Geometry 指向几何 payload，<Material> 是槽表 */
		if (suffix == "model")
		{
			std::string geometry_path;
			std::vector<ModelManifest::Slot> entries;
			if (!ModelManifest::Read(model_path, geometry_path, entries))
				return nullptr;

			SharedPtr<Model> model = Create(geometry_path);
			if (model == nullptr)
			{
				CORE_LOG_ERROR("Failed to load geometry '{}' for model '{}'.", geometry_path, model_path);
				return nullptr;
			}

			model->m_Path = model_path;
			model->m_GeometryPath = geometry_path;

			/* 几何加载时按段补过一遍占位槽，这里换成清单里的槽表 */
			model->m_MaterialSlots.clear();
			model->m_MaterialSlots.reserve(entries.size());
			for (const ModelManifest::Slot& entry : entries)
				model->m_MaterialSlots.push_back(ResolveManifestSlot(entry));

			model->EnsureMaterialSlots();
			return model;
		}

		/* 加载Mesh模型数据（纯几何；没有清单就是白模） */
		if (suffix == "mesh")
		{
			/* 有骨骼段的 .mesh 交给 SkeletonModel（同一个入口、同一个模型装载流程） */
			SharedPtr<Model> model = MeshFileHasSkeleton(model_path)
				? std::static_pointer_cast<Model>(CreateSharedPtr<SkeletonModel>(model_path))
				: CreateSharedPtr<Model>(model_path);

			model->m_GeometryPath = model_path;

			/* 直接从mesh文件加载 */
			const std::filesystem::path mesh_path = PathFromUtf8(model_path);
			std::ifstream in_mesh_file(mesh_path, std::ios::in | std::ios::binary);
			if (!in_mesh_file)
			{
				CORE_LOG_ERROR("Failed to load file: {}.", model_path);
				return nullptr;
			}

			/* 读取子模型数量 */
			size_t mesh_segment_cnt;
			in_mesh_file.read((char*)&mesh_segment_cnt, sizeof(size_t));

			/* 蒙皮模型要留着 CPU 侧的绑定顶点（构建动态网格、CPU 蒙皮都用它）。
			 * 先收在临时表里，读到末尾的蒙皮段再决定要不要落到段上。 */
			std::vector<std::vector<float>> segment_sources;

			for (size_t mesh_segment_idx = 0; mesh_segment_idx < mesh_segment_cnt; ++mesh_segment_idx)
			{
				/* 子模型Name（先按长度开好缓冲再读：名字超过短串容量时直写 data() 会越界） */
				size_t name_size;
				in_mesh_file.read((char*)&name_size, sizeof(size_t));
				std::string name(name_size, '\0');
				in_mesh_file.read(name.data(), name_size);

				/* Vertex Array */
				auto vertex_array = DeviceVertexArray::Create(name + "_VertexArray");
				vertex_array->Bind();

				/* 顶点数量 */
				uint32_t vertex_count;
				in_mesh_file.read((char*)&vertex_count, sizeof(uint32_t));

				/* 子模型索引数据 */
				size_t index_count;
				in_mesh_file.read((char*)&index_count, sizeof(size_t));
				uint32_t* indices = new uint32_t[index_count];
				in_mesh_file.read((char*)indices, index_count * sizeof(uint32_t));

				/* Index Buffer */
				auto index_type = IndexType::UInt32; // index_count > UINT16_MAX ? IndexType::UInt32 : IndexType::UInt16;
				auto index_buffer = IndexBuffer::Create(name + "_IndexBuffer", indices, index_count, index_type);
				vertex_array->SetIndexBuffer(index_buffer);
				delete[] indices;

				/* VertexBuffer的数量 */
				size_t vertex_buffer_count;
				in_mesh_file.read((char*)&vertex_buffer_count, sizeof(size_t));

				/* VertexBuffer数据 */
				for (size_t i = 0; i < vertex_buffer_count; ++i)
				{
					uint32_t stride;
					in_mesh_file.read((char*)&stride, sizeof(uint32_t));

					float* buffer_data = new float[vertex_count * stride];
					in_mesh_file.read((char*)buffer_data, vertex_count * stride * sizeof(float));

					/* VertexLayout */
					VertexBufferLayout vertex_buffer_layout;
					size_t element_cnt;
					in_mesh_file.read((char*)&element_cnt, sizeof(size_t));
					for (size_t element_idx = 0; element_idx < element_cnt; ++element_idx)
					{
						/* Name */
						size_t element_name_size;
						in_mesh_file.read((char*)&element_name_size, sizeof(size_t));
						std::string element_name(element_name_size, '\0');
						in_mesh_file.read(element_name.data(), element_name_size);

						/* Type */
						uint8_t element_type;
						in_mesh_file.read((char*)&element_type, sizeof(uint8_t));

						/* Offset */
						size_t element_offset;
						in_mesh_file.read((char*)&element_offset, sizeof(size_t));

						/* Normalized */
						bool element_normalized;
						in_mesh_file.read((char*)&element_normalized, sizeof(bool));

						vertex_buffer_layout.EmplaceElement({ element_name, static_cast<BufferDataType>(element_type), element_normalized });
					}

					/* 第一个顶点缓冲的 CPU 副本（蒙皮模型还会用到） */
					if (i == 0)
						segment_sources.emplace_back(buffer_data, buffer_data + vertex_count * stride);

					/* Vertex Buffer */
					auto vertex_buffer = DeviceVertexBuffer::Create(name + "_VertexBuffer", buffer_data, vertex_count * stride * sizeof(float));
					vertex_buffer->SetLayout(vertex_buffer_layout);
					vertex_array->AddVertexBuffer(vertex_buffer);
					delete[] buffer_data;
				}

				/* 材质槽索引：段只记索引、不解析材质（解析链收口在 ResolveSlotMaterial） */
				int material_idx;
				in_mesh_file.read((char*)&material_idx, sizeof(int));

				/* AABB */
				glm::vec3 aabb_min, aabb_max;
				in_mesh_file.read((char*)&aabb_min, sizeof(glm::vec3));
				in_mesh_file.read((char*)&aabb_max, sizeof(glm::vec3));

				/* 创建MeshSegment */
				auto mesh_segment = CreateSharedPtr<MeshSegment>(name, vertex_array, material_idx);
				mesh_segment->SetAABB(aabb_min, aabb_max);

				model->AddMeshSegment(mesh_segment);
			}

			/* .mesh 末尾可选的蒙皮段：老文件没有这一段，读不到就按静态模型处理（先读再关文件） */
			model->m_SkinData = CreateSharedPtr<SkeletonData>();
			if (!model->m_SkinData->Read(in_mesh_file))
				model->m_SkinData.reset();

			/* 带蒙皮：把绑定顶点挂到各段上（顺序与段一致） */
			if (model->HasSkin())
			{
				const auto& segments = model->GetMeshSegments();
				for (size_t index = 0; index < segments.size() && index < segment_sources.size(); ++index)
					segments[index]->SetSourceVertices(std::move(segment_sources[index]));
			}

			/* 关闭文件 */
			in_mesh_file.close();

			/* 槽表兜底：导入时没有材质的模型（.mtl 缺失 / 条目比段少）也要有槽名，
			 * 否则属性面板和模型编辑器挂的材质挂不上去 */
			model->EnsureMaterialSlots();

			return model;
		}
		else
		{
			/* todo: support other model formats */
			CORE_LOG_ERROR("Unsupported model format: {}.", path);
			ASSERT(false);
			return nullptr;
		}
	}

	/* 创建内建模型 */
	SharedPtr<Model> Model::Create(BuiltinModelType type, const SharedPtr<Material>& material)
	{
		/* 材质为空 = 内置白模（新建 / 拖入的初始外观） */
		auto slot_material = (material != nullptr) ? material : Material::BuiltinWhite();

		switch (type)
		{
		case BuiltinModelType::Cube:   return CreateCube(slot_material);
		case BuiltinModelType::Sphere: return CreateSphere(slot_material);
		case BuiltinModelType::Plane:  return CreatePlane(slot_material);
		}
		return nullptr;
	}

	/* 添加一个材质槽 */
	void Model::AddMaterialSlot(const std::string& name, const SharedPtr<Material>& material)
	{
		MaterialSlot slot;
		slot.Name = name;
		slot.pDefault = material;
		m_MaterialSlots.emplace_back(std::move(slot));
	}

	const MaterialSlot* Model::GetSlotByName(const std::string& name) const
	{
		for (const MaterialSlot& slot : m_MaterialSlots)
		{
			if (slot.Name == name)
				return &slot;
		}
		return nullptr;
	}

	/* 槽表兜底：无槽的段分配新槽、索引越界的段把空缺补齐（默认槽没材质，解析时兜底白模） */
	void Model::EnsureMaterialSlots()
	{
		for (const auto& segment : m_MeshSegments)
		{
			if (segment == nullptr)
				continue;

			if (segment->GetSlotIndex() < 0)
			{
				const int new_index = static_cast<int>(m_MaterialSlots.size());
				AddMaterialSlot("slot_" + std::to_string(new_index), nullptr);
				segment->SetSlotIndex(new_index);
				continue;
			}

			for (int i = static_cast<int>(m_MaterialSlots.size()); i <= segment->GetSlotIndex(); ++i)
				AddMaterialSlot("slot_" + std::to_string(i), nullptr);
		}
	}

	/* 按索引取槽 */
	const MaterialSlot* Model::GetSlotByIndex(int idx) const
	{
		if (idx < 0 || idx >= static_cast<int>(m_MaterialSlots.size()))
			return nullptr;

		return &m_MaterialSlots[idx];
	}

	const MaterialSlot* Model::GetSlotForSegment(size_t segment_index) const
	{
		if (segment_index >= m_MeshSegments.size() || m_MeshSegments[segment_index] == nullptr)
			return nullptr;

		return GetSlotByIndex(m_MeshSegments[segment_index]->GetSlotIndex());
	}

	/* 内建模型类型：路径即身份名（见 TryParseBuiltinModelName） */
	bool Model::TryGetBuiltinType(BuiltinModelType& out_type) const
	{
		return TryParseBuiltinModelName(m_Path, out_type);
	}

}
