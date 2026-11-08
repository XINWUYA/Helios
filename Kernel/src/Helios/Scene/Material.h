#pragma once
#include "Helios/Common/Export.h"
#include <any>
#include <map>
#include <optional>
#include <vector>
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Renderer/ShaderVariants.h"

namespace tinyxml2 { class XMLElement; }

namespace Helios
{
	class DeviceTexture;
	class DeviceShader;

	/* 材质参数信息 */
	struct MaterialParamInfo
	{
		ParamType	Type{}; /* 参数类型 */
		std::string Name;   /* 参数名 */
		std::any	Value;  /* 参数值, 对于Texture需特殊处理，其Value类型为std::pair<SharedPtr<DeviceTexture>, uint32_t> */

		MaterialParamInfo() = default;
		MaterialParamInfo(ParamType type, std::string name, std::any value)
			: Type(type), Name(std::move(name)), Value(std::move(value))
		{
		}
	};

	/* 绘制时补齐材质没写的参数：结果缓存在材质上，参数或 shader 变了才重算 */
	namespace MaterialFill
	{
		constexpr uint8_t None = 0;
		constexpr uint8_t Values = 1u << 0;   /* 数值参数补默认值（裸 uniform 是 per-program 状态，不补会串值） */
		constexpr uint8_t Textures = 1u << 1; /* @property 采样器补默认贴图（缺绑定 Metal 会断言） */
	}

	/* 单次绘制的覆盖参数（per-draw）。
	 * 由渲染通道随提交通道应用：只写当前绑定的 Shader 与光栅状态、不落材质对象 ——
	 * 用于 IBL 探针选择、逐光源加法笔这类"每次绘制不同"的绑定，避免污染共享材质。 */
	struct DrawParams
	{
		std::vector<MaterialParamInfo> Overrides;
		/* 光栅状态覆盖（如逐光源加法笔的"加法混合 + Equal 深度比较"）。
		 * 设置时整份替代材质自身的光栅状态（不是逐字段合并）；未设置用材质自身的。 */
		std::optional<RenderRasterState> RasterStateOverride{};
		/* 先于 Overrides 应用的补齐项，取值见 MaterialFill */
		uint8_t FillMissing{ MaterialFill::None };
		/* 这一笔要用的着色器变体（外部维度，如调试档位）。会跟材质参数推导合成，
		 * 见 Material::GetDrawShader；空表就是不作额外指定，走材质自己的推导。 */
		ShaderVariantDefines VariantDefines{};
	};

	/*
	 * 材质类
	 */
	class HELIOS_API Material
	{
		using ParameterMap = std::unordered_map<uint32_t, MaterialParamInfo>;

	public:
		Material() = default;
		~Material();

		/* 资产路径（材质资产 = .mtl 文件；运行时构造的匿名材质为空） */
		[[nodiscard]] const std::string& GetPath() const { return m_Path; }
		void SetPath(std::string path) { m_Path = std::move(path); }

		/* 设置Shader */
		void SetShader(const SharedPtr<DeviceShader>& shader);
		/* 参数版本号：增删改参数、换 Shader 都加一。渲染侧据此跳过没变的重复绑定 */
		[[nodiscard]] uint32_t GetRevision() const { return m_Revision; }
		[[nodiscard]] const SharedPtr<DeviceShader>& GetShader() const { return m_pShader; }

		/* 实际绘制用的着色器：着色器声明了材质参数维度时按参数推导，没有就返回基础着色器。
		 * 画用它，判断身份（是否天空盒、能不能进 G-Buffer）仍用 GetShader()。 */
		[[nodiscard]] const SharedPtr<DeviceShader>& GetDrawShader() const { return GetDrawShader(ShaderVariantContext{}); }

		/* 推导结果上再叠外部维度（pass 逐笔决定的那种，如调试档位）。叠加顺序：
		 * 着色器当前绑的变体作底，外部值覆盖它，材质参数最后覆盖。结果按完整 define 指纹分桶缓存。 */
		[[nodiscard]] const SharedPtr<DeviceShader>& GetDrawShader(const ShaderVariantDefines& external) const
		{
			ShaderVariantContext context;
			context.External = &external;
			return GetDrawShader(context);
		}

		/* 逐来源合成这一笔的变体取值：参数从材质推导，外部 / 视图 / 全局各查各的表。
		 * 回退开着时精确变体缺失会取最接近的已加载变体，否则现编。 */
		[[nodiscard]] const SharedPtr<DeviceShader>& GetDrawShader(const ShaderVariantContext& context) const;

