#pragma once
#include <string>
#include <glm/glm.hpp>
#include "DeviceVertexArray.h"

namespace Helios
{
	/* Shader反射数据：从Shader中`layout(binding = X) uniform sampler2D u_xxx; ` 反射出真实的绑定点  */
	struct ShaderReflectionData
	{
		/* Sampler变量名->binding */
		std::unordered_map<std::string, uint32_t> SamplerBindings{};
		/* Sampler变量名->MetalSamplerState索引（[[sampler(N)]]），仅Metal后端使用 */
		std::unordered_map<std::string, uint32_t> SamplerStateBindings{};

		void Clear()
		{
			SamplerBindings.clear();
			SamplerStateBindings.clear();
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
	};

	/* Shader基类 */
	class DeviceShader
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

