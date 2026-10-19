#include "Pch.h"
#include "Material.h"
#include <tinyxml2.h>

#include "SceneCommon.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/VirtualDevice/DeviceShader.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

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
		 * AssetManager 按路径缓存，指针比较即可。 */
		static const SharedPtr<DeviceShader> s_skybox_shader =
			AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH("Shaders/Sky/SkyBox.glsl"));
		return m_pShader != nullptr && m_pShader == s_skybox_shader;
	}

	bool Material::SupportsIBL() const
	{
		PROFILE_FUNCTION();

		/* 判据 = 声明了 IBL 契约的辐照度采样器（只能按 sampler 反射来查）；不能查 u_UseIBL ——
		 * 裸 uniform 在 Metal 反射里查不到，拿它做门控会恒为假、per-draw 的探针绑定永远发不出去。 */
		return m_pShader != nullptr && m_pShader->GetUniformBinding("u_IrradianceMap") >= 0;
	}

	bool Material::SupportsDirectLighting() const
	{
		PROFILE_FUNCTION();

		if (m_pShader == nullptr)
			return false;

		/* 判据 = 声明了直接光照契约的 u_ComposeAmbientEmission（Unlit 类不声明）。反射表为空 =
		 * 后端拿不到裸 uniform 的声明（GL 的 SPIR-V 反射路径），就退回 IBL 契约近似判断 —— 完整
		 * 光照的着色器都会声明 IBL 采样器。 */
		const auto& reflection = m_pShader->GetReflectionData();
		if (!reflection.MaterialParams.empty())
			return reflection.FindMaterialParam("u_ComposeAmbientEmission") != nullptr;

		return SupportsIBL();
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
			m_pDefaultMaterial->SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH("Shaders/MaterialShaders/PBRStandard.glsl")));
		}

		return m_pDefaultMaterial;
	}

	/* 错误材质 */
	SharedPtr<Material>& Material::Error()
	{
		if (!m_pErrorMaterial->GetShader())
		{
			m_pErrorMaterial->SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH("Shaders/Builtin/Error.glsl")));
		}
		return m_pErrorMaterial;
	}

	/* 内置白模材质 */
	SharedPtr<Material>& Material::BuiltinWhite()
	{
		if (!m_pBuiltinWhiteMaterial->GetShader())
		{
			/* 跟 Default/Error 一样：只能在获取时初始化（静态期 GL 还没就绪）。着色器用
			 * PBRStandard（场景物体的通用默认外观：前向直画，延迟侧由 G-Buffer Pass 自动转换），
			 * 9 个采样点必须全有贴图 —— 白模就用内置默认贴图配满，零外部依赖。 */
			m_pBuiltinWhiteMaterial->SetShader(
				AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH("Shaders/MaterialShaders/PBRStandard.glsl")));

			auto& white = *m_pBuiltinWhiteMaterial;
			const auto texture_white = AssetManager::Instance().GetOrLoad<DeviceTexture>(ABSOLUTE_PATH("Textures/White.png"));
			const auto texture_black = AssetManager::Instance().GetOrLoad<DeviceTexture>(ABSOLUTE_PATH("Textures/Black.png"));
			/* u_NormalTexture 的缺省平面法线：normal.png 必须存 (128,128,255)——采样经 *2-1
			 * 解码后为 (0,0,1)（切空间朝上、不加扰动）。该文件是所有"缺省补图"（本处、
			 * DeferredPasses 的 DefaultTextureForSampler、新建材质模板）共用的单一来源。 */
			const auto texture_normal = AssetManager::Instance().GetOrLoad<DeviceTexture>(ABSOLUTE_PATH("Textures/normal.png"));

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

	void Material::MakeIBLParamOverrides(std::vector<MaterialParamInfo>& overrides,
		const SharedPtr<DeviceTexture>& irradiance_map,
		const SharedPtr<DeviceTexture>& prefilter_map,
		const SharedPtr<DeviceTexture>& brdf_lut)
	{
		/* 三张图齐备才启用（与探针"可用"的判据同源：不齐视为无 IBL） */
		const bool ibl_ready = irradiance_map != nullptr && prefilter_map != nullptr && brdf_lut != nullptr;

		overrides.emplace_back(ParamType::Int, "u_UseIBL", ibl_ready ? 1 : 0);
		overrides.emplace_back(MakeTextureParam("u_IrradianceMap",
			ibl_ready ? irradiance_map : DeviceTexture::BlackCube()));
		overrides.emplace_back(MakeTextureParam("u_PrefilterMap",
			ibl_ready ? prefilter_map : DeviceTexture::BlackCube()));
		overrides.emplace_back(MakeTextureParam("u_BRDFLut",
			ibl_ready ? brdf_lut : DeviceTexture::Black()));
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
			const ReflectedMaterialParam* declared = reflection->FindMaterialParam(iter->second.Name);
			if (declared != nullptr && declared->Property)
			{
				++iter;
				continue;
			}

			/* 没标 @property 的也不留：引擎逐绘制给的值存进材质只会过期 */
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

	/* ---- 材质体读写：.mtl 条目、独立材质资产、实例覆盖共用 ---- */
	namespace MaterialIO
	{
		namespace
		{
			/* 按扩展名识别 HDR 贴图：老资产的 LoadConfig 没有 IsHdr 字段，
			 * 缺省按扩展名推断（.hdr 用浮点解码；按 8bit 读会先转 LDR 再上传，
			 * 既慢又丢动态范围）。 */
			[[nodiscard]] bool IsHdrTexturePath(const char* path)
			{
				if (path == nullptr)
					return false;

				std::string suffix = ExtractFileSuffix(path);
				std::transform(suffix.begin(), suffix.end(), suffix.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return suffix == "hdr";
			}
		}

		void WriteBody(tinyxml2::XMLElement* material_element, const Material& material, bool write_raster_state)
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
						load_config_doc->SetAttribute("IsHdr", load_config.IsHdr);
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

			/* RasterState（参数覆盖可以不带，沿用基材质） */
			if (!write_raster_state)
				return;

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
			material.SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH(shader_path)));

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
								load_config.IsHdr = (load_config_doc->FindAttribute("IsHdr") != nullptr)
									? load_config_doc->BoolAttribute("IsHdr")
									: IsHdrTexturePath(texture_path);
							}
							else
							{
								load_config.IsHdr = IsHdrTexturePath(texture_path);
							}

							auto texture = AssetManager::Instance().GetOrLoad<DeviceTexture>(ABSOLUTE_PATH(texture_path), load_config);
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

		bool ReadFile(const std::string& path, Material& material)
		{
			const std::string absolute = ABSOLUTE_PATH(path);
			std::ifstream stream(PathFromUtf8(absolute), std::ios::binary);
			if (!stream)
			{
				CORE_LOG_ERROR("Failed to open material file: {}.", absolute);
				return false;
			}

			const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			tinyxml2::XMLDocument document;
			if (document.Parse(text.c_str()) != tinyxml2::XML_SUCCESS)
			{
				CORE_LOG_ERROR("Bad material file: {}.", absolute);
				return false;
			}

			const tinyxml2::XMLElement* element = document.FirstChildElement("Material");
			if (element == nullptr)
			{
				CORE_LOG_ERROR("Material file has no <Material>: {}.", absolute);
				return false;
			}

			return ReadBody(element, material);
		}

		bool WriteFile(const std::string& path, const Material& material)
		{
			const std::string absolute = ABSOLUTE_PATH(path);
			const std::filesystem::path file_path = PathFromUtf8(absolute);

			std::error_code error;
			if (!file_path.parent_path().empty())
				std::filesystem::create_directories(file_path.parent_path(), error);

			tinyxml2::XMLDocument document;
			document.InsertEndChild(document.NewDeclaration());
			auto* root = document.NewElement("Material");
			document.InsertEndChild(root);
			WriteBody(root, material);

			/* 文件句柄走 UTF-8 路径的打开方式（Windows 上 char* 路径会丢非 ASCII） */
			auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(
				OpenUtf8File(file_path, "wb"), &std::fclose);
			if (!file)
			{
				CORE_LOG_ERROR("Failed to open material file for writing: {}.", absolute);
				return false;
			}

			if (document.SaveFile(file.get()) != tinyxml2::XML_SUCCESS)
			{
				CORE_LOG_ERROR("Failed to write material file: {}.", absolute);
				return false;
			}
			return true;
		}

		bool MakeTexturesLocal(const std::string& material_path, const std::string& texture_dir)
		{
			const std::string absolute = ABSOLUTE_PATH(material_path);
			const std::filesystem::path target_dir = PathFromUtf8(texture_dir);
			if (target_dir.empty())
				return false;

			std::ifstream stream(PathFromUtf8(absolute), std::ios::binary);
			if (!stream)
			{
				CORE_LOG_ERROR("Failed to open material file: {}.", absolute);
				return false;
			}

			const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			tinyxml2::XMLDocument document;
			if (document.Parse(text.c_str()) != tinyxml2::XML_SUCCESS)
			{
				CORE_LOG_ERROR("Bad material file: {}.", absolute);
				return false;
			}

			std::error_code error;
			std::filesystem::create_directories(target_dir, error);

			/* 贴图挂在 <ParamInfo> 下面：整棵树找 <Texture>，逐条搬 */
			const auto relocate = [&](auto&& self, tinyxml2::XMLElement* element) -> void
			{
				if (std::strcmp(element->Name(), "Texture") == 0)
				{
					const char* source_attribute = element->Attribute("Path");
					if (source_attribute != nullptr && *source_attribute != '\0')
					{
						const std::filesystem::path source = ResolveAssetPath(PathFromUtf8(source_attribute));
						if (std::filesystem::is_regular_file(source, error))
						{
							const std::filesystem::path destination = target_dir / source.filename();
							std::error_code equivalent_error;
							const bool same_file = std::filesystem::exists(destination, equivalent_error)
								&& std::filesystem::equivalent(source, destination, equivalent_error);

							bool copied = true;
							if (!same_file)
							{
								std::filesystem::copy_file(source, destination,
									std::filesystem::copy_options::overwrite_existing, error);
								copied = !error;
								if (error)
									CORE_LOG_WARN("Failed to copy texture {}: {}.", PathToUtf8(source), error.message());
							}

							if (copied)
							{
								element->SetAttribute("Path",
									RELATIVE_PATH(PathToUtf8(destination)).c_str());
							}
						}
					}
				}

				for (tinyxml2::XMLElement* child = element->FirstChildElement();
					child != nullptr; child = child->NextSiblingElement())
				{
					self(self, child);
				}
			};

			for (tinyxml2::XMLElement* element = document.FirstChildElement();
				element != nullptr; element = element->NextSiblingElement())
			{
				relocate(relocate, element);
			}

			auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(
				OpenUtf8File(PathFromUtf8(absolute), "wb"), &std::fclose);
			if (!file)
			{
				CORE_LOG_ERROR("Failed to open material file for writing: {}.", absolute);
				return false;
			}

			return document.SaveFile(file.get()) == tinyxml2::XML_SUCCESS;
		}
	}
}