		/* 材质级变体取值（外部 / 视图维度按 0 算），面板显示当前启用的维度用 */
		[[nodiscard]] ShaderVariantDefines GetMaterialVariantDefines() const;
		/* 是否天空盒材质：以 SkyBox.glsl 为着色器的材质（天空盒的几何约定：
		 * 顶点着色器把网格铺满全屏、深度取远平面）。这类材质不进延迟管线 G-Buffer、
		 * 不参与阴影投射，由天空背景通道专绘。 */
		[[nodiscard]] bool IsSkyBox() const;
		/* 是否支持反射探针的 IBL 着色（着色器声明了 IBL 契约的采样器，
		 * 见 ReflectionProbe/IBLCommon.glsl）。渲染通道据此决定是否发 per-draw 的探针绑定。 */
		[[nodiscard]] bool SupportsIBL() const;
		/* 是否参与逐光源的直接光照合成（着色器声明了光照契约的控制 uniform，
		 * 见 Builtin/DirectLight.glsl）。前向渲染通道据此决定一个对象是否走
		 * "逐光源一笔"循环（天空盒 / Unlit 类材质单笔原样提交）。 */
		[[nodiscard]] bool SupportsDirectLighting() const;
		/* 能否装进延迟 G-Buffer：着色器声明了标准材质契约的主采样器（u_AlbedoTexture，
		 * 见 Builtin/MaterialLit.glsl）。声明不了的自定义着色器（纯色 / Unlit）
		 * 在延迟管线下用自身着色器前向直画，否则参数会被转换丢弃、外观退化。 */
		[[nodiscard]] bool SupportsGBuffer() const;
		/* 能否自己输出调试读数：着色器接了前向调试分流（声明了 u_DebugView，
		 * 见 DebugShaders/DebugForward.glsl）。接不上的在前向调试档位下由调试材质兜底重画。 */
		[[nodiscard]] bool SupportsDebugView() const;
		/* 渲染队列：小于 2500 不透明（先画、写深度），大于等于 2500 透明
		 * （后画、从远到近、只测不写深度）。见 RenderCommon 的队列常量。 */
		void SetRenderQueue(int32_t render_queue) { m_RenderQueue = render_queue; }
		[[nodiscard]] int32_t GetRenderQueue() const { return m_RenderQueue; }
		[[nodiscard]] bool IsTransparent() const { return IsTransparentRenderQueue(m_RenderQueue); }
		/* 设置参数 */
		void SetParameters(ParamType type, const std::string& name, const std::any& param);
		[[nodiscard]] const ParameterMap& GetAllParameters() const { return m_Parameters; }
		/* 设置纹理：绑定点（texture unit / [[texture(N)]]）完全由 Shader 反射决定 —— 直接取
		 * `layout(binding = X) uniform sampler2D <name>;` 里的 X，调用方不用、也不该再指定 slot。 */
		void SetTexture(const std::string& name, const SharedPtr<DeviceTexture>& texture);
		/* 设置光栅化状态 */
		void SetRasterState(const RenderRasterState& state) { m_RasterState = state; }
		[[nodiscard]] const RenderRasterState& GetRasterState() const { return m_RasterState; }
		RenderRasterState& GetRasterState() { return m_RasterState; }

		/* 绑定材质中的各参数；fill_missing 见 MaterialFill。
		 * draw_shader 是这一笔实际要画的着色器（可能是变体），空指针表示按材质默认推导 */
		void Bind(uint8_t fill_missing = MaterialFill::None, const SharedPtr<DeviceShader>& draw_shader = nullptr);
		/* 解绑材质 */
		void Unbind();

		/* 应用单个参数到当前绑定的 Shader（Bind 与 per-draw 覆盖共用）。
		 * Texture 参数的绑定点：值里带着的 slot 无效时按 Shader 反射重新解析。 */
		void ApplyParam(const MaterialParamInfo& param_info, const SharedPtr<DeviceShader>& draw_shader = nullptr);

		/* 构造一个 per-draw 用的纹理参数：绑定点留待应用时按 Shader 反射解析 */
		static MaterialParamInfo MakeTextureParam(const std::string& name, const SharedPtr<DeviceTexture>& texture);

		/* 组装 IBL 契约（u_UseIBL + 辐照度 / 预滤波 / BRDF LUT）的 per-draw 覆盖：三张图齐了就置一 +
		 * 真实纹理；缺任何一张就置零 + 中性兜底。采样器每次绘制都必须有绑定（Metal 会校验断言），
		 * 门控 uniform 只关取值、不省绑定，四个槽位始终有绑定。 */
		static void MakeIBLParamOverrides(std::vector<MaterialParamInfo>& overrides,
			const SharedPtr<DeviceTexture>& irradiance_map,
			const SharedPtr<DeviceTexture>& prefilter_map,
			const SharedPtr<DeviceTexture>& brdf_lut);

