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
	/* 内置白模材质 */
	SharedPtr<Material> Material::m_pBuiltinWhiteMaterial = CreateSharedPtr<Material>();

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

	bool Material::IsSkyBox() const
	{
		PROFILE_FUNCTION();

		/* 天空盒的识别以着色器为单一来源（样例场景的约定同源）：资产里的天空
		 * 就是"网格 + SkyBox.glsl 材质"，由顶点着色器折叠成全屏天空。
		 * ShaderAssetManager 按路径缓存，指针比较即可。 */
		static const SharedPtr<DeviceShader> s_skybox_shader =
			ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/SkyBox.glsl"));
		return m_pShader != nullptr && m_pShader == s_skybox_shader;
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

	/* 应用单个参数（Bind 与 per-draw 覆盖共用）。
	 * Texture 的绑定点：值里带着的 slot 无效时按 Shader 反射重新解析 ——
	 * per-draw 参数（见 MakeTextureParam）正是靠这条在应用时解析出正确的 binding。 */
	void Material::ApplyParam(const MaterialParamInfo& param_info)
	{
		const auto& value = param_info.Value;
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
				uint32_t slot = texture_info.second;
				if (slot == InvalidTextureSlot)
					slot = ResolveTextureBinding(param_info.Name);
				if (slot == InvalidTextureSlot)
					slot = 0u;
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

	/* 绑定材质中的各参数 */
	void Material::Bind()
	{
		PROFILE_FUNCTION();

		/* 先绑定Shader */
		m_pShader->Bind();

		/* 绑定参数 */
		for (auto& param : m_Parameters)
			ApplyParam(param.second);
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

	/* 内置白模材质 */
	SharedPtr<Material>& Material::BuiltinWhite()
	{
		if (!m_pBuiltinWhiteMaterial->GetShader())
		{
			/* 同 Default/Error：只能在获取时初始化（静态期 GL 尚未就绪）。
			 * default.glsl 是全贴图驱动的 G-Buffer 材质，9 个采样点必须全部有贴图 ——
			 * 白模用内置默认贴图把它配满，因此零外部依赖、编辑器延迟与前向路径都可用。 */
			m_pBuiltinWhiteMaterial->SetShader(
				ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/default.glsl")));

			auto& white = *m_pBuiltinWhiteMaterial;
			const auto texture_white = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/White.png"));
			const auto texture_black = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/Black.png"));
			const auto texture_normal = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/normal.png"));

			white.SetTexture("u_AlbedoTexture", texture_white);
			white.SetTexture("u_SpecularTexture", texture_black);
			white.SetTexture("u_NormalTexture", texture_normal);
			white.SetTexture("u_BumpTexture", texture_black);
			white.SetTexture("u_DisplacementTexture", texture_black);
			white.SetTexture("u_RoughnessTexture", texture_white); /* 全哑光 */
			white.SetTexture("u_MetallicTexture", texture_black);
			white.SetTexture("u_EmissiveTexture", texture_black);
			white.SetTexture("u_AmbientTexture", texture_black);
		}
		return m_pBuiltinWhiteMaterial;
	}

	/* 创建材质 */
	SharedPtr<Material> Material::Create(const SharedPtr<DeviceShader>& shader)
	{
		PROFILE_FUNCTION();

		auto material = CreateSharedPtr<Material>();
		material->SetShader(shader);

		return material;
	}

	/* 构造一个 per-draw 用的纹理参数 */
	MaterialParamInfo Material::MakeTextureParam(const std::string& name, const SharedPtr<DeviceTexture>& texture)
	{
		/* slot 填无效值：应用时（ApplyParam）按 Shader 反射解析出真正的绑定点 */
		return { ParamType::Texture, name,
			std::make_pair(texture, static_cast<uint32_t>(InvalidTextureSlot)) };
	}

	/* 克隆一个材质实例 */
	SharedPtr<Material> Material::Clone() const
	{
		PROFILE_FUNCTION();

		auto clone = CreateSharedPtr<Material>();
		clone->m_pShader = m_pShader;
		clone->m_Parameters = m_Parameters; /* 参数表逐条拷贝；纹理的 SharedPtr 共享 */
		clone->m_RasterState = m_RasterState;
		return clone;
	}

	/* 用另一份材质的内容整体覆盖本材质的定义 */
	void Material::CopyDefinitionFrom(const Material& source)
	{
		if (this == &source)
			return;

		m_pShader = source.m_pShader;
		m_Parameters = source.m_Parameters; /* 参数表逐条拷贝；纹理的 SharedPtr 共享 */
		m_RasterState = source.m_RasterState;

		/* Shader 与参数都可能换了：纹理参数的绑定点统一按新 Shader 重解析 */
		RefreshTextureBindings();
	}

	/* 只保留当前 Shader 声明的参数 */
	void Material::RetainShaderDeclaredParams()
	{
		PROFILE_FUNCTION();

		const ShaderReflectionData* reflection = (m_pShader != nullptr)
			? &m_pShader->GetReflectionData() : nullptr;
		if (reflection == nullptr || reflection->MaterialParams.empty())
			return;

		for (auto iter = m_Parameters.begin(); iter != m_Parameters.end();)
		{
			if (reflection->FindMaterialParam(iter->second.Name) != nullptr)
			{
				++iter;
				continue;
			}

			iter = m_Parameters.erase(iter);
		}
	}

	namespace
	{
		/* 无源码默认值时按类型零值兜底（与参数表同款值类型；Texture 不参与） */
		std::any ZeroValueForParamType(ParamType type)
		{
			switch (type)
			{
			case ParamType::Int: return std::any(0);
			case ParamType::Float: return std::any(0.0f);
			case ParamType::Vec2: return std::any(glm::vec2(0.0f));
			case ParamType::Vec3: return std::any(glm::vec3(0.0f));
			case ParamType::Vec4: return std::any(glm::vec4(0.0f));
			case ParamType::Mat4: return std::any(glm::mat4(0.0f));
			case ParamType::Texture: break;
			}
			return std::any{};
		}
	}

	/* 收集"Shader 已声明、本材质没有写"的值参数及其默认值 */
	std::vector<MaterialParamInfo> Material::CollectMissingValueParamDefaults() const
	{
		PROFILE_FUNCTION();

		std::vector<MaterialParamInfo> fills;
		if (m_pShader == nullptr)
			return fills;

		for (const ReflectedMaterialParam& reflected : m_pShader->GetReflectionData().MaterialParams)
		{
			if (reflected.Type == ParamType::Texture)
				continue;
			if (m_Parameters.find(ToID(reflected.Name)) != m_Parameters.end())
				continue;

			fills.emplace_back(reflected.Type, reflected.Name,
				reflected.Default.has_value()
					? reflected.Default : ZeroValueForParamType(reflected.Type));
		}
		return fills;
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

	/* 添加一个内嵌材质条目 */
	void MaterialGroup::EmplaceMaterial(const SharedPtr<Material>& material)
	{
		MaterialEntry entry;
		entry.pMaterial = material;
		m_Entries.emplace_back(std::move(entry));
	}

	/* 设置条目槽名 */
	void MaterialGroup::SetEntrySlotName(int idx, std::string name)
	{
		if (idx < 0 || idx >= static_cast<int>(m_Entries.size()))
			return;

		m_Entries[idx].SlotName = std::move(name);
	}

	/* 根据索引获取条目 */
	const MaterialEntry* MaterialGroup::GetEntryByIndex(int idx) const
	{
		if (idx < 0 || idx >= static_cast<int>(m_Entries.size()))
			return nullptr;

		return &m_Entries[idx];
	}

	/* 根据索引获取内嵌材质 */
	const SharedPtr<Material>& MaterialGroup::GetMaterialByIndex(int idx)
	{
		if (idx < 0 || idx >= static_cast<int>(m_Entries.size()))
		{
			CORE_LOG_ERROR("Unaccessable material idx, default return ErrorMaterial.");
			return Material::Error();
		}

		return m_Entries[idx].pMaterial;
	}

	/* 序列化 */
	void MaterialGroup::Serializer(const std::string& path)
	{
		m_Path = path;

		ASSERT(!m_Path.empty());
		std::filesystem::path material_path;
		if (!TryPathFromUtf8(path, material_path))
		{
			CORE_LOG_ERROR("Invalid UTF-8 material path.");
			return;
		}
		auto material_file = std::unique_ptr<FILE, decltype(&std::fclose)>(
			OpenUtf8File(material_path, "wb"), &std::fclose);
		if (!material_file)
		{
			CORE_LOG_ERROR("Failed to open material file for writing: {}.", path);
			return;
		}
		
		/* 写入材质信息 */
		auto* out_mtl_file = new tinyxml2::XMLDocument();
		out_mtl_file->InsertEndChild(out_mtl_file->NewDeclaration());
		auto* mtl_root = out_mtl_file->NewElement("Materials");
		out_mtl_file->InsertEndChild(mtl_root);
		mtl_root->SetAttribute("Count", static_cast<unsigned int>(m_Entries.size()));

		for (int i = 0; i < static_cast<int>(m_Entries.size()); ++i)
		{
			const auto& entry = m_Entries[i];
			auto* mtl_doc = mtl_root->InsertNewChildElement("Material");

			/* ID */
			mtl_doc->SetAttribute("ID", i);

			/* 槽名（可空 = 未指定） */
			if (!entry.SlotName.empty())
				mtl_doc->SetAttribute("Slot", entry.SlotName.c_str());

			/* 材质资产引用（可空 = 内嵌定义） */
			if (!entry.AssetPath.empty())
				mtl_doc->SetAttribute("Asset", RELATIVE_PATH(entry.AssetPath).c_str());

			/* 材质体：内嵌形态才有；引用形态只写路径 */
			if (entry.AssetPath.empty() && entry.pMaterial != nullptr)
			{
				MaterialIO::WriteBody(mtl_doc, *entry.pMaterial);
			}
		}

		/* 保存到文本 */
		if (out_mtl_file->SaveFile(material_file.get()) != tinyxml2::XML_SUCCESS)
			CORE_LOG_ERROR("Failed to write material file: {}.", path);
		delete out_mtl_file;
	}

	/* 反序列化 */
	bool MaterialGroup::Deserializer(const std::string& path)
	{
		PROFILE_FUNCTION();

		m_Path = ABSOLUTE_PATH(path);
		ASSERT(!m_Path.empty());

		std::filesystem::path material_path;
		if (!TryPathFromUtf8(m_Path, material_path))
		{
			CORE_LOG_ERROR("Invalid UTF-8 material path.");
			return false;
		}
		auto material_file = std::unique_ptr<FILE, decltype(&std::fclose)>(
			OpenUtf8File(material_path, "rb"), &std::fclose);
		if (!material_file)
		{
			CORE_LOG_ERROR("Failed to open material file: {}.", m_Path);
			return false;
		}

		/* 读取材质信息 */
		auto in_mtl_file = std::make_unique<tinyxml2::XMLDocument>();
		tinyxml2::XMLError error = in_mtl_file->LoadFile(material_file.get());
		if (error != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_ERROR("Failed to deserializer mtl file: {}.", m_Path);
			return false;
		}

		const tinyxml2::XMLElement* mtl_root = in_mtl_file->FirstChildElement("Materials");
		if (mtl_root == nullptr)
		{
			CORE_LOG_ERROR("Missing <Materials> root in mtl file: {}.", m_Path);
			return false;
		}

		/* 条目数取"声明值"与"实际元素数"的较大者：手工编辑过 Count 的文件也能完整读入 */
		int actual_count = 0;
		for (const auto* element = mtl_root->FirstChildElement("Material"); element; element = element->NextSiblingElement("Material"))
			++actual_count;
		m_Entries.resize(static_cast<size_t>(std::max(mtl_root->IntAttribute("Count"), actual_count)));

		for (const tinyxml2::XMLElement* mtl_doc = mtl_root->FirstChildElement("Material"); mtl_doc; mtl_doc = mtl_doc->NextSiblingElement("Material"))
		{
			/* ID */
			const int id = mtl_doc->IntAttribute("ID");
			ASSERT(id >= 0 && id < static_cast<int>(m_Entries.size()));
			if (id < 0 || id >= static_cast<int>(m_Entries.size()))
				continue;

			MaterialEntry entry;

			/* 槽名（老资产没有 = 未指定） */
			if (const char* slot = mtl_doc->Attribute("Slot"))
				entry.SlotName = slot;

			/* 材质资产引用（解析推迟到使用方：模型加载链 / 场景覆盖） */
			if (const char* asset = mtl_doc->Attribute("Asset"))
			{
				entry.AssetPath = ABSOLUTE_PATH(asset);
			}
			else
			{
				auto material = CreateSharedPtr<Material>();
				if (MaterialIO::ReadBody(mtl_doc, *material))
					entry.pMaterial = std::move(material);
			}

			m_Entries[id] = std::move(entry);
		}

		return true;
	}

	/* ---- 材质体读写：.mtl 条目、独立材质资产、实例覆盖共用 ---- */
	namespace MaterialIO
	{
		void WriteBody(tinyxml2::XMLElement* material_element, const Material& material)
		{
			/* Shader */
			const auto& shader = material.GetShader();
			material_element->SetAttribute("ShaderPath",
				shader != nullptr ? RELATIVE_PATH(shader->GetPath()).c_str() : "");

			/* Parameters */
			auto* params_root = material_element->InsertNewChildElement("Parameters");
			const auto& parameters = material.GetAllParameters();
			for (const auto& param : parameters)
			{
				const auto param_info = param.second;
				auto* param_doc = params_root->InsertNewChildElement("ParamInfo");
				param_doc->SetAttribute("Type", static_cast<uint32_t>(param_info.Type));
				param_doc->SetAttribute("Name", param_info.Name.c_str());
				switch (param_info.Type)
				{
				case ParamType::Texture:
					{
						auto texture_doc = param_doc->InsertNewChildElement("Texture");
						const auto texture_info = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(param_info.Value);
						const auto& texture = texture_info.first;

						texture_doc->SetAttribute("Path", RELATIVE_PATH(texture->GetPath()).c_str());
						texture_doc->SetAttribute("Slot", texture_info.second);

						auto* load_config_doc = texture_doc->InsertNewChildElement("LoadConfig");
						const auto load_config = texture->GetTextureLoadConfig();
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
			const auto raster_state = material.GetRasterState();
			auto* raster_state_doc = material_element->InsertNewChildElement("RasterState");
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

		bool ReadBody(const tinyxml2::XMLElement* material_element, Material& material)
		{
			if (material_element == nullptr)
				return false;

			/* Shader */
			const char* shader_path = material_element->Attribute("ShaderPath");
			if (shader_path == nullptr || *shader_path == '\0')
				return false;
			material.SetShader(ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH(shader_path)));

			/* Parameters */
			if (const auto* params_root = material_element->FirstChildElement("Parameters"))
			{
				for (const tinyxml2::XMLElement* param_doc = params_root->FirstChildElement("ParamInfo"); param_doc; param_doc = param_doc->NextSiblingElement("ParamInfo"))
				{
					const auto param_type = static_cast<ParamType>(param_doc->IntAttribute("Type"));
					const char* param_name = param_doc->Attribute("Name");

					switch (param_type)
					{
					case ParamType::Texture:
						{
							const auto* texture_doc = param_doc->FirstChildElement("Texture");
							if (texture_doc == nullptr)
								break;
							const char* texture_path = texture_doc->Attribute("Path");

							TextureLoadConfig load_config;
							if (const auto* load_config_doc = texture_doc->FirstChildElement("LoadConfig"))
							{
								load_config.IsFlipV = load_config_doc->BoolAttribute("IsFlipV");
								load_config.IsGenMips = load_config_doc->BoolAttribute("IsGenMips");
								load_config.SamplerType = static_cast<SamplerType>(load_config_doc->IntAttribute("SamplerType"));
							}

							auto texture = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(texture_path), load_config);
							material.SetTexture(param_name, texture);
						}
						break;
					case ParamType::Int:
						material.SetParameters(ParamType::Int, param_name, param_doc->IntAttribute("Value"));
						break;
					case ParamType::Float:
						material.SetParameters(ParamType::Float, param_name, param_doc->FloatAttribute("Value"));
						break;
					case ParamType::Vec2:
						material.SetParameters(ParamType::Vec2, param_name, ToVec2(param_doc->Attribute("Value")));
						break;
					case ParamType::Vec3:
						material.SetParameters(ParamType::Vec3, param_name, ToVec3(param_doc->Attribute("Value")));
						break;
					case ParamType::Vec4:
						material.SetParameters(ParamType::Vec4, param_name, ToVec4(param_doc->Attribute("Value")));
						break;
					}
				}
			}

			/* RasterState：独立元素；老资产可能没有 —— 缺失时保持默认值 */
			if (const auto* raster_state_doc = material_element->FirstChildElement("RasterState"))
			{
				RenderRasterState raster_state;
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
				material.SetRasterState(raster_state);
			}

			return true;
		}
	}
}
