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

		/* 按着色器声明的属性位置配置顶点属性指针（GL 的指针挂在 VAO 上，绑定前重设） */
		void BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array) override;

		int GetUniformLocation(const std::string& name) override;
		void SetInt(const std::string& name, int value) override;
		void SetIntArray(const std::string& name, int* values, uint32_t count) override;
		void SetFloat(const std::string& name, float value) override;
		void SetFloat2(const std::string& name, const glm::vec2& value) override;
		void SetFloat3(const std::string& name, const glm::vec3& value) override;
		void SetFloat4(const std::string& name, const glm::vec4& value) override;
		void SetMat4(const std::string& name, const glm::mat4& value) override;
		[[nodiscard]] const std::string& GetDebugName() const override { return m_DebugName; }

		/* 计算着色器（#type compute）：桌面 GL 4.3+ 才有，线程组尺寸读 local_size_x */
		[[nodiscard]] bool IsCompute() const override { return m_IsCompute; }
		[[nodiscard]] uint32_t GetThreadGroupSize() const override { return m_ThreadGroupSize; }

		/* 把驱动消息里的 "program N" 句柄还原成 shader 名附在消息后。驱动警告只带
		 * 句柄不带名字，且句柄会复用，需要按当时的登记表反查（实现见 .cpp） */
		[[nodiscard]] static std::string AnnotateProgramHandlesInMessage(const char* message);

	private:
		std::string ReadFile(const std::string& filepath);
		void PreProcessShaderSrc(const std::string& source, std::unordered_map<GLenum, std::string>& shader_sources /*<ShaderStage, ShaderSrc>*/);
#ifdef PLATFORM_WINDOWS
		void CompileShadersToOpenGL();
#endif
		void CreateShaderProgram();

		std::string m_DebugName{"Unnamed Shader"};
		uint32_t m_ProgramID{ 0 };
		bool m_IsCompute{ false };
		uint32_t m_ThreadGroupSize{ 64 };

#ifdef PLATFORM_WINDOWS
		std::unordered_map<GLenum, std::vector<uint32_t>> m_OpenGLSPIRVs{};
#endif
		std::unordered_map<GLenum, std::string> m_OpenGLSourceCodes{};

		/* 构造时编译并挂载的 shader 句柄，link 完成后统一释放 */
		std::vector<GLuint> m_AttachedShaders{};

		/* 延迟链接：构造时只编译、挂载，第一次 Bind 时才执行 link。
		 * link 时驱动会记录当时的 GL 状态，推迟到绘制前可保证与首次绘制一致，
		 * cull face 等状态差异不会触发驱动重新编译着色器 */
		bool m_IsLinked{ false };
		void LazyLink();

		/* uniform 位置缓存：名字 → location。材质每帧按名重写全部参数，
		 * 每次都让驱动按字符串查一遍太浪费；-1 也缓存，省掉反复的空查找。 */
		GLint FindUniformLocation(const std::string& name);
		std::unordered_map<std::string, GLint> m_UniformLocationCache;
	};
}
