#include "Pch.h"
#include "Material.h"
#include <tinyxml2.h>

#include "SceneCommon.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Core/TextureSettings.h"
#include "Helios/VirtualDevice/DeviceShader.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include <cctype>
#include <cstdlib>
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
		++m_Revision;
		++m_LayoutRevision;
		/* Shader更换，binding布局也随之改变，刷新Shader的反射结果 */
		RefreshTextureBindings();
		m_ShaderGeneration = shader != nullptr ? shader->GetGeneration() : 0;
	}

	/* 能力位跟 Shader 的版本号绑定：热重载换代后重算一次 */
	bool Material::ShaderFlagsReady() const
	{
		return m_HasShaderFlags
			&& m_FlagsGeneration == (m_pShader != nullptr ? m_pShader->GetGeneration() : 0);
	}

	/* 能力位一次算清：这几个判据每帧按对象查成千上万次，不能每次都翻反射表 */
	void Material::RefreshShaderFlags() const
	{
		m_HasShaderFlags = true;
		m_FlagsGeneration = m_pShader != nullptr ? m_pShader->GetGeneration() : 0;
		m_IsSkyBox = false;
		m_SupportsIBL = false;
		m_SupportsDirectLighting = false;
		m_SupportsGBuffer = false;
		m_SupportsDebugView = false;
		if (m_pShader == nullptr)
			return;

		/* 天空盒以着色器为单一来源（样例场景的约定同源）：资产里的天空就是
		 * "网格 + SkyBox.glsl 材质"，由顶点着色器折叠成全屏天空。
		 * 大气散射那套（Atmosphere.glsl）走同一个约定，顶点折叠与深度写法一致，
		 * 只是片元改成现算 —— 所以两种都算天空材质。 */
		static BuiltinAsset<DeviceShader> s_skybox_shader("Shaders/Sky/SkyBox.glsl");
		static BuiltinAsset<DeviceShader> s_atmosphere_shader("Shaders/Sky/Atmosphere.glsl");
		m_IsSkyBox = m_pShader == s_skybox_shader.Get() || m_pShader == s_atmosphere_shader.Get();

		/* IBL 判据 = 声明了 IBL 契约的辐照度采样器（只能按 sampler 反射查）：裸 uniform
		 * u_UseIBL 在 Metal 反射里查不到，拿它门控会恒为假 */
		m_SupportsIBL = m_pShader->GetUniformBinding("u_IrradianceMap") >= 0;

		/* 直接光照判据 = 声明了 u_ComposeAmbientEmission（Unlit 类不声明）。反射表为空 =
		 * 后端拿不到裸 uniform 声明（GL 的 SPIR-V 反射路径），退回 IBL 契约近似判断 */
		const auto& reflection = m_pShader->GetReflectionData();
		m_SupportsDirectLighting = !reflection.MaterialParams.empty()
			? reflection.FindMaterialParam("u_ComposeAmbientEmission") != nullptr
			: m_SupportsIBL;

		/* G-Buffer 判据 = 声明了标准材质契约的主采样器：自定义着色器的裸 uniform 转不过去 */
		m_SupportsGBuffer = m_pShader->GetUniformBinding("u_AlbedoTexture") >= 0;

		/* 调试分流判据 = 声明了 u_DebugView；拿不到裸 uniform 声明时按光照契约近似 */
		m_SupportsDebugView = !reflection.MaterialParams.empty()
			? reflection.FindMaterialParam("u_DebugView") != nullptr
			: m_SupportsDirectLighting;
	}

	bool Material::IsSkyBox() const
	{
		if (!ShaderFlagsReady())
			RefreshShaderFlags();
		return m_IsSkyBox;
	}

	bool Material::SupportsIBL() const
	{
		if (!ShaderFlagsReady())
			RefreshShaderFlags();
		return m_SupportsIBL;
	}

	bool Material::SupportsDirectLighting() const
	{
		if (!ShaderFlagsReady())
			RefreshShaderFlags();
		return m_SupportsDirectLighting;
	}

	bool Material::SupportsGBuffer() const
	{
		if (!ShaderFlagsReady())
			RefreshShaderFlags();
		return m_SupportsGBuffer;
	}

	bool Material::SupportsDebugView() const
	{
		if (!ShaderFlagsReady())
			RefreshShaderFlags();
		return m_SupportsDebugView;
	}

	void Material::SetParameters(ParamType type, const std::string& name, const std::any& param)
	{
		PROFILE_FUNCTION();

		if (type == ParamType::Texture)
		{
			const auto texture = std::any_cast<const SharedPtr<DeviceTexture>>(param);
			if (!texture)
				return;
			StoreParam({ ParamType::Texture, name, std::make_pair(texture, ResolveTextureBinding(name)) });
			return;
		}

		StoreParam({ type, name, param });
	}

	void Material::SetTexture(const std::string& name, const SharedPtr<DeviceTexture>& texture)
	{
		PROFILE_FUNCTION();

		if (!texture)
			return;

		StoreParam({ ParamType::Texture, name, std::make_pair(texture, ResolveTextureBinding(name)) });
	}

	/* 只改值不动绑定表：表里存的是参数节点的地址，值在绘制时现读 */
	void Material::StoreParam(MaterialParamInfo info)
	{
		const auto [iter, inserted] = m_Parameters.try_emplace(ToID(info.Name));
		if (inserted || iter->second.Type != info.Type)
			++m_LayoutRevision;
		iter->second = std::move(info);
		++m_Revision;
	}

	namespace
	{
		/* 数值参数的原始字节；值的类型和声明对不上时返回空（跳过不写） */
		const void* ParamValueData(const MaterialParamInfo& info)
		{
			switch (info.Type)
			{
			case ParamType::Int: return std::any_cast<int>(&info.Value);
			case ParamType::Float: return std::any_cast<float>(&info.Value);
			case ParamType::Vec2: return std::any_cast<glm::vec2>(&info.Value);
			case ParamType::Vec3: return std::any_cast<glm::vec3>(&info.Value);
			case ParamType::Vec4: return std::any_cast<glm::vec4>(&info.Value);
			case ParamType::Mat4: return std::any_cast<glm::mat4>(&info.Value);
			case ParamType::Texture: break;
			}
			return nullptr;
		}

		const std::pair<SharedPtr<DeviceTexture>, uint32_t>* TextureValue(const MaterialParamInfo& info)
		{
			return std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(&info.Value);
		}

		/* 贴图缺失或加载失败时改绑默认贴图：空槽位 Metal 校验层会直接断言 */
		bool BindTextureParam(const MaterialParamInfo& info, uint32_t slot)
		{
			const auto* texture_info = TextureValue(info);
			DeviceTexture* texture = texture_info != nullptr ? texture_info->first.get() : nullptr;
			SharedPtr<DeviceTexture> fallback;
			if (texture == nullptr || !texture->IsLoaded())
			{
				fallback = Material::DefaultTextureForSampler(info.Name);
				texture = fallback.get();
			}
			if (texture == nullptr)
				return false;

			texture->Bind(slot);
			return true;
		}
	}

	/* 应用单个参数（per-draw 覆盖走这里，按名字写）。
	 * Texture 的绑定点：值里带着的 slot 无效时按 Shader 反射重新解析 ——
	 * per-draw 参数（见 MakeTextureParam）正是靠这条在应用时解析出正确的 binding。 */
	void Material::ApplyParam(const MaterialParamInfo& param_info)
	{
		if (param_info.Type == ParamType::Texture)
		{
			/* 反射不到绑定点时回退到槽位 0，绝不静默跳过绑定：
			 * 若管线要求采样器/纹理而未被绑定，Metal 校验层会直接断言终止。 */
			const auto* texture_info = TextureValue(param_info);
			uint32_t slot = texture_info != nullptr ? texture_info->second : InvalidTextureSlot;
			if (slot == InvalidTextureSlot)
				slot = ResolveTextureBinding(param_info.Name);
			if (slot == InvalidTextureSlot)
				slot = 0u;
			if (BindTextureParam(param_info, slot))
				m_pShader->SetInt(param_info.Name, static_cast<int>(slot));
			return;
		}

		const void* data = ParamValueData(param_info);
		if (data == nullptr)
			return;

		switch (param_info.Type)
		{
		case ParamType::Int: m_pShader->SetInt(param_info.Name, *static_cast<const int*>(data)); break;
		case ParamType::Float: m_pShader->SetFloat(param_info.Name, *static_cast<const float*>(data)); break;
		case ParamType::Vec2: m_pShader->SetFloat2(param_info.Name, *static_cast<const glm::vec2*>(data)); break;
		case ParamType::Vec3: m_pShader->SetFloat3(param_info.Name, *static_cast<const glm::vec3*>(data)); break;
		case ParamType::Vec4: m_pShader->SetFloat4(param_info.Name, *static_cast<const glm::vec4*>(data)); break;
		case ParamType::Mat4: m_pShader->SetMat4(param_info.Name, *static_cast<const glm::mat4*>(data)); break;
		case ParamType::Texture: break;
		}
	}

	Material::BoundParam Material::BindParam(const MaterialParamInfo& info) const
	{
		BoundParam bound;
		bound.Info = &info;
		bound.Handle = m_pShader->ResolveParamHandle(info.Name);
		if (info.Type == ParamType::Texture)
		{
			const auto* texture_info = TextureValue(info);
			uint32_t slot = texture_info != nullptr ? texture_info->second : InvalidTextureSlot;
			if (slot == InvalidTextureSlot)
				slot = ResolveTextureBinding(info.Name);
			bound.Slot = slot == InvalidTextureSlot ? 0u : slot;
		}
		return bound;
	}

	void Material::ApplyBound(const BoundParam& bound)
	{
		const MaterialParamInfo& info = *bound.Info;
		if (info.Type == ParamType::Texture)
		{
			if (BindTextureParam(info, bound.Slot) && bound.Handle >= 0)
			{
				const int slot = static_cast<int>(bound.Slot);
				m_pShader->SetParam(bound.Handle, ParamType::Int, &slot);
			}
			return;
		}

		if (bound.Handle < 0)
			return;
		if (const void* data = ParamValueData(info))
			m_pShader->SetParam(bound.Handle, info.Type, data);
	}

	void Material::RebuildBindTable()
	{
		BindCache& cache = m_BindCache;
		cache.Revision = m_LayoutRevision;
		cache.Shader = m_pShader.get();
		cache.Generation = m_pShader->GetGeneration();

		cache.Params.clear();
		cache.Params.reserve(m_Parameters.size());
		for (const auto& [id, info] : m_Parameters)
			cache.Params.push_back(BindParam(info));

		cache.FillsBuilt = MaterialFill::None;
		cache.ValueFills.clear();
		cache.TextureFills.clear();
		cache.BoundValueFills.clear();
		cache.BoundTextureFills.clear();
	}

	/* 绑定材质中的各参数 */
	void Material::Bind(uint8_t fill_missing)
	{
		PROFILE_FUNCTION();

		/* Shader 重载换代后反射可能变了（加一个采样器，后面的槽位整体位移），
		 * 这里比一次版本号，变了就重解析贴图绑定点。O(1)，不必遍历材质表 */
		if (m_pShader->GetGeneration() != m_ShaderGeneration)
		{
			m_ShaderGeneration = m_pShader->GetGeneration();
			RefreshTextureBindings();
		}

		/* 先绑定Shader：GL 第一次绑定才 link，句柄要在这之后解析 */
		m_pShader->Bind();

		BindCache& cache = m_BindCache;
		if (cache.Revision != m_LayoutRevision || cache.Shader != m_pShader.get()
			|| cache.Generation != m_pShader->GetGeneration())
			RebuildBindTable();

		for (const BoundParam& bound : cache.Params)
			ApplyBound(bound);

		/* 补齐项第一次用到时才收集，之后跟着绑定表一起作废 */
		const auto apply_fills = [this, &cache](uint8_t kind, std::vector<MaterialParamInfo>& fills,
			std::vector<BoundParam>& bound_fills)
		{
			if ((cache.FillsBuilt & kind) == 0)
			{
				fills = kind == MaterialFill::Values
					? CollectMissingValueParamDefaults() : CollectMissingTextureParamDefaults();
				bound_fills.clear();
				bound_fills.reserve(fills.size());
				for (const MaterialParamInfo& fill : fills)
					bound_fills.push_back(BindParam(fill));
				cache.FillsBuilt |= kind;
			}
			for (const BoundParam& bound : bound_fills)
				ApplyBound(bound);
		};
		if ((fill_missing & MaterialFill::Textures) != 0)
			apply_fills(MaterialFill::Textures, cache.TextureFills, cache.BoundTextureFills);
		if ((fill_missing & MaterialFill::Values) != 0)
			apply_fills(MaterialFill::Values, cache.ValueFills, cache.BoundValueFills);
	}

	SharedPtr<Material> Material::GetDerived(const SharedPtr<DeviceShader>& target, DeriveFunc build)
	{
		BindCache& cache = m_BindCache;
		const uint32_t generation = target != nullptr ? target->GetGeneration() : 0;
		const bool same_target = !cache.DerivedTarget.owner_before(target) && !target.owner_before(cache.DerivedTarget);
		if (!same_target || cache.DerivedTarget.expired() || cache.DerivedRevision != m_Revision
			|| cache.DerivedGeneration != generation)
		{
			cache.Derived = build(*this, target);
			cache.DerivedTarget = target;
			cache.DerivedRevision = m_Revision;
			cache.DerivedGeneration = generation;
		}
		return cache.Derived;
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
		clone->m_RenderQueue = m_RenderQueue;
		/* 槽位是按这个版本号解出来的，一起带过去，首次 Bind 不必再解一遍 */
		clone->m_ShaderGeneration = m_ShaderGeneration;
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
		m_RenderQueue = source.m_RenderQueue;
		++m_Revision;
		++m_LayoutRevision;

		/* Shader 与参数都可能换了：纹理参数的绑定点统一按新 Shader 重解析 */
		RefreshTextureBindings();
		m_ShaderGeneration = m_pShader != nullptr ? m_pShader->GetGeneration() : 0;
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
		++m_Revision;
		++m_LayoutRevision;
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

	/* 收集"Shader 已声明、本材质没有写"的贴图参数及其默认贴图 */
	std::vector<MaterialParamInfo> Material::CollectMissingTextureParamDefaults() const
	{
		PROFILE_FUNCTION();

		std::vector<MaterialParamInfo> fills;
		if (m_pShader == nullptr)
			return fills;

		for (const ReflectedMaterialParam& reflected : m_pShader->GetReflectionData().MaterialParams)
		{
			/* 只补材质该管的采样器；引擎逐绘制绑的（阴影图 / 探针贴图）不拦 */
			if (reflected.Type != ParamType::Texture || !reflected.Property)
				continue;
			if (m_Parameters.find(ToID(reflected.Name)) != m_Parameters.end())
				continue;

			fills.push_back(MakeTextureParam(reflected.Name, DefaultTextureForSampler(reflected.Name)));
		}
		return fills;
	}

	/* 材质侧的变体取值：贴图看有没有设、数值看是否非零，都归一成 0 / 1；
	 * 外部维度不在材质里取，留给绘制端补。 */
	ShaderVariantDefines Material::CollectVariantDefines() const
	{
		PROFILE_FUNCTION();

		ShaderVariantDefines defines;
		if (m_pShader == nullptr)
			return defines;

		for (const ShaderVariantSpec& spec : m_pShader->GetReflectionData().VariantSpecs)
		{
			if (spec.Source != ShaderVariantSource::Param || spec.ParamName.empty())
				continue;

			const auto it = m_Parameters.find(ToID(spec.ParamName));
			if (it == m_Parameters.end())
			{
				defines[spec.Name] = 0;
				continue;
			}

			const MaterialParamInfo& param = it->second;
			int value = 0;
			if (param.Type == ParamType::Texture)
			{
				const auto texture =
					std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(&param.Value);
				value = (texture != nullptr && texture->first != nullptr && texture->first->IsLoaded())
					? 1 : 0;
			}
			else if (param.Type == ParamType::Int)
			{
				const int* number = std::any_cast<int>(&param.Value);
				value = (number != nullptr && *number != 0) ? 1 : 0;
			}
			else if (param.Type == ParamType::Float)
			{
				const float* number = std::any_cast<float>(&param.Value);
				value = (number != nullptr && *number != 0.0f) ? 1 : 0;
			}
			defines[spec.Name] = value;
		}
		return defines;
	}

	SharedPtr<DeviceTexture> Material::DefaultTextureForSampler(const std::string& sampler_name)
	{
		PROFILE_FUNCTION();

		/* 注意：Ambient 不能用黑 —— 缺贴图的材质走 "albedo × 0.3" 回退环境项，
		 * 黑贴图会让延迟端在没有探针时把这个环境项丢掉。 */
		static BuiltinAsset<DeviceTexture> s_normal("Textures/normal.png");
		static BuiltinAsset<DeviceTexture> s_white("Textures/White.png");
		static BuiltinAsset<DeviceTexture> s_black("Textures/Black.png");
		if (sampler_name.find("Normal") != std::string::npos)
			return s_normal.Get();
		if (sampler_name.find("Albedo") != std::string::npos
			|| sampler_name.find("Roughness") != std::string::npos
			|| sampler_name.find("Ambient") != std::string::npos)
			return s_white.Get();
		return s_black.Get();
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
		++m_Revision;
		++m_LayoutRevision;
	}

	/* ---- 材质体读写：.mtl 条目、独立材质资产、实例覆盖共用 ---- */
	namespace MaterialIO
	{
		void WriteBody(tinyxml2::XMLElement* material_element, const Material& material, bool write_raster_state)
		{
			/* Shader */
			const auto& shader = material.GetShader();
			material_element->SetAttribute("ShaderPath",
				shader != nullptr ? RELATIVE_PATH(shader->GetPath()).c_str() : "");

			/* 渲染队列：与 RasterState 同属可选定义 —— 只写参数的覆盖条目沿用基材质的队列 */
			if (write_raster_state)
				material_element->SetAttribute("RenderQueue", material.GetRenderQueue());

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
						/* 贴图参数就是路径值，和其他类型一样走 Value；采样与格式归贴图自身的 .texmeta */
						const auto texture_info = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(param_info.Value);
						param_doc->SetAttribute("Value", RELATIVE_PATH(texture_info.first->GetPath()).c_str());
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
			raster_state_doc->SetAttribute("EnableDepthTest", raster_state.EnableDepthTest);
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

			/* 渲染队列：老资产没有这个属性时保持默认（不透明档） */
			if (const auto* render_queue = material_element->Attribute("RenderQueue"))
				material.SetRenderQueue(render_queue[0] != '\0' ? std::atoi(render_queue) : RENDER_QUEUE_GEOMETRY);

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
							/* 贴图参数就是路径值；采样与格式归贴图自身的 .texmeta */
							const char* texture_path = param_doc->Attribute("Value");
							if (texture_path == nullptr || *texture_path == '\0')
								break;

							auto texture = AssetManager::Instance().GetOrLoad<DeviceTexture>(ABSOLUTE_PATH(texture_path));
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
				raster_state.EnableDepthTest = (raster_state_doc->FindAttribute("EnableDepthTest") != nullptr)
					? raster_state_doc->BoolAttribute("EnableDepthTest")
					: true;
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

			/* 贴图参数就是 ParamInfo 的 Value：整棵树找 Type 为贴图的条目，逐条搬 */
			const auto relocate = [&](auto&& self, tinyxml2::XMLElement* element) -> void
			{
				const bool is_texture_param = std::strcmp(element->Name(), "ParamInfo") == 0
					&& element->IntAttribute("Type", -1) == static_cast<int>(ParamType::Texture);
				const char* source_attribute = is_texture_param ? element->Attribute("Value") : nullptr;
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
							/* 采样设置跟着贴图走，导出后也要能用（原地导出时跳过） */
							const std::filesystem::path source_meta =
								PathFromUtf8(TextureSettingsStore::MetaPath(PathToUtf8(source)));
							std::error_code meta_error;
							if (!same_file && std::filesystem::is_regular_file(source_meta, meta_error))
							{
								const std::filesystem::path destination_meta =
									PathFromUtf8(TextureSettingsStore::MetaPath(PathToUtf8(destination)));
								std::filesystem::copy_file(source_meta, destination_meta,
									std::filesystem::copy_options::overwrite_existing, meta_error);
								if (meta_error)
									CORE_LOG_WARN("Failed to copy texture settings {}: {}.",
										PathToUtf8(source_meta), meta_error.message());
							}

							element->SetAttribute("Value", RELATIVE_PATH(PathToUtf8(destination)).c_str());
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
