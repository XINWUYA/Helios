#pragma once
#include <glad/glad.h>
#include "Helios/VirtualDevice/DeviceShader.h"

namespace Helios
{
	class OpenGLShader : public DeviceShader
	{
	public:
		OpenGLShader(const std::string& filepath);
		OpenGLShader(std::string name, const std::string& vertex_src, const std::string& pixel_src);
		~OpenGLShader() override;

		void Bind() override;
		void Unbind() override;

		/* OpenGL 无此需求，空实现 */
		void BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array) override {}

		int GetUniformLocation(const std::string& name) override;
		void SetInt(const std::string& name, int value) override;
		void SetIntArray(const std::string& name, int* values, uint32_t count) override;
		void SetFloat(const std::string& name, float value) override;
		void SetFloat2(const std::string& name, const glm::vec2& value) override;
		void SetFloat3(const std::string& name, const glm::vec3& value) override;
		void SetFloat4(const std::string& name, const glm::vec4& value) override;
		void SetMat4(const std::string& name, const glm::mat4& value) override;
		[[nodiscard]] const std::string& GetDebugName() const override { return m_DebugName; }

	private:
		std::string ReadFile(const std::string& filepath);
		void PreProcessShaderSrc(const std::string& source, std::unordered_map<GLenum, std::string>& shader_sources /*<ShaderStage, ShaderSrc>*/);
#ifdef PLATFORM_WINDOWS
		void CompileShadersToOpenGL();
#endif
		void CreateShaderProgram();

		std::string m_DebugName{"Unnamed Shader"};
		uint32_t m_ProgramID{ 0 };

#ifdef PLATFORM_WINDOWS
		std::unordered_map<GLenum, std::vector<uint32_t>> m_OpenGLSPIRVs{};
#endif
		std::unordered_map<GLenum, std::string> m_OpenGLSourceCodes{};
	};
}
