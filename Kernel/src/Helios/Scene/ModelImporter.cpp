#include "Pch.h"
#include "ModelImporter.h"
#include <tiny_obj_loader.h>
#include <tinyxml2.h>

#include "SceneCommon.h"
#include "Helios/Common/Utils.h"

namespace Helios
{
	/* 导入Obj模型，从模型路径生成对应的.mesh和.mtl文件 */
	void ImportObj(const std::string& filepath)
	{
		if (filepath.empty())
		{
			CORE_LOG_ERROR("Model filepath is empty.");
		}

		const size_t pos = filepath.find_last_of("/\\");
		const std::string basedir = pos != std::string::npos ? filepath.substr(0, pos + 1) : ""; /* 模型所在路径 */
		const std::string basename = filepath.substr(pos + 1); /* 模型名 */

		tinyobj::attrib_t in_attribs; /* 包含所有的vertex、normal、texcoords */
		std::vector<tinyobj::shape_t> in_shapes; /* 包含每个shape的数据、indices、faces、material_idx */
		std::vector<tinyobj::material_t> in_materials; /* textures */

		/* 加载模型数据 */
		std::string warn, err;
		bool ret = tinyobj::LoadObj(&in_attribs, &in_shapes, &in_materials, &warn, &err, filepath.c_str(), basedir.c_str());

		if (!warn.empty())
			CORE_LOG_WARN("Load Obj Warning: {}.", warn);

		if (!err.empty())
			CORE_LOG_ERROR("Load Obj Error: {}.", err);

#if HELIOS_DEBUG
		/* 打印模型信息 */
		CORE_LOG_DEBUG("Loading Obj: {}", filepath);
		CORE_LOG_DEBUG("Vertices  : {}", in_attribs.vertices.size() / 3);
		CORE_LOG_DEBUG("Normals   : {}", in_attribs.normals.size() / 3);
		CORE_LOG_DEBUG("Texcoords : {}", in_attribs.texcoords.size() / 2);
		CORE_LOG_DEBUG("Shapes    : {}", in_shapes.size());
		CORE_LOG_DEBUG("Materials : {}", in_materials.size());
#endif

		/* 1. 导出模型对象信息到.mesh文件 */
		{
			std::ofstream out_mesh_file(filepath + ".mesh", std::ios::out | std::ios::binary);
			size_t shape_count = in_shapes.size();
			out_mesh_file.write((char*)&(shape_count), sizeof(size_t)); /* shape数量 */

			for (size_t shape_idx = 0; shape_idx < in_shapes.size(); ++shape_idx)
			{
				/* 构建VertexData */
				std::vector<float> data;

				/* 记录AABB信息 */
				glm::vec3 aabb_min = glm::vec3(std::numeric_limits<float>::max());
				glm::vec3 aabb_max = glm::vec3(-std::numeric_limits<float>::max());

				const auto& shape_data = in_shapes[shape_idx];
				const uint32_t face_count = shape_data.mesh.indices.size() / 3;
				for (size_t face_idx = 0; face_idx < face_count; ++face_idx)
				{
					/* 遍历三角形片3个顶点索引 */
					auto index0 = shape_data.mesh.indices[face_idx * 3 + 0];
					auto index1 = shape_data.mesh.indices[face_idx * 3 + 1];
					auto index2 = shape_data.mesh.indices[face_idx * 3 + 2];

					/* �ռ�3���������� */
					ASSERT(index0.vertex_index >= 0 && index1.vertex_index >= 0 && index2.vertex_index >= 0);
					float vertex[3][3];
					for (int i = 0; i < 3; ++i)
					{
						/* x->y->z值，依次位取 */
						vertex[0][i] = in_attribs.vertices[index0.vertex_index * 3 + i];
						vertex[1][i] = in_attribs.vertices[index1.vertex_index * 3 + i];
						vertex[2][i] = in_attribs.vertices[index2.vertex_index * 3 + i];

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
					if (!in_attribs.normals.empty())
					{
						if (index0.normal_index < 0 || index1.normal_index < 0 || index2.normal_index < 0)
						{
							invalid_normal = true;
						}
						else
						{
							for (int i = 0; i < 3; ++i)
							{
								/* x->y->z值，依次位取 */
								normal[0][i] = in_attribs.normals[index0.normal_index * 3 + i];
								normal[1][i] = in_attribs.normals[index1.normal_index * 3 + i];
								normal[2][i] = in_attribs.normals[index2.normal_index * 3 + i];
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
					if (!in_attribs.colors.empty())
					{
						for (int i = 0; i < 3; ++i)
						{
							/* x->y->z值，依次位取 */
							color[0][i] = in_attribs.colors[index0.vertex_index * 3 + i];
							color[1][i] = in_attribs.colors[index1.vertex_index * 3 + i];
							color[2][i] = in_attribs.colors[index2.vertex_index * 3 + i];
						}
					}

					/* 收集3个顶点uv */
					float uv[3][2];
					memset(uv, 0.0f, 6 * sizeof(float));
					if (!in_attribs.texcoords.empty())
					{
						if (index0.texcoord_index >= 0 && index1.texcoord_index >= 0 && index2.texcoord_index >= 0)
						{
							uv[0][0] = in_attribs.texcoords[index0.texcoord_index * 2];
							uv[0][1] = 1.0f - in_attribs.texcoords[index0.texcoord_index * 2 + 1];
							uv[1][0] = in_attribs.texcoords[index1.texcoord_index * 2];
							uv[1][1] = 1.0f - in_attribs.texcoords[index1.texcoord_index * 2 + 1];
							uv[2][0] = in_attribs.texcoords[index2.texcoord_index * 2];
							uv[2][1] = 1.0f - in_attribs.texcoords[index2.texcoord_index * 2 + 1];
						}
					}

					/* Combine */
					for (int v = 0; v < 3; ++v)
					{
						data.emplace_back(vertex[v][0]);
						data.emplace_back(vertex[v][1]);
						data.emplace_back(vertex[v][2]);
						data.emplace_back(normal[v][0]);
						data.emplace_back(normal[v][1]);
						data.emplace_back(normal[v][2]);
						data.emplace_back(color[v][0]);
						data.emplace_back(color[v][1]);
						data.emplace_back(color[v][2]);
						data.emplace_back(uv[v][0]);
						data.emplace_back(uv[v][1]);
					}
				}
				size_t name_size = shape_data.name.size();
				out_mesh_file.write((char*)&name_size, sizeof(size_t));
				out_mesh_file.write(shape_data.name.c_str(), name_size);

				size_t vertex_data_size = data.size();
				out_mesh_file.write((char*)&vertex_data_size, sizeof(size_t));
				out_mesh_file.write((char*)(data.data()), sizeof(float) * data.size());

				int shape_material_id = shape_data.mesh.material_ids[0];
				if (shape_material_id < 0 || shape_material_id >= in_materials.size())
					shape_material_id = in_materials.size() - 1; /* 使用默认材质 */
				out_mesh_file.write((char*)&shape_material_id, sizeof(int));

				out_mesh_file.write((char*)&aabb_min, sizeof(glm::vec3));
				out_mesh_file.write((char*)&aabb_max, sizeof(glm::vec3));
			}

			/* 关闭Mesh文件 */
			out_mesh_file.close();		}

		/* 2. 导出材质到.mtl文件 */
		{
			auto* out_mtl_file = new tinyxml2::XMLDocument();
			out_mtl_file->InsertEndChild(out_mtl_file->NewDeclaration());
		tinyxml2::XMLElement* mtl_root = out_mtl_file->NewElement("Materials");
		mtl_root->SetAttribute("Count", static_cast<unsigned int>(in_materials.size()));
		out_mtl_file->InsertEndChild(mtl_root);

			for (size_t material_idx = 0; material_idx < in_materials.size(); ++material_idx)
			{
				const auto& material_data = in_materials[material_idx];

				tinyxml2::XMLElement* mtl_inst = mtl_root->InsertNewChildElement("Material");
				mtl_inst->SetAttribute("ID", static_cast<unsigned int>(material_idx));
				mtl_inst->SetAttribute("Name", material_data.name.c_str());
				mtl_inst->SetAttribute("Shader", ABSOLUTE_PATH("Shaders/DeferredShaders/GBufferMaterial.glsl").c_str()); /* 首次加载使用默认材质 */

				if (!material_data.diffuse_texname.empty())
					mtl_inst->SetAttribute("AlbedoTex", (basedir + material_data.diffuse_texname).c_str());

				if (!material_data.specular_texname.empty())
					mtl_inst->SetAttribute("SpecularTex", (basedir + material_data.specular_texname).c_str());

				if (!material_data.bump_texname.empty())
					mtl_inst->SetAttribute("NormalTex", (basedir + material_data.bump_texname).c_str());

				if (!material_data.roughness_texname.empty())
					mtl_inst->SetAttribute("RoughnessTex", (basedir + material_data.roughness_texname).c_str());

				if (!material_data.metallic_texname.empty())
					mtl_inst->SetAttribute("MetallicTex", (basedir + material_data.metallic_texname).c_str());

				if (!material_data.emissive_texname.empty())
					mtl_inst->SetAttribute("EmissiveTex", (basedir + material_data.emissive_texname).c_str());

				glm::vec3 diffuse(material_data.diffuse[0], material_data.diffuse[1], material_data.diffuse[2]);
				mtl_inst->SetAttribute("Diffuse", ToString(diffuse).c_str());
				glm::vec3 specular(material_data.specular[0], material_data.specular[1], material_data.specular[2]);
				mtl_inst->SetAttribute("Specular", ToString(specular).c_str());
			}

			/* 保存材质文件 */
			auto out_mtl_path = filepath + ".mtl";
			out_mtl_file->SaveFile(out_mtl_path.c_str());
			delete out_mtl_file;
		}
	}
}
