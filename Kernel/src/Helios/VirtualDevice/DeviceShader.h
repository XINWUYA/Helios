#pragma once
#include <string>
#include <any>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>
#include "DeviceVertexArray.h"
#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
	/* 一条材质属性的面板规范：全部写在声明行尾的注释里，
	 *   @property            进面板，显示名默认就是参数名
	 *   @property(显示名)     同上 + 换显示名（可以带空格）
	 *   @type(color)         vec3 / vec4 用取色器
	 *   @type(bool)          int 用复选框（0 / 1）
	 *   @type(slider)        float / int 用滑条，配合 @range
	 *   @range(min, max, step)  数值范围（step 可省，当作拖拽步进）
	 *   @showif(u_X, 值)      仅当 u_X 的当前值等于该值时显示（true / false 也可写）
	 * 没写的项用默认：显示名 = 参数名、无分组、控件按参数类型、无范围、无条件。 */
	struct MaterialPropertySpec
	{
		std::string	Label{};       /* 面板显示名（空 = 用参数名） */
		std::string	Group{};       /* @group(分组名)，空 = 未分组 */
		std::string	Widget{};      /* 控件类型（空 = 按参数类型来） */
		bool		HasRange{false};
		float		Min{0.0f};
		float		Max{1.0f};
		float		Step{0.0f};     /* 0 = 没指定 */
		std::string	ShowIfParam{}; /* 空 = 无条件显示 */
		float		ShowIfValue{0.0f};
	};

	/* 反射出的一条材质参数：名字 + 类型 + 源码默认值（可空，空就按类型零值兜底）。材质属性面板
	 * 按它列行 —— 参数集合 / 类型 / 默认值的唯一来源。 */
	struct ReflectedMaterialParam
	{
		ParamType	Type{};
		std::string	Name;
		std::any	Default{};
		/* 声明行带 @property 标记 = 材质自己的参数（面板列、存盘留）。没标的归引擎
		 * （逐绘制绑的贴图、管线设的开关）或库内部，面板看不到、也不会写进材质文件。 */
		bool		Property{false};
		/* 面板规范（显示名 / 控件类型 / 范围 / 条件显示），见 MaterialPropertySpec */
		MaterialPropertySpec PropertySpec{};
	};

	/* Shader反射数据：从Shader中`layout(binding = X) uniform sampler2D u_xxx; ` 反射出真实的绑定点  */
	struct ShaderReflectionData
	{
		/* 采样器被哪些阶段使用（位掩码）：Metal 的顶点 / 片元参数表分开，只往用到的阶段绑 */
		static constexpr uint8_t StageVertex = 1u;
		static constexpr uint8_t StageFragment = 2u;
		static constexpr uint8_t StageAll = StageVertex | StageFragment;

		/* Sampler变量名->binding */
		std::unordered_map<std::string, uint32_t> SamplerBindings{};
		/* Sampler变量名->MetalSamplerState索引（[[sampler(N)]]），仅Metal后端使用 */
		std::unordered_map<std::string, uint32_t> SamplerStateBindings{};
		/* Sampler变量名->使用阶段掩码（StageVertex / StageFragment 的或）。查不到按两个阶段都绑 */
		std::unordered_map<std::string, uint8_t> SamplerStages{};
		/* 反射到的全部材质候选参数（声明顺序）：采样器（Texture）+ 裸 uniform（int/float/vec2-4/mat4）。
		 * 只有带 @property 标记的才归材质（见 ReflectedMaterialParam::Property），其余是引擎逐绘制
		 * 给的值或库内部声明。块成员不算参数，表空不代表反射失败，见 MaterialParamsScanned。 */
		std::vector<ReflectedMaterialParam> MaterialParams{};
		/* 参数表是否真扫过源码：只声明 uniform 块（引擎常量）的着色器扫完也是空的。
		 * 面板据此区分「没有可改的」和「反射拿不到声明」，后者才算失败 */
		bool MaterialParamsScanned{ false };
		/* 声明为 uniform 块（std140）的 binding：计算派发据此区分 uniform 缓冲与 storage 缓冲 */
		std::unordered_set<uint32_t> UniformBlockBindings{};
		/* 顶点阶段声明的 attribute：名字 → layout(location = N) 的 N。
		 * 顶点布局按名字认领位置，不再依赖元素在缓冲里的先后顺序。 */
		std::unordered_map<std::string, uint32_t> VertexAttributeLocations{};
		/* 源码里 @variant 标注的变体维度（声明顺序） */
		std::vector<ShaderVariantSpec> VariantSpecs{};

		void Clear()
		{
			SamplerBindings.clear();
			SamplerStateBindings.clear();
			SamplerStages.clear();
			MaterialParams.clear();
			MaterialParamsScanned = false;
			UniformBlockBindings.clear();
			VertexAttributeLocations.clear();
			VariantSpecs.clear();
		}

		/* 按名字找变体维度；没声明返回 nullptr */
		[[nodiscard]] const ShaderVariantSpec* FindVariantSpec(const std::string& name) const
		{
			for (const ShaderVariantSpec& spec : VariantSpecs)
			{
				if (spec.Name == name)
					return &spec;
			}
			return nullptr;
		}

		[[nodiscard]] uint8_t FindSamplerStageMask(const std::string& name) const
		{
			const auto it = SamplerStages.find(name);
			return it == SamplerStages.end() ? StageAll : it->second;
		}

		[[nodiscard]] int FindSamplerBinding(const std::string& name) const
		{
			const auto it = SamplerBindings.find(name);
			return it == SamplerBindings.end() ? -1 : static_cast<int>(it->second);
		}

		[[nodiscard]] int FindSamplerStateBinding(const std::string& name) const
		{
			const auto it = SamplerStateBindings.find(name);
			return it == SamplerStateBindings.end() ? -1 : static_cast<int>(it->second);
		}

		[[nodiscard]] const ReflectedMaterialParam* FindMaterialParam(const std::string& name) const
		{
			for (const ReflectedMaterialParam& param : MaterialParams)
			{
				if (param.Name == name)
					return &param;
			}
			return nullptr;
		}

		/* 顶点属性名对应的 location；着色器没声明该属性时返回 -1 */
		[[nodiscard]] int FindVertexAttributeLocation(const std::string& name) const
		{
			const auto it = VertexAttributeLocations.find(name);
			return it == VertexAttributeLocations.end() ? -1 : static_cast<int>(it->second);
		}
	};

	/* Shader基类 */
	class HELIOS_API DeviceShader
	{
	public:
		DeviceShader(std::string path, ShaderVariantDefines defines = {});
		virtual ~DeviceShader() = default;

		/* 路径 */
		const std::string& GetPath() const { return m_Path; }

		/* 编译时用的变体 define：已按源码声明补成全集（声明过没写的补 0，没声明的丢掉） */
		[[nodiscard]] const ShaderVariantDefines& GetVariantDefines() const { return m_VariantDefines; }

		/* 上次建这个实例花了多少毫秒（读文件加转换，不含驱动编译）；重载时更新 */
		[[nodiscard]] double GetLoadTimeMs() const { return m_LoadTimeMs; }

		/* 驱动侧编译（Metal 的 MTLLibrary、计算管线）累计耗时。首次绘制才发生，
		 * 命中转换缓存时它是大头，和转换耗时分开看 */
		[[nodiscard]] double GetDeviceBuildMs() const { return m_DeviceBuildMs; }

		/* 绑定 */
		virtual void Bind() = 0;
		virtual void Unbind() = 0;

		/* 在绘制前向 Shader 关联顶点数据（Metal 等后端需在创建 PipelineState 前取得 VertexDescriptor） */
		virtual void BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array) = 0;
		/* 带实例布局的绑定：同一份网格也会被非实例化绘制用，所以实例属性不进顶点数组，
		 * 由每次绑定决定（管线按它分桶）。不支持实例化的后端忽略这个重载 */
		virtual void BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array,
			const VertexBufferLayout& /*instance_layout*/)
		{
			BindVertexArray(vertex_array);
		}

		virtual int GetUniformLocation(const std::string& name) = 0;

		/* 拿 sampler 在 Shader 里声明的绑定点（layout(binding = X)）：OpenGL 是 texture unit、
		 * Metal 是 [[texture(N)]] 的索引。没找到该 sampler 时返回 -1。 */
		[[nodiscard]] virtual int GetUniformBinding(const std::string& name) const
		{
			return m_Reflection.FindSamplerBinding(name);
		}

		/* Shader 反射数据 */
		[[nodiscard]] const ShaderReflectionData& GetReflectionData() const { return m_Reflection; }

		/* 热重载：重新编译并换掉 GPU 对象，对象地址不变，材质持有的指针继续有效。
		 * 内容没变时只刷新依赖表；失败返回 false，旧内容原样保留（编辑期存半截代码是常态）。 */
		bool Reload();

		/* 按路径建一份待换入的实例，可在后台线程调。GPU 侧对象也一并建好，
		 * 建不出来的实例 ApplyPrepared 会拒收；编译过程抛异常时返回空。
		 * defines 要与被换入的那个实例一致，不然换进去的就是另一个变体。 */
		static SharedPtr<DeviceShader> PrepareReload(const std::string& filepath,
			const ShaderVariantDefines& defines = {});

		/* 换入前把 GPU 侧对象建好（Metal 的 library、计算管线），失败则标记无效 */
		virtual bool BuildDeviceObjects() { return true; }

		/* 把另一份同类型实例的编译产物换进来，只该在主线程调。
		 * 成功后 prepared 接管旧状态，由调用方销毁；失败则本对象一个字节没动。
		 * 换入成功连构建状态（变体定义、加载耗时）一起同步，热重载两条路径都走它。 */
		bool ApplyPrepared(DeviceShader& prepared);

		/* 加载时算好的源码哈希与依赖表（含被 include 的文件）。
		 * 热重载靠它判断"内容真改了没"与"改的是不是它的依赖"。 */
		[[nodiscard]] const std::string& GetSourceHash() const { return m_SourceHash; }
		[[nodiscard]] const std::vector<std::string>& GetDependencies() const { return m_Dependencies; }

		/* 依赖文件的原始字节跟加载时一不一样：只读一遍文件就能判定。
		 * 只碰了文件（编辑器自动保存）时靠它挡在编译之前，亚毫秒。 */
		[[nodiscard]] bool ContentUnchanged() const;

		/* 每成功重载一次加一：材质据此判断要不要重解析贴图绑定点 */
		[[nodiscard]] uint32_t GetGeneration() const { return m_Generation; }

		/* 设置Uniform参数 */
		virtual void SetInt(const std::string& name, int value) = 0;
		virtual void SetIntArray(const std::string& name, int* values, uint32_t count) = 0;
		virtual void SetFloat(const std::string& name, float value) = 0;
		virtual void SetFloat2(const std::string& name, const glm::vec2& value) = 0;
		virtual void SetFloat3(const std::string& name, const glm::vec3& value) = 0;
		virtual void SetFloat4(const std::string& name, const glm::vec4& value) = 0;
		virtual void SetMat4(const std::string& name, const glm::mat4& value) = 0;

		/* 参数句柄：名字解析一次，绘制时按句柄写值，不再逐次按字符串查表。
		 * -1 = 着色器里没有这个参数；重载换代后句柄作废，按 GetGeneration 重取 */
		virtual int ResolveParamHandle(const std::string& name) = 0;
		/* data 按 type 解释（int / float / vec2-4 / mat4），handle 为 -1 时什么也不做 */
		virtual void SetParam(int handle, ParamType type, const void* data) = 0;

		/* 获取Shader名 */
		virtual const std::string& GetDebugName() const = 0;

		/* 计算着色器：每个工作组多少线程（local_size_x，缺省 64） */
		[[nodiscard]] virtual bool IsCompute() const { return false; }
		[[nodiscard]] virtual uint32_t GetThreadGroupSize() const { return 64; }

		/* 从 GLSL 源码读 local_size_x（读不到按 64） */
		[[nodiscard]] static uint32_t ParseThreadGroupSize(const std::string& glsl_source);

		/* 解析源码里的 @variant 标注（纯文本，不依赖后端）：行尾标注绑到该行声明的参数，
		 * 写成 draw 的不绑参数；同名维度先到者定。 */
		static std::vector<ShaderVariantSpec> ParseVariantSpecs(const std::string& source);

		/* 创建Shader */
		static SharedPtr<DeviceShader> Create(const std::string& filepath, const ShaderVariantDefines& defines = {});
		static SharedPtr<DeviceShader> Create(const std::string& name, const std::string& vertex_src, const std::string& pixel_src);

		/* 只按源码解析参数与变体声明（不编译、不需要实例）：编辑器选中着色器资产时看它声明了
		 * 什么，与运行时反射走同一份实现。source 传展开 include 后的整份源码。 */
		static void ReflectSource(const std::string& source, ShaderReflectionData& reflection);

	protected:
		/* 后端各自的换入实现；外部只经 ApplyPrepared */
		virtual bool ApplyPreparedImpl(DeviceShader& prepared) { (void)prepared; return false; }

		/* 驱动侧编译的耗时累加，后端在建 library / 管线时调 */
		void AddDeviceBuildMs(double milliseconds) { m_DeviceBuildMs += milliseconds; }

		void ReflectFromSPIRV(const std::vector<uint32_t>& spirv);
		void ReflectFromGLSLSource(const std::string& source);
		/* 从顶点阶段源码收集 layout(location = N) in 的属性声明 */
		void ReflectVertexAttributes(const std::string& source);

		/* 文件路径 */
		std::string m_Path{};
		/* 编译时用的变体 define：热重载要按同一份重编，不能退回不带变体的版本 */
		ShaderVariantDefines m_VariantDefines{};
		/* 建实例的耗时（毫秒），Create 里填 */
		double m_LoadTimeMs{ 0.0 };
		/* 驱动侧编译累计耗时（毫秒），后端建 library / 管线时累加 */
		double m_DeviceBuildMs{ 0.0 };
		/* Shader 反射数据（由各后端在编译/链接阶段填充） */
		ShaderReflectionData m_Reflection{};
		/* 展开 include 后各阶段源码的哈希（加载与重载时填） */
		std::string m_SourceHash{};
		/* 依赖文件原始字节的指纹：只用来挡"文件没变"的重载 */
		std::string m_RawHash{};
		/* 依赖的文件绝对路径：主文件在前，后面是递归展开到的 include */
		std::vector<std::string> m_Dependencies{};
		/* 内容版本号：重载成功后加一 */
		uint32_t m_Generation{ 1 };
	};
}