		/* 克隆一个材质实例：共享 Shader 与纹理，复制参数表与光栅化状态；不带资产路径。
		 * 用于"实例化材质"（实体级独立参数，见 ModelComponent::m_SlotOverrides）。 */
		[[nodiscard]] SharedPtr<Material> Clone() const;

		/* 用另一份材质的内容整体覆盖本材质的定义（Shader / 参数表 / 光栅化状态）。
		 * 对象地址不变 —— 已持有它的槽位随之读到新内容
		 * （材质资产保存后的缓存同步走这里，见 AssetManager::Refresh<Material>）。 */
		void CopyDefinitionFrom(const Material& source);

		/* 只保留当前 Shader 声明、且标了 @property 的参数（反射是参数集合的单一来源）：
		 * 保存材质资产前调用 —— 反射没声明的、以及引擎逐绘制给的值都不留在参数表里、
		 * 不会写进文件。Shader 为空、或反射为空（后端不反射材质参数）时不做任何事。 */
		void RetainShaderDeclaredParams();

		/* 收集"Shader 声明了、本材质没写"的值参数和默认值（默认取反射里的源码值、没有就按类型零值；
		 * Texture 不参与）。块式 uniform 是 per-program 状态，不写会残留上一个对象的取值 —— 材质
		 * 转换到通用 Shader 绘制时（延迟 GBuffer）用它做 per-draw 补齐。 */
		[[nodiscard]] std::vector<MaterialParamInfo> CollectMissingValueParamDefaults() const;

		/* 收集"Shader 声明了、本材质没写"的贴图参数，值取内置默认贴图 —— 前向直画用材质自身
		 * shader，声明过的采样器必须每个都有绑定（Metal 校验会直接断言）。 */
		[[nodiscard]] std::vector<MaterialParamInfo> CollectMissingTextureParamDefaults() const;

		/* 采样点缺省时补的默认贴图：法线图按名字认；乘法类（Albedo / Roughness / Ambient）
		 * 用白，其余用黑。GBuffer 材质转换与材质参数补齐共用这一份判据。 */
		static SharedPtr<DeviceTexture> DefaultTextureForSampler(const std::string& sampler_name);

		/* 换成另一个 shader 画时的转换材质（延迟 G-Buffer 用），本材质和目标 shader 都没变就复用上次的。
		 * build 返回空 = 不用转换，直接画自己。 */
		using DeriveFunc = SharedPtr<Material> (*)(const Material& source, const SharedPtr<DeviceShader>& target);
		SharedPtr<Material> GetDerived(const SharedPtr<DeviceShader>& target, DeriveFunc build);

		/* 默认材质 */
		static SharedPtr<Material>& Default();
		/* 错误材质 */
		static SharedPtr<Material>& Error();
		/* 内置白模材质：PBRStandard.glsl + 默认贴图满配（White / normal / Black），零外部依赖。
		 * "槽未绑定"的兜底与"拖 .mesh 起步"的初始外观都是它 —— 白模 = 正常未配置；
		 * 紫色 Error 只代表异常。 */
		static SharedPtr<Material>& BuiltinWhite();

		/* 创建材质 */
		static SharedPtr<Material> Create(const SharedPtr<DeviceShader>& shader);

	private:
		/* 从当前Shader反射出sampler的绑定点，未找到返回TextureSlot::Invalid */
		[[nodiscard]] uint32_t ResolveTextureBinding(const std::string& name) const;
		/* Shader变更后，重新解析所有纹理参数的绑定点 */
		void RefreshTextureBindings();

		/* 按当前 shader 解析好句柄的一条参数：绘制时按句柄写，不再按名字查 */
		struct BoundParam
		{
			const MaterialParamInfo* Info{ nullptr };
			int Handle{ -1 };
			uint32_t Slot{ 0 }; /* 贴图的绑定点 */
		};

		/* 绑定表与转换材质缓存：参数改动计数或 shader 换代后重建。
		 * 拷贝材质时不跟着拷（表里的指针指向原对象的参数表） */
		struct BindCache
		{
			BindCache() = default;
			BindCache(const BindCache&) {}
			BindCache& operator=(const BindCache&) { *this = BindCache(); return *this; }
			BindCache(BindCache&&) = default;
			BindCache& operator=(BindCache&&) = default;

			uint32_t Revision{ UINT32_MAX };
			uint32_t Generation{ 0 };
			const DeviceShader* Shader{ nullptr };
			std::vector<BoundParam> Params;
			std::vector<MaterialParamInfo> ValueFills;
			std::vector<MaterialParamInfo> TextureFills;
			std::vector<BoundParam> BoundValueFills;
			std::vector<BoundParam> BoundTextureFills;
			uint8_t FillsBuilt{ MaterialFill::None };

