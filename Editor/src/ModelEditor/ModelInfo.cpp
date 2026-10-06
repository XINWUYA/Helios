#include "Pch.h"
#include "ModelInfo.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Scene/SceneCommon.h"

namespace Helios
{
	namespace
	{
		class Utf8MaterialReader final : public tinyobj::MaterialReader
		{
		public:
			explicit Utf8MaterialReader(std::filesystem::path base_directory)
				: m_BaseDirectory(std::move(base_directory))
			{
			}

			bool operator()(const std::string& material_name,
				std::vector<tinyobj::material_t>* materials,
				std::map<std::string, int>* material_map,
				std::string* warning,
				std::string* error) override
			{
				std::filesystem::path relative_path;
				if (!TryPathFromUtf8(material_name, relative_path))
				{
					if (warning != nullptr)
						*warning += "Material path is not valid UTF-8: " + material_name + "\\n";
					return false;
				}

				const std::filesystem::path material_path = relative_path.is_absolute()
					? relative_path : m_BaseDirectory / relative_path;
				std::ifstream material_stream(material_path, std::ios::in | std::ios::binary);
				if (!material_stream)
				{
					if (warning != nullptr)
						*warning += "Material file not found: " + PathToUtf8(material_path) + "\\n";
					return false;
				}

				tinyobj::LoadMtl(material_map, materials, &material_stream, warning, error);
				return true;
			}

		private:
			std::filesystem::path m_BaseDirectory;
		};
	}

