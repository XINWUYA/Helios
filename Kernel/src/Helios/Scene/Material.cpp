#include "Pch.h"
#include "Material.h"
#include <tinyxml2.h>

#include "SceneCommon.h"
#include "Helios/Application/AssetManager.h"
#include "Helios/VirtualDevice/DeviceShader.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{

	/* 无效Slot */
	constexpr uint8_t InvalidTextureSlot = 255;

	/* 默认材质和错误材质 */
	SharedPtr<Material> Material::m_pDefaultMaterial = CreateSharedPtr<Material>();// Material::Create();
	SharedPtr<Material> Material::m_pErrorMaterial = CreateSharedPtr<Material>();

	Material::~Material()
	{
		m_Parameters.clear();
	}

	void Material::SetShader(const SharedPtr<DeviceShader>& shader)
	{
		PROFILE_FUNCTION();

		if (m_pShader == shader)
			return;

		m_pShader = shader;
		/* Shader更换，binding布局也随之改变，刷新Shader的反射结果 */
		RefreshTextureBindings();
	}

	void Material::SetParameters(ParamType type, const std::string& name, const std::any& param)
	{
		PROFILE_FUNCTION();

		if (type == ParamType::Texture)
		{
			const auto texture = std::any_cast<const SharedPtr<DeviceTexture>>(param);
			if (!texture)
				return;
			m_Parameters[ToID(name)] = { ParamType::Texture, name, std::make_pair(texture, ResolveTextureBinding(name)) };
			return;
		}

		m_Parameters[ToID(name)] = { type, name, param };
	}

	void Material::SetTexture(const std::string& name, const SharedPtr<DeviceTexture>& texture)
	{
		PROFILE_FUNCTION();

		if (!texture)
			return;

		m_Parameters[ToID(name)] = { ParamType::Texture, name, std::make_pair(texture, ResolveTextureBinding(name)) };
	}

	/* 绑定材质中的各参数 */
	void Material::Bind()
	{
		PROFILE_FUNCTION();

		/* 先绑定Shader */
		m_pShader->Bind();

		/* 绑定参数 */
		for (auto& param : m_Parameters)
		{
			auto& param_info = param.second;
			auto& value = param_info.Value;
			switch (param_info.Type)
			{
			case ParamType::Texture:
				{
					/* 绑定纹理 */
					const auto texture_info = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(value);
					if (!texture_info.first)
						break;

					/* 反射不到绑定点时回退到槽位 0，绝不静默跳过绑定：
					 * 若管线要求采样器/纹理而未被绑定，Metal 校验层会直接断言终止。 */
					const uint32_t slot = (texture_info.second == InvalidTextureSlot) ? 0u : texture_info.second;
					texture_info.first->Bind(slot);
					m_pShader->SetInt(param_info.Name, static_cast<int>(slot));
				}
				break;
			case ParamType::Int:
				m_pShader->SetInt(param_info.Name, std::any_cast<int>(value));
				break;
			case ParamType::Float:
				m_pShader->SetFloat(param_info.Name, std::any_cast<float>(value));
				break;
			case ParamType::Vec2:
				m_pShader->SetFloat2(param_info.Name, std::any_cast<glm::vec2>(value));
				break;
			case ParamType::Vec3:
				m_pShader->SetFloat3(param_info.Name, std::any_cast<glm::vec3>(value));
				break;
			case ParamType::Vec4:
				m_pShader->SetFloat4(param_info.Name, std::any_cast<glm::vec4>(value));
				break;
			case ParamType::Mat4:
				m_pShader->SetMat4(param_info.Name, std::any_cast<glm::mat4>(value));
				break;
			}
		}
	}

	/* 解绑材质 */
	void Material::Unbind()
	{
		m_pShader->Unbind();
	}

	/* 默认材质 */
	SharedPtr<Material>& Material::Default()
	{
		if (!m_pDefaultMaterial->GetShader())
		{
			/* 只能在获取时设置Shader，在静态编译期，OpenGL尚未初始化，无法正确创建ShaderProgram */
			m_pDefaultMaterial->SetShader(ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/default.glsl")));
		}

		return m_pDefaultMaterial;
	}

	/* 错误材质 */
	SharedPtr<Material>& Material::Error()
	{
		if (!m_pErrorMaterial->GetShader())
		{
			m_pErrorMaterial->SetShader(ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/error.glsl")));
		}
		return m_pErrorMaterial;
	}
	
	/* 创建材质 */
	SharedPtr<Material> Material::Create(const SharedPtr<DeviceShader>& shader)
	{
		PROFILE_FUNCTION();

		auto material = CreateSharedPtr<Material>();
		material->SetShader(shader);

		return material;
	}

	uint32_t Material::ResolveTextureBinding(const std::string& name) const
	{
		if (!m_pShader)
			return InvalidTextureSlot;

		const int binding = m_pShader->GetUniformBinding(name);
		return binding < 0 ? InvalidTextureSlot : static_cast<uint32_t>(binding);
	}

	void Material::RefreshTextureBindings()
	{
		if (!m_pShader)
			return;

		for (auto& [id, param_info] : m_Parameters)
		{
			if (param_info.Type != ParamType::Texture)
				continue;

			auto texture_info = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(param_info.Value);
			texture_info.second = ResolveTextureBinding(param_info.Name);
			m_Parameters[id] = { param_info.Type, param_info.Name, texture_info };
		}
	}

	/* 添加一个Material */
	void MaterialGroup::EmplaceMaterial(const SharedPtr<Material>& material)
	{
		m_Materials.emplace_back(material);
	}

	/* 根据索引获取Material */
	const SharedPtr<Material>& MaterialGroup::GetMaterialByIndex(int idx)
	{
		if (idx < 0 || idx >= m_Materials.size())
		{
			CORE_LOG_ERROR("Unaccessable material idx, default return ErrorMaterial.");
			return Material::Error();
		}

		return m_Materials[idx];
	}

	/* 序列化 */
	void MaterialGroup::Serializer(const std::string& path)
	{
		m_Path = path;

		ASSERT(!m_Path.empty());
		
		/* 写入材质信息 */
		auto* out_mtl_file = new tinyxml2::XMLDocument();
		out_mtl_file->InsertEndChild(out_mtl_file->NewDeclaration());
		auto* mtl_root = out_mtl_file->NewElement("Materials");
		out_mtl_file->InsertEndChild(mtl_root);
		mtl_root->SetAttribute("Count", static_cast<unsigned int>(m_Materials.size()));

		for (int i = 0; i < m_Materials.size(); ++i)
		{
			auto& material = m_Materials[i];
			auto* mtl_doc = mtl_root->InsertNewChildElement("Material");

			/* ID */
			mtl_doc->SetAttribute("ID", i);

			/* Shader*/
			mtl_doc->SetAttribute("ShaderPath", RELATIVE_PATH(material->GetShader()->GetPath()).c_str());

			/* Parameters */
			auto params_root = mtl_doc->InsertNewChildElement("Parameters");
			auto& parameters = material->GetAllParameters();
			for (auto& param : parameters)
			{
				auto param_info = param.second;
				auto param_doc = params_root->InsertNewChildElement("ParamInfo");
				param_doc->SetAttribute("Type", static_cast<uint32_t>(param_info.Type));
				param_doc->SetAttribute("Name", param_info.Name.c_str());
				switch (param_info.Type)
				{
				case ParamType::Texture:
					{
						auto texture_doc = param_doc->InsertNewChildElement("Texture");
						const auto texture_info = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(param_info.Value);
						auto& texture = texture_info.first;
						
						texture_doc->SetAttribute("Path", RELATIVE_PATH(texture->GetPath()).c_str());
						texture_doc->SetAttribute("Slot", texture_info.second);

						auto load_config_doc = texture_doc->InsertNewChildElement("LoadConfig");
						auto load_config = texture->GetTextureLoadConfig();
						load_config_doc->SetAttribute("IsFlipV", load_config.IsFlipV);
						load_config_doc->SetAttribute("IsGenMips", load_config.IsGenMips);
						load_config_doc->SetAttribute("SamplerType", static_cast<uint32_t>(load_config.SamplerType));
					}
					break;
				case ParamType::Int:
					param_doc->SetAttribute("Value", std::any_cast<int>(param_info.Value));
					break;
				case ParamType::Float:
					param_doc->SetAttribute("Value", std::any_cast<float>(param_info.Value));
					break;
				case ParamType::Vec2: 
					param_doc->SetAttribute("Value", ToString(std::any_cast<glm::vec2>(param_info.Value)).c_str());
					break;
				case ParamType::Vec3: 
					param_doc->SetAttribute("Value", ToString(std::any_cast<glm::vec3>(param_info.Value)).c_str());
					break;
				case ParamType::Vec4: 
					param_doc->SetAttribute("Value", ToString(std::any_cast<glm::vec4>(param_info.Value)).c_str());
					break;
				}
			}

			/* RasterState */
			auto raster_state = material->GetRasterState();
			auto raster_state_doc = mtl_doc->InsertNewChildElement("RasterState");
			raster_state_doc->SetAttribute("CullMode", static_cast<uint32_t>(raster_state.CullMode));
			raster_state_doc->SetAttribute("FrontFaceType", static_cast<uint32_t>(raster_state.FrontFaceType));
			raster_state_doc->SetAttribute("EnableBlend", raster_state.EnableBlend);
			raster_state_doc->SetAttribute("BlendEquationRGB", static_cast<uint32_t>(raster_state.BlendEquationRGB));
			raster_state_doc->SetAttribute("BlendEquationA", static_cast<uint32_t>(raster_state.BlendEquationA));
			raster_state_doc->SetAttribute("BlendFuncSrcRGB", static_cast<uint32_t>(raster_state.BlendFuncSrcRGB));
			raster_state_doc->SetAttribute("BlendFuncSrcA", static_cast<uint32_t>(raster_state.BlendFuncSrcA));
			raster_state_doc->SetAttribute("BlendFuncDstRGB", static_cast<uint32_t>(raster_state.BlendFuncDstRGB));
			raster_state_doc->SetAttribute("BlendFuncDstA", static_cast<uint32_t>(raster_state.BlendFuncDstA));
			raster_state_doc->SetAttribute("EnableDepthWrite", raster_state.EnableDepthWrite);
			raster_state_doc->SetAttribute("DepthCompareFunc", static_cast<uint32_t>(raster_state.DepthCompareFunc));
			raster_state_doc->SetAttribute("EnableColorWrite", raster_state.EnableColorWrite);
		}

		/* 保存到文本 */
		out_mtl_file->SaveFile(path.c_str());
		delete out_mtl_file;
	}

	/* 反序列化 */
	bool MaterialGroup::Deserializer(const std::string& path)
	{
		PROFILE_FUNCTION();

		m_Path = ABSOLUTE_PATH(path);
		ASSERT(!m_Path.empty());

		/* 读取材质信息 */
		auto* in_mtl_file = new tinyxml2::XMLDocument();
		tinyxml2::XMLError error = in_mtl_file->LoadFile(m_Path.c_str());
		if (error != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_ERROR("Failed to deserializer mtl file: {}.", m_Path);
			return false;
		}

		tinyxml2::XMLElement* mtl_root = in_mtl_file->FirstChildElement("Materials");
		m_Materials.resize(mtl_root->IntAttribute("Count"));

		for (tinyxml2::XMLElement* mtl_doc = mtl_root->FirstChildElement("Material"); mtl_doc; mtl_doc = mtl_doc->NextSiblingElement("Material"))
		{
			auto material = CreateSharedPtr<Material>();

			/* ID */
			int id = mtl_doc->IntAttribute("ID");
			ASSERT(id >= 0 && id < m_Materials.size());

			/* Shader */
			material->SetShader(ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH(mtl_doc->Attribute("ShaderPath"))));

			/* Parameters */
			const auto params_root = mtl_doc->FirstChildElement("Parameters");
			for (tinyxml2::XMLElement* param_doc = params_root->FirstChildElement("ParamInfo"); param_doc; param_doc = param_doc->NextSiblingElement("ParamInfo"))
			{
				auto param_type = static_cast<ParamType>(param_doc->IntAttribute("Type"));
				auto param_name = param_doc->Attribute("Name");

				switch (param_type) {
				case ParamType::Texture:
					{
						const auto texture_doc = param_doc->FirstChildElement("Texture");
						auto texture_path = texture_doc->Attribute("Path");

						TextureLoadConfig load_config;
						const auto load_config_doc = texture_doc->FirstChildElement("LoadConfig");
						load_config.IsFlipV = load_config_doc->BoolAttribute("IsFlipV");
						load_config.IsGenMips = load_config_doc->BoolAttribute("IsGenMips");
						load_config.SamplerType = static_cast<SamplerType>(load_config_doc->IntAttribute("SamplerType"));

						auto texture = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(texture_path), load_config);
						material->SetTexture(param_name, texture);
					}
					break;
				case ParamType::Int:
					material->SetParameters(ParamType::Int, param_name, param_doc->IntAttribute("Value"));
					break;
				case ParamType::Float: 
					material->SetParameters(ParamType::Float, param_name, param_doc->FloatAttribute("Value"));
					break;
				case ParamType::Vec2: 
					material->SetParameters(ParamType::Vec2, param_name, ToVec2(param_doc->Attribute("Value")));
					break;
				case ParamType::Vec3: 
					material->SetParameters(ParamType::Vec3, param_name, ToVec3(param_doc->Attribute("Value")));
					break;
				case ParamType::Vec4: 
					material->SetParameters(ParamType::Vec4, param_name, ToVec4(param_doc->Attribute("Value")));
					break;
				}

				/* RasterState */
				RenderRasterState raster_state;
				const auto raster_state_doc = mtl_doc->FirstChildElement("RasterState");
				raster_state.CullMode = static_cast<CullMode>(raster_state_doc->IntAttribute("CullMode"));
				raster_state.FrontFaceType = static_cast<FrontFaceType>(raster_state_doc->IntAttribute("FrontFaceType"));
				raster_state.EnableBlend = raster_state_doc->BoolAttribute("EnableBlend");
				raster_state.BlendEquationRGB = static_cast<BlendEquation>(raster_state_doc->IntAttribute("BlendEquationRGB"));
				raster_state.BlendEquationA = static_cast<BlendEquation>(raster_state_doc->IntAttribute("BlendEquationA"));
				raster_state.BlendFuncSrcRGB = static_cast<BlendFunc>(raster_state_doc->IntAttribute("BlendFuncSrcRGB"));
				raster_state.BlendFuncSrcA = static_cast<BlendFunc>(raster_state_doc->IntAttribute("BlendFuncSrcA"));
				raster_state.BlendFuncDstRGB = static_cast<BlendFunc>(raster_state_doc->IntAttribute("BlendFuncDstRGB"));
				raster_state.BlendFuncDstA = static_cast<BlendFunc>(raster_state_doc->IntAttribute("BlendFuncDstA"));
				raster_state.EnableDepthWrite = raster_state_doc->BoolAttribute("EnableDepthWrite");
				raster_state.DepthCompareFunc = static_cast<CompareFunc>(raster_state_doc->IntAttribute("DepthCompareFunc"));
				raster_state.EnableColorWrite = raster_state_doc->BoolAttribute("EnableColorWrite");
				material->SetRasterState(raster_state);
			}

			m_Materials[id] = material;
		}

		delete in_mtl_file;
		return true;
	}
}
