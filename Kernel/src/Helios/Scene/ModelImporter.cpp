#include "Pch.h"
#include "ModelImporter.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/material.h>

#include "Helios/Common/Utils.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Scene/Material.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"
#include "SceneCommon.h"

/* 贴图通道拆分要写 PNG（打包金属粗糙度的拆分 / 未压缩内嵌贴图的落盘）：
 * stb_image 的实现已在 OpenGLTexture.cpp，这里只补 image_write 的实现（全工程唯一一份）。 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace Helios
{
	namespace
	{
		/* 顶点契约（与内置几何、G Buffer 着色器一致）：位置 f3 / 法线 f3 / 颜色 f4 / UV f3 / 切线 f3。
		 * 元素按这个顺序声明，location 就是 0..4；stride 按 float 数记（16）。 */
		constexpr size_t kVertexElementCount = 5;
		constexpr const char* kVertexElementNames[kVertexElementCount] = {
			"a_Position", "a_Normal", "a_Color", "a_TexCoord", "a_Tangent"
		};
		constexpr BufferDataType kVertexElementTypes[kVertexElementCount] = {
			BufferDataType::Float3, BufferDataType::Float3, BufferDataType::Float4,
			BufferDataType::Float3, BufferDataType::Float3
		};
		constexpr uint32_t kVertexStride = 16; /* 每个顶点的 float 数 */

		/* 新材质用的默认着色器（与模型编辑器导入同一份） */
		constexpr const char* kDefaultMaterialShader = "Shaders/MaterialShaders/PBRStandard.glsl";

		/* 一个 Obj shape / assimp mesh 的转换结果：对应一个 MeshSegment */
		struct ShapeImportResult
		{
			std::string Name;
			std::vector<float> VertexData;   /* kVertexStride 个 float 一个顶点 */
			std::vector<uint32_t> Indices;
			glm::vec3 AabbMin{ 0.0f };
			glm::vec3 AabbMax{ 0.0f };
			int MaterialIndex{ -1 };
		};

		/* 一张要收进目标目录的贴图：外部文件复制过来；内嵌的（fbx / glb）字节直接写成文件 */
		struct ImportedTexture
		{
			std::string SourcePath;              /* 外部贴图的绝对源路径；内嵌为空 */
			std::string DestinationRelative;     /* 目标目录下的相对落点 */
			std::vector<unsigned char> Embedded; /* 内嵌贴图的文件字节（PNG / JPG / …） */
		};

		/* 一条材质槽：贴图按引擎参数名绑好（值 = ImportedTexture 下标） */
		struct ImportedMaterial
		{
			std::string SlotName;
			std::vector<std::pair<std::string, size_t>> Textures;
		};

		/* 一次导入的中间结果：各格式解析器填它，收尾步骤（收文件 / 写 .mesh / .mtl）共用 */
		struct ImportedModel
		{
			std::vector<ShapeImportResult> Shapes;
			std::vector<ImportedMaterial> Materials;
			std::vector<ImportedTexture> Textures;
		};

		constexpr size_t kInvalidTextureIndex = static_cast<size_t>(-1);

		/* 导入中的一个顶点：切线逐面累加、收尾时去掉沿法线的分量 */
		struct ImportedVertex
		{
			glm::vec3 Position{ 0.0f };
			glm::vec3 Normal{ 0.0f, 0.0f, 1.0f };
			glm::vec3 Color{ 1.0f };
			glm::vec2 TexCoord{ 0.0f };
			glm::vec3 TangentAccum{ 0.0f };
		};

		/* 面切线（两条边与 UV 的差分；UV 退化的面给固定朝向，保证 TBN 可建） */
		glm::vec3 ComputeFaceTangent(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
			const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2)
		{
			const glm::vec3 edge01 = p1 - p0;
			const glm::vec3 edge02 = p2 - p0;
			const glm::vec2 uv01 = uv1 - uv0;
			const glm::vec2 uv02 = uv2 - uv0;
			const float determinant = uv01.x * uv02.y - uv01.y * uv02.x;
			if (std::fabs(determinant) <= 1.0e-12f)
				return glm::vec3(1.0f, 0.0f, 0.0f);
			return (1.0f / determinant) * (uv02.y * edge01 - uv01.y * edge02);
		}

		/* 顶点收尾：切线去掉沿法线的分量（退化换一条不平行方向）、再按 16 float 契约打包
		 * （位置 f3 / 法线 f3 / 颜色 f4[w 补 1] / UV f3[w 补 0] / 切线 f3）。 */
		void FinalizeVertices(const std::vector<ImportedVertex>& vertices, std::vector<float>& out_vertex_data)
		{
			out_vertex_data.reserve(vertices.size() * kVertexStride);
			for (const ImportedVertex& vertex : vertices)
			{
				const float normal_length = glm::length(vertex.Normal);
				const glm::vec3 normal = (normal_length > 1.0e-6f)
					? vertex.Normal / normal_length : glm::vec3(0.0f, 0.0f, 1.0f);

				glm::vec3 tangent = vertex.TangentAccum - normal * glm::dot(normal, vertex.TangentAccum);
				if (glm::dot(tangent, tangent) < 1.0e-12f)
				{
					const glm::vec3 fallback = (std::fabs(normal.x) < 0.9f)
						? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
					tangent = fallback - normal * glm::dot(normal, fallback);
				}
				tangent = glm::normalize(tangent);

				out_vertex_data.emplace_back(vertex.Position.x);
				out_vertex_data.emplace_back(vertex.Position.y);
				out_vertex_data.emplace_back(vertex.Position.z);
				out_vertex_data.emplace_back(normal.x);
				out_vertex_data.emplace_back(normal.y);
				out_vertex_data.emplace_back(normal.z);
				out_vertex_data.emplace_back(vertex.Color.r);
				out_vertex_data.emplace_back(vertex.Color.g);
				out_vertex_data.emplace_back(vertex.Color.b);
				out_vertex_data.emplace_back(1.0f);
				out_vertex_data.emplace_back(vertex.TexCoord.x);
				out_vertex_data.emplace_back(vertex.TexCoord.y);
				out_vertex_data.emplace_back(0.0f);
				out_vertex_data.emplace_back(tangent.x);
				out_vertex_data.emplace_back(tangent.y);
				out_vertex_data.emplace_back(tangent.z);
			}
		}

		/* 写出 .mesh：布局与 Model::Create 的读取一一对应 */
		bool WriteMeshFile(const std::filesystem::path& mesh_path, const std::vector<ShapeImportResult>& shapes)
		{
			std::ofstream mesh_file(mesh_path, std::ios::out | std::ios::binary);
			if (!mesh_file)
			{
				CORE_LOG_ERROR("Failed to open mesh file for writing: {}.", PathToUtf8(mesh_path));
				return false;
			}

			const size_t shape_count = shapes.size();
			mesh_file.write((char*)&shape_count, sizeof(size_t));

			for (const ShapeImportResult& shape : shapes)
			{
				const size_t name_size = shape.Name.size();
				mesh_file.write((char*)&name_size, sizeof(size_t));
				mesh_file.write(shape.Name.c_str(), static_cast<std::streamsize>(name_size));

				const uint32_t vertex_count = static_cast<uint32_t>(shape.VertexData.size() / kVertexStride);
				mesh_file.write((char*)&vertex_count, sizeof(uint32_t));

				const size_t index_count = shape.Indices.size();
				mesh_file.write((char*)&index_count, sizeof(size_t));
				mesh_file.write((char*)shape.Indices.data(),
					static_cast<std::streamsize>(index_count * sizeof(uint32_t)));

				const size_t vertex_buffer_count = 1;
				mesh_file.write((char*)&vertex_buffer_count, sizeof(size_t));
				mesh_file.write((char*)&kVertexStride, sizeof(uint32_t));
				mesh_file.write((char*)shape.VertexData.data(),
					static_cast<std::streamsize>(shape.VertexData.size() * sizeof(float)));

				const size_t element_count = kVertexElementCount;
				mesh_file.write((char*)&element_count, sizeof(size_t));
				size_t element_offset = 0; /* 元素在 stride 内的字节偏移（读取端会按顺序重算，这里写实际值） */
				for (size_t element_index = 0; element_index < kVertexElementCount; ++element_index)
				{
					const char* element_name = kVertexElementNames[element_index];
					const size_t element_name_size = std::strlen(element_name);
					mesh_file.write((char*)&element_name_size, sizeof(size_t));
					mesh_file.write(element_name, static_cast<std::streamsize>(element_name_size));

					const uint8_t element_type = static_cast<uint8_t>(kVertexElementTypes[element_index]);
					mesh_file.write((char*)&element_type, sizeof(uint8_t));
					mesh_file.write((char*)&element_offset, sizeof(size_t));
					const bool normalized = false;
					mesh_file.write((char*)&normalized, sizeof(bool));

					element_offset += BufferElement(element_name, kVertexElementTypes[element_index]).Size;
				}

				const int material_index = shape.MaterialIndex;
				mesh_file.write((char*)&material_index, sizeof(int));
				mesh_file.write((char*)&shape.AabbMin, sizeof(glm::vec3));
				mesh_file.write((char*)&shape.AabbMax, sizeof(glm::vec3));
			}

			mesh_file.close();
			return true;
		}

		/* 贴图引用解析成源文件路径（相对路径按模型所在目录解释） */
		bool ResolveSourceTexture(const std::filesystem::path& source_dir,
			const std::string& texture_name, std::filesystem::path& out_path)
		{
			out_path.clear();
			if (texture_name.empty())
				return false;

			std::filesystem::path texture_path;
			if (!TryPathFromUtf8(texture_name, texture_path))
				return false;

			out_path = texture_path.is_absolute() ? texture_path : source_dir / texture_path;
			return true;
		}

		/* 贴图在目标目录里的相对落点：复制时保持相对子目录结构；源目录之外的引用退化为文件名 */
		std::filesystem::path TextureDestination(const std::filesystem::path& source_dir,
			const std::string& texture_name)
		{
			std::filesystem::path source_path;
			if (!ResolveSourceTexture(source_dir, texture_name, source_path))
				return {};

			std::filesystem::path relative = source_path.lexically_relative(source_dir);
			if (relative.empty() || *relative.begin() == "..")
				relative = source_path.filename();
			return relative;
		}

		/* 复制模型文件本体（已经就在目标目录里则跳过；目录先建出来） */
		bool CopyModelFile(const std::filesystem::path& source_path, const std::filesystem::path& dest_dir)
		{
			std::error_code error;
			std::filesystem::create_directories(dest_dir, error);
			if (error)
			{
				CORE_LOG_ERROR("Failed to create model folder: {}.", PathToUtf8(dest_dir));
				return false;
			}

			std::error_code equivalent_error;
			if (std::filesystem::equivalent(source_path.parent_path(), dest_dir, equivalent_error))
				return true;

			std::filesystem::copy_file(source_path, dest_dir / source_path.filename(),
				std::filesystem::copy_options::overwrite_existing, error);
			if (error)
			{
				CORE_LOG_ERROR("Failed to copy model file: {}.", PathToUtf8(source_path));
				return false;
			}
			return true;
		}

		/* 登记一张外部贴图：按源路径去重（同一张图被多个材质引用只收一份）；
		 * 源文件不在（或路径非法）返回 kInvalidTextureIndex */
		size_t RegisterExternalTexture(ImportedModel& model, const std::filesystem::path& source_dir,
			const std::string& texture_name)
		{
			std::filesystem::path source_texture;
			if (!ResolveSourceTexture(source_dir, texture_name, source_texture))
				return kInvalidTextureIndex;

			std::error_code error;
			if (!std::filesystem::is_regular_file(source_texture, error))
			{
				CORE_LOG_WARN("Texture referenced by the model is missing: {}.", texture_name);
				return kInvalidTextureIndex;
			}

			const std::string source_key = PathToUtf8(source_texture);
			for (size_t index = 0; index < model.Textures.size(); ++index)
			{
				if (model.Textures[index].Embedded.empty() && model.Textures[index].SourcePath == source_key)
					return index;
			}

			ImportedTexture texture;
			texture.SourcePath = source_key;
			texture.DestinationRelative = PathToUtf8(TextureDestination(source_dir, texture_name));
			model.Textures.push_back(std::move(texture));
			return model.Textures.size() - 1;
		}

		/* 登记一张内嵌贴图：按目标文件名去重，bytes 是文件字节（PNG / JPG 等） */
		size_t RegisterEmbeddedTexture(ImportedModel& model, const std::string& destination_name,
			std::vector<unsigned char> bytes)
		{
			if (bytes.empty())
				return kInvalidTextureIndex;

			for (size_t index = 0; index < model.Textures.size(); ++index)
			{
				if (model.Textures[index].DestinationRelative == destination_name)
					return index;
			}

			ImportedTexture texture;
			texture.DestinationRelative = destination_name;
			texture.Embedded = std::move(bytes);
			model.Textures.push_back(std::move(texture));
			return model.Textures.size() - 1;
		}

		/* RGBA 像素编码成 PNG 字节（内嵌贴图落盘 / 打包通道拆分共用） */
		bool EncodeRgbaToPng(const unsigned char* pixels, int width, int height, std::vector<unsigned char>& out_png)
		{
			out_png.clear();
			const auto append_bytes = [](void* context, void* data, int size)
			{
				auto* bytes = static_cast<std::vector<unsigned char>*>(context);
				const auto* begin = static_cast<unsigned char*>(data);
				bytes->insert(bytes->end(), begin, begin + size);
			};
			return stbi_write_png_to_func(append_bytes, &out_png, width, height, 4, pixels, width * 4) != 0;
		}

		/* 贴图收进目标目录：外部文件复制（模型已在目标目录里就跳过），内嵌的字节写成文件 */
		void MaterializeTextures(const std::filesystem::path& dest_dir, const ImportedModel& model, bool copy_external)
		{
			std::error_code error;
			for (const ImportedTexture& texture : model.Textures)
			{
				const std::filesystem::path dest_texture = dest_dir / PathFromUtf8(texture.DestinationRelative);

				if (!texture.Embedded.empty())
				{
					std::filesystem::create_directories(dest_texture.parent_path(), error);
					std::ofstream texture_file(dest_texture, std::ios::binary | std::ios::trunc);
					texture_file.write(reinterpret_cast<const char*>(texture.Embedded.data()),
						static_cast<std::streamsize>(texture.Embedded.size()));
					continue;
				}

				if (!copy_external || texture.SourcePath.empty())
					continue;

				std::filesystem::create_directories(dest_texture.parent_path(), error);
				std::filesystem::copy_file(PathFromUtf8(texture.SourcePath), dest_texture,
					std::filesystem::copy_options::overwrite_existing, error);
				if (error)
					CORE_LOG_WARN("Failed to copy texture: {}.", texture.SourcePath);
			}
		}

		/* 写出伴生 .mtl（槽表）：一条材质一条，贴图引用「收进来之后」的落点
		 * （序列化时统一转成资产相对形式）。 */
		void WriteMaterialFile(const std::filesystem::path& mtl_path, const std::filesystem::path& dest_dir,
			const ImportedModel& model)
		{
			auto material_group = CreateSharedPtr<MaterialGroup>();
			/* 着色器按资产根解析成绝对路径：相对路径直接喂文件流是相对进程工作目录的，会读不到 */
			auto default_shader = AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH(kDefaultMaterialShader));

			TextureLoadConfig load_config;
			load_config.IsFlipV = false; /* UV 在导入时已翻转，贴图按原样上传（与既有模型资产同口径） */

			for (size_t material_index = 0; material_index < model.Materials.size(); ++material_index)
			{
				const ImportedMaterial& material_data = model.Materials[material_index];
				auto material = Material::Create(default_shader);

				for (const auto& [param_name, texture_index] : material_data.Textures)
				{
					if (texture_index >= model.Textures.size())
						continue;

					const std::filesystem::path texture_path = dest_dir / PathFromUtf8(model.Textures[texture_index].DestinationRelative);
					std::error_code error;
					if (!std::filesystem::is_regular_file(texture_path, error))
						continue;

					auto texture = AssetManager::Instance().GetOrLoad<DeviceTexture>(PathToUtf8(texture_path), load_config);
					if (texture != nullptr)
						material->SetTexture(param_name, texture);
				}

				material_group->EmplaceMaterial(material);
				material_group->SetEntrySlotName(static_cast<int>(material_index), material_data.SlotName);
			}

			material_group->Serializer(PathToUtf8(mtl_path));
		}

		/* Obj 伴生材质的避让：引擎写的 .mtl 是 XML 格式，和 obj 的 mtllib 同名会互相踩。
		 * 把 obj 风格的伴生 .mtl 收成 <名字>.obj.mtl（归档的自包含）——
		 *   - 项目外收进来的：复制一份，并把复制版 obj 的 mtllib 指向改成新名字（源文件不动）
		 *   - 原地转换：已存在的 obj 风格 .mtl 复制保留（不改用户的源 obj；之后 XML 会写到原位）
		 * 目标位置已经是 XML 的（上一次的转换产物）不动，后面照常重写。 */
		void ArchiveObjMaterials(const std::filesystem::path& source_path, const std::filesystem::path& dest_dir)
		{
			std::ifstream obj_stream(source_path, std::ios::in | std::ios::binary);
			if (!obj_stream)
				return;

			/* 找 mtllib 行引用的伴生材质文件（取第一个真实存在的） */
			std::string referenced;
			{
				std::string line;
				while (std::getline(obj_stream, line))
				{
					const size_t token = line.find("mtllib");
					if (token == std::string::npos || line.find_first_not_of(" \t") != token)
						continue;

					std::istringstream tokens(line.substr(token + 6));
					std::string candidate;
					while (tokens >> candidate)
					{
						std::filesystem::path resolved;
						if (ResolveSourceTexture(source_path.parent_path(), candidate, resolved)
							&& std::filesystem::is_regular_file(resolved))
						{
							referenced = candidate;
							break;
						}
					}
					if (!referenced.empty())
						break;
				}
			}
			if (referenced.empty())
				return;

			/* 引用的文件是 XML 格式就不用归档（之前转换过 / 就是引擎格式） */
			std::filesystem::path source_mtl;
			ResolveSourceTexture(source_path.parent_path(), referenced, source_mtl);
			{
				std::ifstream material_stream(source_mtl, std::ios::in | std::ios::binary);
				std::string head;
				std::getline(material_stream, head);
				const size_t first = head.find_first_not_of(" \t");
				if (first != std::string::npos && head[first] == '<')
					return;
			}

			std::error_code error;
			const std::string stem = PathToUtf8(source_path.stem());
			const std::string archive_name = stem + ".obj.mtl";
			const std::filesystem::path archive_path = dest_dir / PathFromUtf8(archive_name);

			std::filesystem::path source_mtl_absolute = std::filesystem::absolute(source_mtl, error);
			const std::filesystem::path archive_absolute = std::filesystem::absolute(archive_path, error);
			if (source_mtl_absolute != archive_absolute)
			{
				std::filesystem::copy_file(source_mtl, archive_path,
					std::filesystem::copy_options::overwrite_existing, error);
				if (error)
				{
					CORE_LOG_WARN("Failed to archive obj material file: {}.", PathToUtf8(source_mtl));
					return;
				}
			}

			/* 项目外的把复制版 obj 的 mtllib 指向改成归档名（原地转换不动用户的源文件） */
			std::error_code equivalent_error;
			if (std::filesystem::equivalent(source_path.parent_path(), dest_dir, equivalent_error))
				return;

			const std::filesystem::path copied_obj = dest_dir / source_path.filename();
			std::ifstream copied_stream(copied_obj, std::ios::in | std::ios::binary);
			if (!copied_stream)
				return;

			std::vector<std::string> lines;
			{
				std::string line;
				while (std::getline(copied_stream, line))
				{
					if (line.find("mtllib") == line.find_first_not_of(" \t"))
						line = "mtllib " + archive_name;
					lines.push_back(std::move(line));
				}
			}
			copied_stream.close();

			std::ofstream patched_stream(copied_obj, std::ios::out | std::ios::binary | std::ios::trunc);
			for (const std::string& line : lines)
			{
				patched_stream << line << '\n';
			}
		}

		/* 收尾：收文件（模型本体 + 贴图）→ 写 .mesh → 写 .mtl（没有材质就不写）。
		 * preserve_obj_materials：Obj 源要走伴生材质避让（见 ArchiveObjMaterials）。
		 * 返回生成的 .mesh 路径；失败返回空串。 */
		std::string FinishImport(const std::filesystem::path& source_path, const std::filesystem::path& dest_dir,
			ImportedModel& model, bool preserve_obj_materials)
		{
			if (!CopyModelFile(source_path, dest_dir))
				return {};

			if (preserve_obj_materials)
				ArchiveObjMaterials(source_path, dest_dir);

			std::error_code equivalent_error;
			const bool in_place = std::filesystem::equivalent(source_path.parent_path(), dest_dir, equivalent_error);
			MaterializeTextures(dest_dir, model, /*copy_external=*/!in_place);

			const std::string stem = PathToUtf8(source_path.stem());
			const std::filesystem::path mesh_path = dest_dir / PathFromUtf8(stem + ".mesh");
			if (!WriteMeshFile(mesh_path, model.Shapes))
				return {};

			if (!model.Materials.empty())
				WriteMaterialFile(dest_dir / PathFromUtf8(stem + ".mtl"), dest_dir, model);

			return PathToUtf8(mesh_path);
		}

		/* assimp 材质里的贴图引用：外部文件 或 scene.mTextures 里的内嵌项（*N） */
		struct AssimpTextureRef
		{
			bool IsEmbedded{ false };
			std::string SourceName;        /* 外部：材质里的路径串（相对模型目录 / 绝对） */
			std::string EmbeddedName;      /* 内嵌：*N 的原文（去重与命名用） */
			const aiTexture* Embedded{ nullptr };
		};

		/* 登记一条 assimp 贴图引用：外部复制 / 内嵌落盘（生成名 <模型名>_<序号>.<扩展名>） */
		size_t RegisterAssimpTexture(ImportedModel& model, const std::filesystem::path& source_dir,
			const std::string& stem, const AssimpTextureRef& reference)
		{
			if (!reference.IsEmbedded || reference.Embedded == nullptr)
				return RegisterExternalTexture(model, source_dir, reference.SourceName);

			const aiTexture& texture = *reference.Embedded;

			/* 生成名先定（压缩内嵌的扩展名看格式提示；未压缩的编码成 png），按名字去重 */
			const bool is_raw = texture.mHeight != 0;
			const std::string extension_name = is_raw ? "png"
				: ((texture.achFormatHint[0] != '\0') ? texture.achFormatHint : "png");
			const std::string tag = reference.EmbeddedName.empty() ? "tex" : reference.EmbeddedName.substr(1);
			const std::string destination_name = stem + "_" + tag + "." + extension_name;

			for (size_t index = 0; index < model.Textures.size(); ++index)
			{
				if (model.Textures[index].DestinationRelative == destination_name)
					return index;
			}

			std::vector<unsigned char> bytes;
			if (is_raw)
			{
				/* 未压缩内嵌：pcData 是 RGBA 像素，编码成 PNG */
				if (!EncodeRgbaToPng(reinterpret_cast<const unsigned char*>(texture.pcData),
					static_cast<int>(texture.mWidth), static_cast<int>(texture.mHeight), bytes))
					return kInvalidTextureIndex;
			}
			else
			{
				/* 压缩内嵌：pcData 就是文件字节（mWidth = 字节数） */
				const auto* begin = reinterpret_cast<const unsigned char*>(texture.pcData);
				bytes.assign(begin, begin + texture.mWidth);
			}

			return RegisterEmbeddedTexture(model, destination_name, std::move(bytes));
		}

		/* 把打包的金属粗糙度贴图拆成两张单通道图（glTF / ORM 惯用：G = 粗糙度、B = 金属度）。
		 * 像素解不出来返回 false（调用方退回原样直挂，至少数据在）。 */
		bool SplitPackedChannels(const AssimpTextureRef& reference, const std::filesystem::path& source_dir,
			std::vector<unsigned char>& out_roughness_png, std::vector<unsigned char>& out_metallic_png)
		{
			int width = 0;
			int height = 0;
			std::vector<unsigned char> pixels;

			if (reference.IsEmbedded && reference.Embedded != nullptr && reference.Embedded->mHeight != 0)
			{
				/* 未压缩内嵌：pcData 就是 RGBA 像素 */
				const aiTexture& texture = *reference.Embedded;
				width = static_cast<int>(texture.mWidth);
				height = static_cast<int>(texture.mHeight);
				const auto* begin = reinterpret_cast<const unsigned char*>(texture.pcData);
				pixels.assign(begin, begin + static_cast<size_t>(width) * height * 4);
			}
			else
			{
				int channels = 0;
				unsigned char* decoded = nullptr;
				if (reference.IsEmbedded && reference.Embedded != nullptr)
				{
					const aiTexture& texture = *reference.Embedded;
					decoded = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(texture.pcData),
						static_cast<int>(texture.mWidth), &width, &height, &channels, 4);
				}
				else
				{
					std::filesystem::path source_texture;
					if (!ResolveSourceTexture(source_dir, reference.SourceName, source_texture))
						return false;
					decoded = stbi_load(PathToUtf8(source_texture).c_str(), &width, &height, &channels, 4);
				}

				if (decoded == nullptr || width <= 0 || height <= 0)
				{
					if (decoded != nullptr)
						stbi_image_free(decoded);
					return false;
				}
				pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);
				stbi_image_free(decoded);
			}

			/* 拆分：两张图都把通道写在 R（着色器采样 .r），其余通道补满 */
			std::vector<unsigned char> roughness(pixels.size(), 255);
			std::vector<unsigned char> metallic(pixels.size(), 255);
			for (size_t pixel = 0; pixel + 3 < pixels.size(); pixel += 4)
			{
				roughness[pixel + 0] = pixels[pixel + 1]; /* G -> R */
				metallic[pixel + 0] = pixels[pixel + 2];  /* B -> R */
			}

			return EncodeRgbaToPng(roughness.data(), width, height, out_roughness_png)
				&& EncodeRgbaToPng(metallic.data(), width, height, out_metallic_png);
		}

		/* 从 assimp 材质登记引擎参数（返回登记完的槽；名字与贴图都空的返回空壳，调用方跳过）。
		 * 参数名用 u_* 一族（与 GBuffer / PBRStandard 的声明同名）：
		 *   BASE_COLOR 或 DIFFUSE -> u_AlbedoTexture；NORMALS 或 NORMAL_CAMERA 或 HEIGHT -> u_NormalTexture；
		 *   EMISSION_COLOR 或 EMISSIVE -> u_EmissiveTexture；AMBIENT / SPECULAR / DISPLACEMENT 对应同名槽；
		 *   METALNESS / DIFFUSE_ROUGHNESS 两条贴图指向同一张时按打包图拆通道（glTF 口径），
		 *   不同图就各自直挂。 */
		ImportedMaterial BuildAssimpMaterial(const aiScene& scene, const aiMaterial& material,
			const std::filesystem::path& source_dir, const std::string& stem, size_t material_index,
			ImportedModel& out_model)
		{
			ImportedMaterial out;

			aiString material_name;
			if (material.Get(AI_MATKEY_NAME, material_name) == AI_SUCCESS)
				out.SlotName = material_name.C_Str();

			const auto resolve = [&scene, &material](aiTextureType type, AssimpTextureRef& out_reference) -> bool
			{
				aiString path;
				if (material.GetTexture(type, 0, &path) != AI_SUCCESS)
					return false;

				const std::string name = path.C_Str();
				if (name.empty())
					return false;

				if (name[0] == '*')
				{
					const int embedded_index = std::atoi(name.c_str() + 1);
					if (embedded_index < 0 || embedded_index >= static_cast<int>(scene.mNumTextures))
						return false;
					out_reference.IsEmbedded = true;
					out_reference.EmbeddedName = name;
					out_reference.Embedded = scene.mTextures[embedded_index];
					return true;
				}

				out_reference.SourceName = name;
				return true;
			};

			const auto attach = [&](const char* param_name, const AssimpTextureRef& reference)
			{
				const size_t index = RegisterAssimpTexture(out_model, source_dir, stem, reference);
				if (index != kInvalidTextureIndex)
					out.Textures.emplace_back(param_name, index);
			};

			AssimpTextureRef albedo;
			if (resolve(aiTextureType_BASE_COLOR, albedo) || resolve(aiTextureType_DIFFUSE, albedo))
				attach("u_AlbedoTexture", albedo);

			/* 法线：声明成 NORMALS / NORMAL_CAMERA 优先；只有 Bump（含 obj 的 map_Bump、fbx 的
			 * Bump 槽）时也按法线贴图用 —— 拿高度图当高度会出怪波动，按法线看反而对得上大头 */
			AssimpTextureRef normal;
			if (resolve(aiTextureType_NORMALS, normal) || resolve(aiTextureType_NORMAL_CAMERA, normal)
				|| resolve(aiTextureType_HEIGHT, normal))
				attach("u_NormalTexture", normal);

			AssimpTextureRef emissive;
			if (resolve(aiTextureType_EMISSION_COLOR, emissive) || resolve(aiTextureType_EMISSIVE, emissive))
				attach("u_EmissiveTexture", emissive);

			AssimpTextureRef ambient;
			if (resolve(aiTextureType_AMBIENT, ambient))
				attach("u_AmbientTexture", ambient);

			AssimpTextureRef specular;
			if (resolve(aiTextureType_SPECULAR, specular))
				attach("u_SpecularTexture", specular);

			AssimpTextureRef displacement;
			if (resolve(aiTextureType_DISPLACEMENT, displacement))
				attach("u_DisplacementTexture", displacement);

			AssimpTextureRef metallic;
			AssimpTextureRef roughness;
			const bool has_metallic = resolve(aiTextureType_METALNESS, metallic);
			const bool has_roughness = resolve(aiTextureType_DIFFUSE_ROUGHNESS, roughness);
			const bool packed = has_metallic && has_roughness
				&& metallic.IsEmbedded == roughness.IsEmbedded
				&& (metallic.IsEmbedded ? metallic.EmbeddedName == roughness.EmbeddedName
					: metallic.SourceName == roughness.SourceName);
			if (packed)
			{
				std::vector<unsigned char> roughness_png;
				std::vector<unsigned char> metallic_png;
				if (SplitPackedChannels(metallic, source_dir, roughness_png, metallic_png))
				{
					const std::string prefix = stem + "_material" + std::to_string(material_index);
					out.Textures.emplace_back("u_RoughnessTexture",
						RegisterEmbeddedTexture(out_model, prefix + "_roughness.png", std::move(roughness_png)));
					out.Textures.emplace_back("u_MetallicTexture",
						RegisterEmbeddedTexture(out_model, prefix + "_metallic.png", std::move(metallic_png)));
				}
				else
				{
					CORE_LOG_WARN("Failed to split packed metallic-roughness texture in material {}.", out.SlotName);
					attach("u_RoughnessTexture", roughness);
					attach("u_MetallicTexture", metallic);
				}
			}
			else
			{
				if (has_roughness)
					attach("u_RoughnessTexture", roughness);
				if (has_metallic)
					attach("u_MetallicTexture", metallic);
			}

			return out;
		}

		/* 交换格式（obj / fbx / gltf / glb / dae / stl）→ 中间结果 —— 全格式统一走这里。
		 * 统一烘焙成静态网格：节点变换烘进顶点、三角化、补法线与 UV、同属性顶点合并；
		 * 蒙皮与动画不带（.mesh 是静态格式）。 */
		bool LoadAssimpModel(const std::filesystem::path& source_path, ImportedModel& out_model)
		{
			Assimp::Importer importer;
			unsigned int flags = aiProcess_Triangulate | aiProcess_PreTransformVertices
				| aiProcess_GenSmoothNormals | aiProcess_GenUVCoords | aiProcess_FindInvalidData
				| aiProcess_JoinIdenticalVertices | aiProcess_FlipUVs;
			/* UV 原点：assimp 里 UV 统一是左下原点（glTF 导入器会先自己把 V 翻一道），
			 * 引擎纹理原点在左上 —— 这里统一翻一次（与 obj 的 1-v 同口径） */

			const aiScene* scene = importer.ReadFile(PathToUtf8(source_path), flags);
			if (scene == nullptr || scene->mNumMeshes == 0)
			{
				CORE_LOG_ERROR("Failed to load model: {}. {}", PathToUtf8(source_path), importer.GetErrorString());
				return false;
			}

			const std::string stem = PathToUtf8(source_path.stem());
			const std::filesystem::path source_dir = source_path.parent_path();

			/* 材质：没有贴图、名字为空或就是 assimp 的兜底名字的，不落进 .mtl
			 * （如 stl 这类没有材质的格式不生成槽表），段里的索引跟着重映射 */
			std::vector<int> material_remap(scene->mNumMaterials, -1);
			for (unsigned material_index = 0; material_index < scene->mNumMaterials; ++material_index)
			{
				ImportedMaterial material = BuildAssimpMaterial(*scene, *scene->mMaterials[material_index],
					source_dir, stem, material_index, out_model);
				if (material.Textures.empty()
					&& (material.SlotName.empty() || material.SlotName == AI_DEFAULT_MATERIAL_NAME))
					continue;

				material_remap[material_index] = static_cast<int>(out_model.Materials.size());
				out_model.Materials.push_back(std::move(material));
			}

			/* 网格段：assimp 的顶点已按 位置 / 法线 / UV 缝 拆好，直接用它的索引 */
			for (unsigned mesh_index = 0; mesh_index < scene->mNumMeshes; ++mesh_index)
			{
				const aiMesh* mesh = scene->mMeshes[mesh_index];
				if (mesh == nullptr || mesh->mNumVertices == 0 || mesh->mNumFaces == 0)
					continue;

				ShapeImportResult result;
				result.Name = (mesh->mName.length > 0)
					? std::string(mesh->mName.C_Str()) : ("mesh_" + std::to_string(mesh_index));
				result.AabbMin = glm::vec3(std::numeric_limits<float>::max());
				result.AabbMax = glm::vec3(-std::numeric_limits<float>::max());

				std::vector<ImportedVertex> vertices(mesh->mNumVertices);
				for (unsigned vertex_index = 0; vertex_index < mesh->mNumVertices; ++vertex_index)
				{
					ImportedVertex& vertex = vertices[vertex_index];
					vertex.Position = glm::vec3(mesh->mVertices[vertex_index].x,
						mesh->mVertices[vertex_index].y, mesh->mVertices[vertex_index].z);

					if (mesh->mNormals != nullptr)
					{
						vertex.Normal = glm::vec3(mesh->mNormals[vertex_index].x,
							mesh->mNormals[vertex_index].y, mesh->mNormals[vertex_index].z);
					}

					if (mesh->mColors[0] != nullptr)
					{
						vertex.Color = glm::vec3(mesh->mColors[0][vertex_index].r,
							mesh->mColors[0][vertex_index].g, mesh->mColors[0][vertex_index].b);
					}

					if (mesh->mTextureCoords[0] != nullptr)
					{
						vertex.TexCoord = glm::vec2(mesh->mTextureCoords[0][vertex_index].x,
							mesh->mTextureCoords[0][vertex_index].y);
					}

					result.AabbMin = glm::min(result.AabbMin, vertex.Position);
					result.AabbMax = glm::max(result.AabbMax, vertex.Position);
				}

				for (unsigned face_index = 0; face_index < mesh->mNumFaces; ++face_index)
				{
					const aiFace& face = mesh->mFaces[face_index];
					if (face.mNumIndices != 3) /* 三角化后应当恒为 3；异常面跳过 */
						continue;

					const uint32_t corner0 = face.mIndices[0];
					const uint32_t corner1 = face.mIndices[1];
					const uint32_t corner2 = face.mIndices[2];
					const glm::vec3 face_tangent = ComputeFaceTangent(
						vertices[corner0].Position, vertices[corner1].Position, vertices[corner2].Position,
						vertices[corner0].TexCoord, vertices[corner1].TexCoord, vertices[corner2].TexCoord);
					vertices[corner0].TangentAccum += face_tangent;
					vertices[corner1].TangentAccum += face_tangent;
					vertices[corner2].TangentAccum += face_tangent;

					result.Indices.emplace_back(corner0);
					result.Indices.emplace_back(corner1);
					result.Indices.emplace_back(corner2);
				}

				if (result.Indices.empty())
					continue;

				FinalizeVertices(vertices, result.VertexData);
				result.MaterialIndex = (mesh->mMaterialIndex < material_remap.size())
					? material_remap[mesh->mMaterialIndex] : -1;
				out_model.Shapes.push_back(std::move(result));
			}

			return !out_model.Shapes.empty();
		}
	}

	/* 导入 Obj 模型（ImportModel 的 obj 分支，也供检查直接调用）：解析与其它格式同走
	 * assimp（LoadAssimpModel），只多一步伴生材质避让（见 ArchiveObjMaterials） */
	std::string ImportObj(const std::string& filepath, const std::string& dest_dir)
	{
		if (filepath.empty() || dest_dir.empty())
		{
			CORE_LOG_ERROR("Model filepath or destination folder is empty.");
			return {};
		}

		std::filesystem::path source_path;
		if (!TryPathFromUtf8(filepath, source_path) || !std::filesystem::is_regular_file(source_path))
		{
			CORE_LOG_ERROR("Model file not found: {}.", filepath);
			return {};
		}

		ImportedModel model;
		if (!LoadAssimpModel(source_path, model))
			return {};

		return FinishImport(source_path, PathFromUtf8(dest_dir), model, /*preserve_obj_materials=*/true);
	}

	/* 导入模型：按后缀分派 —— obj 走内置解析，fbx / gltf / glb / dae / stl 走 assimp */
	std::string ImportModel(const std::string& filepath, const std::string& dest_dir)
	{
		if (filepath.empty() || dest_dir.empty())
		{
			CORE_LOG_ERROR("Model filepath or destination folder is empty.");
			return {};
		}

		std::filesystem::path source_path;
		if (!TryPathFromUtf8(filepath, source_path) || !std::filesystem::is_regular_file(source_path))
		{
			CORE_LOG_ERROR("Model file not found: {}.", filepath);
			return {};
		}

		std::string extension = ExtractFileSuffix(filepath);
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		extension.insert(extension.begin(), '.');

		if (extension == ".obj")
			return ImportObj(filepath, dest_dir);

		const bool assimp_format = extension == ".fbx" || extension == ".gltf" || extension == ".glb"
			|| extension == ".dae" || extension == ".stl";
		if (!assimp_format)
		{
			CORE_LOG_ERROR("Unsupported model format: {}.", filepath);
			return {};
		}

		ImportedModel model;
		if (!LoadAssimpModel(source_path, model))
			return {};

		/* glTF 的外部二进制缓冲（.bin）一并收进项目：源文件搬过去还能再打开 */
		if (extension == ".gltf")
		{
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(source_path.parent_path(), error))
			{
				if (!entry.is_regular_file(error))
					continue;

				std::string sibling_extension = PathToUtf8(entry.path().extension());
				std::transform(sibling_extension.begin(), sibling_extension.end(), sibling_extension.begin(),
					[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
				if (sibling_extension != ".bin")
					continue;

				ImportedTexture companion;
				companion.SourcePath = PathToUtf8(entry.path());
				companion.DestinationRelative = PathToUtf8(entry.path().filename());
				model.Textures.push_back(std::move(companion));
			}
		}

		return FinishImport(source_path, PathFromUtf8(dest_dir), model, /*preserve_obj_materials=*/false);
	}
}
