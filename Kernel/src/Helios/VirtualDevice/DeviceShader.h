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
	/* 一条材质属性的面板规范：全部写在声明行尾的注释里（跟在 @property 后面），
	 *   @property            进面板，显示名默认就是参数名
	 *   @property(显示名)     同上 + 换显示名（可以带空格）
	 *   @type(color)         vec3 / vec4 用取色器
	 *   @type(bool)          int 用复选框（0 / 1）
	 *   @type(slider)        float / int 用滑条，配合 @range
	 *   @range(min, max, step)  数值范围（step 可省，当作拖拽步进）
	 *   @showif(u_X, 值)      仅当 u_X 的当前值等于该值时显示（true / false 也可写）
	 * 没写的项用默认：显示名 = 参数名、控件按参数类型、无范围、无条件。 */
	struct MaterialPropertySpec
	{
		std::string	Label{};       /* 面板显示名（空 = 用参数名） */
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
		/* Sampler变量名->binding */
		std::unordered_map<std::string, uint32_t> SamplerBindings{};
		/* Sampler变量名->MetalSamplerState索引（[[sampler(N)]]），仅Metal后端使用 */
		std::unordered_map<std::string, uint32_t> SamplerStateBindings{};
		/* 反射到的全部材质候选参数（声明顺序）：采样器（Texture）+ 裸 uniform（int/float/vec2-4/mat4）。
		 * 只有带 @property 标记的才归材质（见 ReflectedMaterialParam::Property），其余是引擎逐绘制
		 * 给的值或库内部声明。空 = 拿不到声明的场合（如 SPIR-V 反射路径），调用方自行回退。 */
		std::vector<ReflectedMaterialParam> MaterialParams{};
		/* 声明为 uniform 块（std140）的 binding：计算派发据此区分 uniform 缓冲与 storage 缓冲 */
		std::unordered_set<uint32_t> UniformBlockBindings{};

		void Clear()
		{
			SamplerBindings.clear();
			SamplerStateBindings.clear();
			MaterialParams.clear();
			UniformBlockBindings.clear();
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

		virtual int GetUniformLocation(const std::string& name) = 0;

		/* 拿 sampler 在 Shader 里声明的绑定点（layout(binding = X)）：OpenGL 是 texture unit、
		 * Metal 是 [[texture(N)]] 的索引。没找到该 sampler 时返回 -1。 */
		[[nodiscard]] virtual int GetUniformBinding(const std::string& name) const
		{
			return m_Reflection.FindSamplerBinding(name);
		}

		/* Shader 反射数据 */
		[[nodiscard]] const ShaderReflectionData& GetReflectionData() const { return m_Reflection; }

		/* 设置Uniform参数 */
		virtual void SetInt(const std::string& name, int value) = 0;
		virtual void SetIntArray(const std::string& name, int* values, uint32_t count) = 0;
		virtual void SetFloat(const std::string& name, float value) = 0;
		virtual void SetFloat2(const std::string& name, const glm::vec2& value) = 0;
		virtual void SetFloat3(const std::string& name, const glm::vec3& value) = 0;
		virtual void SetFloat4(const std::string& name, const glm::vec4& value) = 0;
		virtual void SetMat4(const std::string& name, const glm::mat4& value) = 0;

		/* 获取Shader名 */
		virtual const std::string& GetDebugName() const = 0;

		/* 计算着色器：每个工作组多少线程（local_size_x，缺省 64） */
		[[nodiscard]] virtual bool IsCompute() const { return false; }
		[[nodiscard]] virtual uint32_t GetThreadGroupSize() const { return 64; }

		/* 从 GLSL 源码读 local_size_x（读不到按 64） */
		[[nodiscard]] static uint32_t ParseThreadGroupSize(const std::string& glsl_source);

		/* 创建Shader */
		static SharedPtr<DeviceShader> Create(const std::string& filepath);
		static SharedPtr<DeviceShader> Create(const std::string& name, const std::string& vertex_src, const std::string& pixel_src);

	protected:
		void ReflectFromSPIRV(const std::vector<uint32_t>& spirv);
		void ReflectFromGLSLSource(const std::string& source);

		/* 文件路径 */
		std::string m_Path{};
		/* Shader 反射数据（由各后端在编译/链接阶段填充） */
		ShaderReflectionData m_Reflection{};
	};
}

