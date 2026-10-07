#pragma once
#include <any>
#include <vector>
#include "Helios/Renderer/RenderCommon.h"

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

	/* 单次绘制的覆盖参数（per-draw）。
	 * 由渲染通道随提交通道应用：只写当前绑定的 Shader 与纹理，不落材质对象 ——
	 * 用于 IBL 这类"每次绘制不同"的绑定（探针选择），避免污染共享材质。 */
	struct DrawParams
	{
		std::vector<MaterialParamInfo> Overrides;
	};

	/*
	 * 材质类
	 */
	class Material
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
		[[nodiscard]] const SharedPtr<DeviceShader>& GetShader() const { return m_pShader; }
		/* 是否天空盒材质：以 SkyBox.glsl 为着色器的材质（天空盒的几何约定：
		 * 顶点着色器把网格铺满全屏、深度取远平面）。这类材质不进延迟管线 G-Buffer、
		 * 不参与阴影投射，由天空背景通道专绘。 */
		[[nodiscard]] bool IsSkyBox() const;
		/* 是否支持反射探针的 IBL 着色（着色器声明了 IBL 契约的采样器，
		 * 见 builtin/IBL.glsl）。渲染通道据此决定是否发 per-draw 的探针绑定。 */
		[[nodiscard]] bool SupportsIBL() const;
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

		/* 绑定材质中的各参数 */
		void Bind();
		/* 解绑材质 */
		void Unbind();

		/* 应用单个参数到当前绑定的 Shader（Bind 与 per-draw 覆盖共用）。
		 * Texture 参数的绑定点：值里带着的 slot 无效时按 Shader 反射重新解析。 */
		void ApplyParam(const MaterialParamInfo& param_info);

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
		 * （材质资产保存后的缓存同步走这里，见 MaterialAssetManager::Refresh）。 */
		void CopyDefinitionFrom(const Material& source);

		/* 只保留当前 Shader 声明的参数（反射是参数集合的单一来源）：
		 * 保存材质资产前调用 —— 反射没声明的不再留在参数表里、不会写进文件。
		 * Shader 为空、或反射为空（后端不反射材质参数）时不做任何事。 */
		void RetainShaderDeclaredParams();

		/* 收集"Shader 声明了、本材质没写"的值参数和默认值（默认取反射里的源码值、没有就按类型零值；
		 * Texture 不参与）。块式 uniform 是 per-program 状态，不写会残留上一个对象的取值 —— 材质
		 * 转换到通用 Shader 绘制时（延迟 GBufferMaterial）用它做 per-draw 补齐。 */
		[[nodiscard]] std::vector<MaterialParamInfo> CollectMissingValueParamDefaults() const;

		/* 默认材质 */
		static SharedPtr<Material>& Default();
		/* 错误材质 */
		static SharedPtr<Material>& Error();
		/* 内置白模材质：default.glsl + 默认贴图满配（White / normal / Black），零外部依赖。
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

		/* 资产路径（可空） */
		std::string m_Path{};
		/* Shader */
		SharedPtr<DeviceShader> m_pShader{ nullptr };
		/* 材质所需的各种参数<ToID(Name), MaterialParamInfo> */
		ParameterMap m_Parameters{};
		/* 光栅化状态配置 */
		RenderRasterState m_RasterState{};

		/* 默认材质和错误材质 */
		static SharedPtr<Material> m_pDefaultMaterial;
		static SharedPtr<Material> m_pErrorMaterial;
		/* 内置白模材质 */
		static SharedPtr<Material> m_pBuiltinWhiteMaterial;
	};

	/* 一条材质条目：内嵌定义 +（可选）槽名和材质资产引用。三种场合共用：模型伴生的 .mtl（多条）、
	 * 独立材质资产 .mtl（单条）、场景实例化覆盖（序列化复用同一份定义，见 MaterialIO）。 */
	struct MaterialEntry
	{
		std::string SlotName;          /* 槽名（空 = 未指定） */
		std::string AssetPath;         /* 材质资产引用（空 = 内嵌定义） */
		SharedPtr<Material> pMaterial; /* 内嵌材质（引用形态时为空） */
	};

	/* 材质组类：
	 * 模型伴生 .mtl 的读写载体（也是独立材质资产文件的读写载体）。
	 * 槽表语义：组内条目与模型 MeshSegment 的槽索引对位。 */
	class MaterialGroup final
	{
	public:
		MaterialGroup() = default;
		~MaterialGroup() = default;

		/* 添加一个内嵌材质条目 */
		void EmplaceMaterial(const SharedPtr<Material>& material);
		/* 添加一个完整条目 */
		void EmplaceEntry(MaterialEntry entry) { m_Entries.emplace_back(std::move(entry)); }
		/* 设置条目槽名（导出前把导入时的材质名写进槽表） */
		void SetEntrySlotName(int idx, std::string name);
		/* 根据索引获取条目（越界返回 nullptr） */
		[[nodiscard]] const MaterialEntry* GetEntryByIndex(int idx) const;
		/* 根据索引获取内嵌材质（越界回退 Error 材质；引用形态条目返回空指针） */
		const SharedPtr<Material>& GetMaterialByIndex(int idx);
		/* 获取全部条目 */
		[[nodiscard]] const std::vector<MaterialEntry>& GetEntries() const { return m_Entries; }
		/* 清空材质 */
		void ClearAllMaterials() { m_Entries.clear(); }

		/* 序列化 */
		void Serializer(const std::string& path);
		/* 反序列化 */
		bool Deserializer(const std::string& path);

	private:

		/* 材质组路径 */
		std::string m_Path{};
		/* 材质条目列表（槽表） */
		std::vector<MaterialEntry> m_Entries;
	};

	/* 材质体（ShaderPath / Parameters / RasterState）的读写：
	 * .mtl 条目、独立材质资产、场景实例覆盖共用同一份实现。 */
	namespace MaterialIO
	{
		/* 把材质的定义写进一个元素（<Material> 或实例覆盖元素） */
		void WriteBody(tinyxml2::XMLElement* material_element, const Material& material);
		/* 从一个元素读回材质定义；失败返回 false（材质保持默认构造） */
		bool ReadBody(const tinyxml2::XMLElement* material_element, Material& material);
	}
}