			WeakPtr<DeviceShader> DerivedTarget;
			uint32_t DerivedRevision{ UINT32_MAX };
			uint32_t DerivedGeneration{ 0 };
			SharedPtr<Material> Derived;
		};

		void StoreParam(MaterialParamInfo info);
		void RebuildBindTable(const SharedPtr<DeviceShader>& shader);
		BoundParam BindParam(const MaterialParamInfo& info, const SharedPtr<DeviceShader>& shader) const;
		void ApplyBound(const BoundParam& bound, const SharedPtr<DeviceShader>& shader);

		/* 声明换代后作废推导缓存（热重载改了 @variant 也能跟上） */
		void RefreshVariantCache() const;

		struct DrawVariantEntry
		{
			SharedPtr<DeviceShader> Shader;
			uint32_t Revision{ UINT32_MAX };
			uint32_t Generation{ 0 };
			bool TexturesReady{ false };
			/* 近似变体：精确的还在后台编，编完要重新查一次 */
			bool Approximate{ false };
			/* 挑出这份近似结果时的完成代数：后台每编完一个变体加一 */
			uint32_t CompletedGeneration{ 0 };
		};

		/* 按声明的来源逐维度求值，param 维度顺带记下要看加载状态的贴图 */
		ShaderVariantDefines ResolveDrawDefines(const ShaderVariantContext& context) const;

		/* 上次作废缓存时的着色器代数 */
		mutable uint32_t mVariantCacheGeneration{ 0 };
		/* 推导结果，按外部 define 的指纹分桶。贴图加载完成不会改版本号，
		 * 每桶另外记一份"上次求值时是否都加载好了" */
		mutable std::map<uint64_t, DrawVariantEntry> m_DrawVariants;
		mutable std::vector<const DeviceTexture*> m_DrawShaderWatchedTextures;

		/* 参数表或 shader 每改一次加一（转换材质据此作废） */
		uint32_t m_Revision{ 0 };
		/* 只在增删参数、换类型、换 shader 时加一（绑定表据此重建，改值不用重建） */
		uint32_t m_LayoutRevision{ 0 };
		BindCache m_BindCache;

		/* 资产路径（可空） */
		std::string m_Path{};
		/* Shader */
		SharedPtr<DeviceShader> m_pShader{ nullptr };
		/* 上次解析贴图绑定点时 Shader 的版本号：Shader 重载后据此重解析一次 */
		uint32_t m_ShaderGeneration{ 0 };
		/* 着色器能力位：SetShader 与热重载换代后算一次，绘制热路径直接读 */
		void RefreshShaderFlags() const;
		[[nodiscard]] bool ShaderFlagsReady() const;
		mutable bool m_HasShaderFlags{ false };
		mutable uint32_t m_FlagsGeneration{ 0 };
		mutable bool m_IsSkyBox{ false };
		mutable bool m_SupportsIBL{ false };
		mutable bool m_SupportsDirectLighting{ false };
		mutable bool m_SupportsGBuffer{ false };
		mutable bool m_SupportsDebugView{ false };
		/* 材质所需的各种参数<ToID(Name), MaterialParamInfo> */
		ParameterMap m_Parameters{};
		/* 光栅化状态配置 */
		RenderRasterState m_RasterState{};
		/* 渲染队列（缺省不透明档） */
		int32_t m_RenderQueue{ RENDER_QUEUE_GEOMETRY };

		/* 默认材质和错误材质 */
		static SharedPtr<Material> m_pDefaultMaterial;
		static SharedPtr<Material> m_pErrorMaterial;
		/* 内置白模材质 */
		static SharedPtr<Material> m_pBuiltinWhiteMaterial;
	};

	/* 材质体（ShaderPath / Parameters / RasterState）的读写：
	 * .mtl 文件、.model 覆盖、场景实例覆盖共用同一份实现。 */
	namespace MaterialIO
	{
		/* 读 / 写一个 .mtl 文件：根元素就是 <Material> */
		HELIOS_API bool ReadFile(const std::string& path, Material& material);
		HELIOS_API bool WriteFile(const std::string& path, const Material& material);

		/* 把材质引用的贴图收进 texture_dir 并改写 Path：导出的模型目录自包含 */
		HELIOS_API bool MakeTexturesLocal(const std::string& material_path, const std::string& texture_dir);

		/* 把材质的定义写进一个元素（<Material> 或实例覆盖元素）；write_raster_state = false
		 * 时只写参数（引用条目的参数覆盖用，光栅状态沿用基材质） */
		HELIOS_API void WriteBody(tinyxml2::XMLElement* material_element, const Material& material,
			bool write_raster_state = true);
		/* 从一个元素读回材质定义；失败返回 false（材质保持默认构造） */
		HELIOS_API bool ReadBody(const tinyxml2::XMLElement* material_element, Material& material);
	}
}
