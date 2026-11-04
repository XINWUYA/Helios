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

	/* Shader 变体的取值来源。维度写在着色器源码里（配置在 shader，引擎不认识具体名字）：
	 *   uniform sampler2D u_NormalTexture; // @property @variant(USE_NORMAL)
	 * 行尾标注绑到那一行的参数；写成 @variant(NAME, external) 的维度不绑参数。 */
	enum class ShaderVariantSource : uint8_t
	{
		Param = 0, /* 值从材质参数派生：贴图看有没有值，数值看是否非零 */
		External   /* 值由绘制端给：光源类型这类不在材质参数表里的维度 */
	};

	/* 一条变体维度声明 */
	struct ShaderVariantSpec
	{
		std::string			Name{};      /* define 名，拼成 `#define NAME 值` */
		std::string			ParamName{}; /* Param 来源时绑定的材质参数名 */
		ShaderVariantSource	Source{ ShaderVariantSource::Param };
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
		 * 给的值或库内部声明。空 = 拿不到声明的场合（如 SPIR-V 反射路径），调用方自行回退。 */
		std::vector<ReflectedMaterialParam> MaterialParams{};
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
		DeviceShader(std::string path);
		virtual ~DeviceShader() = default;

		/* 路径 */
		const std::string& GetPath() const { return m_Path; }

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
		 * 建不出来的实例 ApplyPrepared 会拒收；编译过程抛异常时返回空。 */
		static SharedPtr<DeviceShader> PrepareReload(const std::string& filepath);

		/* 换入前把 GPU 侧对象建好（Metal 的 library、计算管线），失败则标记无效 */
		virtual bool BuildDeviceObjects() { return true; }

		/* 把另一份同类型实例的编译产物换进来，只该在主线程调。
		 * 成功后 prepared 接管旧状态，由调用方销毁；失败则本对象一个字节没动。 */
		virtual bool ApplyPrepared(DeviceShader& prepared) { (void)prepared; return false; }

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
		 * 写成 external 的不绑参数；同名维度先到者定。 */
		static std::vector<ShaderVariantSpec> ParseVariantSpecs(const std::string& source);

		/* 创建Shader */
		static SharedPtr<DeviceShader> Create(const std::string& filepath);
		static SharedPtr<DeviceShader> Create(const std::string& name, const std::string& vertex_src, const std::string& pixel_src);

	protected:
		void ReflectFromSPIRV(const std::vector<uint32_t>& spirv);
		void ReflectFromGLSLSource(const std::string& source);
		/* 从顶点阶段源码收集 layout(location = N) in 的属性声明 */
		void ReflectVertexAttributes(const std::string& source);

		/* 文件路径 */
		std::string m_Path{};
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