	/* 加载Obj模型信息 */
	void ModelInfo::LoadFromObj(const std::string& filepath)
	{
		if (filepath.empty())
		{
			EDITOR_LOG_ERROR("Model filepath is empty.");
			return;
		}

		std::filesystem::path model_path;
		if (!TryPathFromUtf8(filepath, model_path))
		{
			EDITOR_LOG_ERROR("Model path is not valid UTF-8.");
			return;
		}

		m_Path = PathToUtf8(model_path);
		std::ifstream model_stream(model_path, std::ios::in | std::ios::binary);
		if (!model_stream)
		{
			EDITOR_LOG_ERROR("Failed to open model file: {}.", m_Path);
			return;
		}

		Utf8MaterialReader material_reader(model_path.parent_path());

		/* 加载模型数据 */
		std::string warn, err;
		bool ret = tinyobj::LoadObj(&m_Attributes, &m_Shapes, &m_Materials, &warn, &err,
			&model_stream, &material_reader);

		if (!warn.empty())
			EDITOR_LOG_WARN("Load Obj Warning: {}.", warn);

		if (!err.empty())
			EDITOR_LOG_ERROR("Load Obj Error: {}.", err);
		if (!ret)
			return;

#if HELIOS_DEBUG
		/* 打印模型信息 */
		EDITOR_LOG_DEBUG("Loading Obj: {}", filepath);
		EDITOR_LOG_DEBUG("Vertices  : {}", m_Attributes.vertices.size() / 3);
		EDITOR_LOG_DEBUG("Normals   : {}", m_Attributes.normals.size() / 3);
		EDITOR_LOG_DEBUG("Texcoords : {}", m_Attributes.texcoords.size() / 2);
		EDITOR_LOG_DEBUG("Shapes    : {}", m_Shapes.size());
		EDITOR_LOG_DEBUG("Materials : {}", m_Materials.size());
#endif

		/* 每个Shape对应一个MeshSegment */
		for (size_t shape_idx = 0; shape_idx < m_Shapes.size(); ++shape_idx)
		{
			const auto& shape_data = m_Shapes[shape_idx];

			SharedPtr<SubModelInfo> sub_model_info = CreateSharedPtr<SubModelInfo>();
			sub_model_info->Name = shape_data.name;

			/* 记录AABB信息 */
			auto aabb_min = glm::vec3(std::numeric_limits<float>::max());
			auto aabb_max = glm::vec3(-std::numeric_limits<float>::max());

			const auto face_count = shape_data.mesh.indices.size() / 3;
			for (size_t face_idx = 0; face_idx < face_count; ++face_idx)
			{
				/* 三角形面片的3个顶点索引 */
				auto index0 = shape_data.mesh.indices[face_idx * 3 + 0];
				auto index1 = shape_data.mesh.indices[face_idx * 3 + 1];
				auto index2 = shape_data.mesh.indices[face_idx * 3 + 2];

				/* 收集3个顶点坐标 */
				ASSERT(index0.vertex_index >= 0 && index1.vertex_index >= 0 && index2.vertex_index >= 0);
				float vertex[3][3];
				for (int i = 0; i < 3; ++i)
				{
					/* x->y->z值依次获取 */
					vertex[0][i] = m_Attributes.vertices[index0.vertex_index * 3 + i];
					vertex[1][i] = m_Attributes.vertices[index1.vertex_index * 3 + i];
					vertex[2][i] = m_Attributes.vertices[index2.vertex_index * 3 + i];

					/* 记录AABB */
					aabb_min[i] = std::min(vertex[0][i], aabb_min[i]);
					aabb_min[i] = std::min(vertex[1][i], aabb_min[i]);
					aabb_min[i] = std::min(vertex[2][i], aabb_min[i]);
					aabb_max[i] = std::max(vertex[0][i], aabb_max[i]);
					aabb_max[i] = std::max(vertex[1][i], aabb_max[i]);
					aabb_max[i] = std::max(vertex[2][i], aabb_max[i]);
				}

				/* 收集3个顶点法线 */
				float normal[3][3];
				bool invalid_normal = false;
				if (!m_Attributes.normals.empty())
				{
					if (index0.normal_index < 0 || index1.normal_index < 0 || index2.normal_index < 0)
					{
						invalid_normal = true;
					}
					else
					{
						for (int i = 0; i < 3; ++i)
						{
							/* x->y->z值依次获取 */
							normal[0][i] = m_Attributes.normals[index0.normal_index * 3 + i];
							normal[1][i] = m_Attributes.normals[index1.normal_index * 3 + i];
							normal[2][i] = m_Attributes.normals[index2.normal_index * 3 + i];
						}
					}
				}

				if (invalid_normal)
				{
					/* todo: 生成法线 */
				}

				/* 收集3个顶点颜色 */
				float color[3][3];
				memset(color, 1.0f, 9 * sizeof(float)); /* 默认为白色 */
				if (!m_Attributes.colors.empty())
				{
					for (int i = 0; i < 3; ++i)
					{
						/* x->y->z值依次获取 */
						color[0][i] = m_Attributes.colors[index0.vertex_index * 3 + i];
						color[1][i] = m_Attributes.colors[index1.vertex_index * 3 + i];
						color[2][i] = m_Attributes.colors[index2.vertex_index * 3 + i];
					}
				}

				/* 收集3个顶点uv */
				float uv[3][2];
				memset(uv, 0.0f, 6 * sizeof(float));
				if (!m_Attributes.texcoords.empty())
				{
					if (index0.texcoord_index >= 0 && index1.texcoord_index >= 0 && index2.texcoord_index >= 0)
					{
						uv[0][0] = m_Attributes.texcoords[index0.texcoord_index * 2];
						uv[0][1] = 1.0f - m_Attributes.texcoords[index0.texcoord_index * 2 + 1];
						uv[1][0] = m_Attributes.texcoords[index1.texcoord_index * 2];
						uv[1][1] = 1.0f - m_Attributes.texcoords[index1.texcoord_index * 2 + 1];
						uv[2][0] = m_Attributes.texcoords[index2.texcoord_index * 2];
						uv[2][1] = 1.0f - m_Attributes.texcoords[index2.texcoord_index * 2 + 1];
					}
				}

				/* 计算切线 */
				float tangent[3][4];
				memset(tangent, 0.0f, 12 * sizeof(float));
				if (!m_Attributes.texcoords.empty()) /* 确保存在uv数据 */
				{
					// Ref: https://learnopengl-cn.github.io/05%20Advanced%20Lighting/04%20Normal%20Mapping/#_3
					glm::vec3 v0 = glm::vec3(vertex[0][0], vertex[0][1], vertex[0][2]);
					glm::vec3 v1 = glm::vec3(vertex[1][0], vertex[1][1], vertex[1][2]);
					glm::vec3 v2 = glm::vec3(vertex[2][0], vertex[2][1], vertex[2][2]);

					glm::vec2 w0 = glm::vec2(uv[0][0], uv[0][1]);
					glm::vec2 w1 = glm::vec2(uv[1][0], uv[1][1]);
					glm::vec2 w2 = glm::vec2(uv[2][0], uv[2][1]);

					glm::vec3 edge01 = v1 - v0;
					glm::vec3 edge02 = v2 - v0;
					glm::vec2 uv01 = w1 - w0;
					glm::vec2 uv02 = w2 - w0;

					float f = 1.0f / (uv01.x * uv02.y - uv01.y * uv02.x);
					tangent[0][0] = f * (uv02.y * edge01.x - uv01.y * edge02.x);
					tangent[0][1] = f * (uv02.y * edge01.y - uv01.y * edge02.y);
					tangent[0][2] = f * (uv02.y * edge01.z - uv01.y * edge02.z);
				}

				/* Combine */
				for (int v = 0; v < 3; ++v)
				{
					sub_model_info->VertexData.emplace_back(vertex[v][0]);
					sub_model_info->VertexData.emplace_back(vertex[v][1]);
					sub_model_info->VertexData.emplace_back(vertex[v][2]);
					sub_model_info->VertexData.emplace_back(normal[v][0]);
					sub_model_info->VertexData.emplace_back(normal[v][1]);
					sub_model_info->VertexData.emplace_back(normal[v][2]);
					sub_model_info->VertexData.emplace_back(color[v][0]);
					sub_model_info->VertexData.emplace_back(color[v][1]);
					sub_model_info->VertexData.emplace_back(color[v][2]);
					sub_model_info->VertexData.emplace_back(uv[v][0]);
					sub_model_info->VertexData.emplace_back(uv[v][1]);
				}
			}

			/* Vertex Buffer */
			auto vertex_buffer = DeviceVertexBuffer::Create(shape_data.name + "_VertexBuffer", sub_model_info->VertexData.data(), sub_model_info->VertexData.size() * sizeof(float));
			VertexBufferLayout vertex_buffer_layout = {
				{ "a_Position", BufferDataType::Float3 },
				{ "a_Normal", BufferDataType::Float3 },
				{ "a_Color", BufferDataType::Float3 },
				{ "a_TexCoord", BufferDataType::Float2 },
				// { "a_Tangent", BufferDataType::Float3 },
			};
			vertex_buffer->SetLayout(vertex_buffer_layout);

			/* Vertex Array */
			sub_model_info->VertexArray = DeviceVertexArray::Create(shape_data.name + "_VertexArray");
			sub_model_info->VertexArray->Bind();
			sub_model_info->VertexArray->AddVertexBuffer(vertex_buffer);

			/* MaterialParams */
			const std::filesystem::path relative_dir = std::filesystem::relative(model_path.parent_path(), g_AssetsPath);
			auto& params = sub_model_info->MaterialParams;
			const auto relative_texture_path = [&relative_dir](const std::string& texture_name)
			{
				return texture_name.empty()
					? std::string()
					: PathToUtf8(relative_dir / PathFromUtf8(texture_name));
			};
			int shape_material_id = shape_data.mesh.material_ids[0];
			if (shape_material_id >= 0 && shape_material_id < m_Materials.size())
			{
				const auto& material_data = m_Materials[shape_material_id];
				/* Name */
				params.Name = material_data.name;
				/* Ambient */
				params.AmbientTexPath = relative_texture_path(material_data.ambient_texname);
				params.Ambient = glm::vec3(material_data.ambient[0], material_data.ambient[1], material_data.ambient[2]);
				/* Diffuse/Albedo */
				params.DiffuseTexPath = relative_texture_path(material_data.diffuse_texname);
				params.Diffuse = glm::vec3(material_data.diffuse[0], material_data.diffuse[1], material_data.diffuse[2]);
				/* Specular */
				params.SpecularTexPath = relative_texture_path(material_data.specular_texname);
				params.Specular = glm::vec3(material_data.specular[0], material_data.specular[1], material_data.specular[2]);
				/* NormalTex */
				params.BumpTexPath = relative_texture_path(material_data.bump_texname);
				params.DisplacementTexPath = relative_texture_path(material_data.displacement_texname);
				/* Roughness */
				params.RoughnessTexPath = relative_texture_path(material_data.roughness_texname);
				params.Roughness = material_data.roughness;
				/* Metallic */
				params.MetallicTexPath = relative_texture_path(material_data.metallic_texname);
				params.Metallic = material_data.metallic;
				/* Emission */
				params.EmissionTexPath = relative_texture_path(material_data.emissive_texname);
				params.Emission = glm::vec3(material_data.emission[0], material_data.emission[1], material_data.emission[2]);
				/* ClearCoat */
				params.ClearCoatRoughness = material_data.clearcoat_roughness;
				params.ClearCoatThickness = material_data.clearcoat_thickness;
				/* Others */
				params.Transmittance = glm::vec3(material_data.transmittance[0], material_data.transmittance[1], material_data.transmittance[2]);
				params.IOR = material_data.ior;
			}

			sub_model_info->AABB = std::make_pair(aabb_min, aabb_max);

			m_SubModelInfos.emplace_back(sub_model_info);

			/* 更新模型整体的AABB */
			m_AABB.first.x = std::min(m_AABB.first.x, aabb_min.x);
			m_AABB.first.y = std::min(m_AABB.first.y, aabb_min.y);
			m_AABB.first.z = std::min(m_AABB.first.z, aabb_min.z);
			m_AABB.second.x = std::max(m_AABB.second.x, aabb_max.x);
			m_AABB.second.y = std::max(m_AABB.second.y, aabb_max.y);
			m_AABB.second.z = std::max(m_AABB.second.z, aabb_max.z);
		}
	}

	/* 重置数据 */
	void ModelInfo::Reset()
	{
		m_Shapes.clear();
		m_Materials.clear();
		m_SubModelInfos.clear();
		m_Path.clear();
	}
}
