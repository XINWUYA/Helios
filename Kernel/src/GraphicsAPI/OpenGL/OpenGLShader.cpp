#include "Pch.h"
#include "OpenGLShader.h"
#include "OpenGLVertexArray.h"

#ifdef PLATFORM_WINDOWS
#include <shaderc/shaderc.hpp>
#endif

#include "Helios/Scene/SceneCommon.h"

#include <cstring>

namespace Helios
{
	static std::filesystem::path GetShaderCacheDirectory()
	{
		return g_AssetsPath / "Cache/shader/opengl";
	}

	static void CreateShaderCacheDirectoryIfNeed()
	{
		const auto shader_cache_dir = GetShaderCacheDirectory();
		if (!std::filesystem::exists(shader_cache_dir))
			std::filesystem::create_directories(shader_cache_dir);
	}

	static const char* GetOpenGLShaderCacheFileExtension(GLenum shader_type)
	{
		switch (shader_type)
		{
		case GL_VERTEX_SHADER:
			return ".cached_opengl_vert.bin";
		case GL_FRAGMENT_SHADER:
			return ".cached_opengl_frag.bin";
		case GL_COMPUTE_SHADER:
			return ".cached_opengl_comp.bin";
		default:
			CORE_LOG_ERROR("Unsupported shader type to cache: {}.", STRINGIFY(shader_type));
			ASSERT(false);
			return "";
		}
	}

	static const char* GetOpenGLShaderStageName(GLenum shader_type)
	{
		switch (shader_type)
		{
		case GL_VERTEX_SHADER:
			return "vertex";
		case GL_FRAGMENT_SHADER:
			return "fragment";
		case GL_COMPUTE_SHADER:
			return "compute";
		default:
			return "unknown";
		}
	}

	/* program 句柄到 shader 名的登记表：驱动警告只带句柄。GL 对象只在主线程创建销毁，无需加锁 */
	static std::unordered_map<GLuint, std::string>& ProgramNameRegistry()
	{
		static std::unordered_map<GLuint, std::string> registry;
		return registry;
	}

	std::string OpenGLShader::AnnotateProgramHandlesInMessage(const char* message)
	{
		if (message == nullptr)
			return {};

		std::string annotation;
		const std::string text(message);

		/* 逐个还原消息里的 "program N"（驱动性能警告的固定句式），句柄不在表中则标注已销毁 */
		std::string::size_type pos = text.find("program ");
		while (pos != std::string::npos)
		{
			const std::string::size_type digits_begin = pos + 8; /* strlen("program ") */
			std::string::size_type digits_end = digits_begin;
			while (digits_end < text.size() && text[digits_end] >= '0' && text[digits_end] <= '9')
				++digits_end;

			if (digits_end > digits_begin)
			{
				const std::string handle_text = text.substr(digits_begin, digits_end - digits_begin);
				const GLuint program = static_cast<GLuint>(std::stoul(handle_text));

				const auto& registry = ProgramNameRegistry();
				const auto it = registry.find(program);

				annotation += " [program " + handle_text + " = " +
					(it != registry.end() ? it->second : std::string("not live (destroyed or reused)")) + "]";
			}

			pos = text.find("program ", digits_end);
		}

		return annotation;
	}

	OpenGLShader::OpenGLShader(const std::string& filepath)
		: DeviceShader(filepath)
	{
		PROFILE_FUNCTION();

		m_DebugName = ExtractFilename(filepath);

		CreateShaderCacheDirectoryIfNeed();

		const std::string shader_src = ReadFile(filepath);
		PreProcessShaderSrc(shader_src, m_OpenGLSourceCodes);

		const auto compute_source = m_OpenGLSourceCodes.find(GL_COMPUTE_SHADER);
		if (compute_source != m_OpenGLSourceCodes.end())
		{
			m_IsCompute = true;
			m_ThreadGroupSize = ParseThreadGroupSize(compute_source->second);
		}

		CreateShaderProgram();
	}

	OpenGLShader::OpenGLShader(std::string name, const std::string& vertex_src, const std::string& pixel_src)
		: DeviceShader(""), m_DebugName(std::move(name))
	{
		PROFILE_FUNCTION();

		// Preprocess shader src
		m_OpenGLSourceCodes[GL_VERTEX_SHADER] = vertex_src;
		m_OpenGLSourceCodes[GL_FRAGMENT_SHADER] = pixel_src;
		
		CreateShaderProgram();
	}

	OpenGLShader::~OpenGLShader()
	{
		PROFILE_FUNCTION();

		if (m_ProgramID != 0)
		{
			/* 从登记表移除：GL 句柄会复用，留着会反查到已销毁的 shader 名 */
			ProgramNameRegistry().erase(m_ProgramID);
			CORE_LOG_DEBUG("[GL] Program #{0} ({1}) destroyed", m_ProgramID, m_DebugName);
		}

		glDeleteProgram(m_ProgramID);
	}

	void OpenGLShader::Bind()
	{
		PROFILE_FUNCTION();

		/* 第一次绑定才执行 link，保证 link 时 GL 状态与绘制时一致（见 LazyLink） */
		if (!m_IsLinked)
			LazyLink();

		glUseProgram(m_ProgramID);
	}

	void OpenGLShader::Unbind()
	{
		PROFILE_FUNCTION();

		glUseProgram(0);
	}

	void OpenGLShader::BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array)
	{
		auto gl_vertex_array = std::dynamic_pointer_cast<OpenGLVertexArray>(vertex_array);
		if (!gl_vertex_array)
			return;

		gl_vertex_array->ConfigureAttributes(m_Reflection);
	}

	int OpenGLShader::GetUniformLocation(const std::string& name)
	{
		return FindUniformLocation(name);
	}

	GLint OpenGLShader::FindUniformLocation(const std::string& name)
	{
		const auto cached = m_UniformLocationCache.find(name);
		if (cached != m_UniformLocationCache.end())
			return cached->second;

		const GLint location = glGetUniformLocation(m_ProgramID, name.c_str());
		m_UniformLocationCache.emplace(name, location);
		return location;
	}

	void OpenGLShader::SetInt(const std::string& name, int value)
	{
		PROFILE_FUNCTION();

		const GLint location = FindUniformLocation(name);
		if (location < 0) return;
		glUniform1i(location, value);
	}

	void OpenGLShader::SetIntArray(const std::string& name, int* values, uint32_t count)
	{
		PROFILE_FUNCTION();

		const GLint location = FindUniformLocation(name);
		if (location < 0) return;
		glUniform1iv(location, count, values);
	}

	void OpenGLShader::SetFloat(const std::string& name, float value)
	{
		PROFILE_FUNCTION();

		const GLint location = FindUniformLocation(name);
		if (location < 0) return;
		glUniform1f(location, value);
	}

	void OpenGLShader::SetFloat2(const std::string& name, const glm::vec2& value)
	{
		PROFILE_FUNCTION();

		const GLint location = FindUniformLocation(name);
		if (location < 0) return;
		glUniform2f(location, value.x, value.y);
	}

	void OpenGLShader::SetFloat3(const std::string& name, const glm::vec3& value)
	{
		PROFILE_FUNCTION();

		const GLint location = FindUniformLocation(name);
		if (location < 0) return;
		glUniform3f(location, value.x, value.y, value.z);
	}

	void OpenGLShader::SetFloat4(const std::string& name, const glm::vec4& value)
	{
		PROFILE_FUNCTION();

		const GLint location = FindUniformLocation(name);
		if (location < 0) return;
		glUniform4f(location, value.x, value.y, value.z, value.w);
	}

	void OpenGLShader::SetMat4(const std::string& name, const glm::mat4& value)
	{
		PROFILE_FUNCTION();

		const GLint location = FindUniformLocation(name);
		if (location < 0) return;
		glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(value));
	}

	std::string OpenGLShader::ReadFile(const std::string& filepath)
	{
		PROFILE_FUNCTION();

		std::string result;
		std::ifstream in(filepath, std::ios::in | std::ios::binary); // ifstream closes itself due to RAII
		if (in)
		{
			/* 处理 #include
			 * Ref: https://community.khronos.org/t/include-in-glsl/59203
			 */
			static const std::regex regex("^[ ]*#[ ]*include[ ]+[\"<](.*)[\">].*");
			std::smatch matches;
			std::string line_str;
			while (std::getline(in, line_str))
			{
				/* 找到#include指令行 */
				if (line_str.find("#include") != line_str.npos)
				{
					if (std::regex_search(line_str, matches, regex))
					{
						/* 获取include文件路径，递归读取 */
						std::string base_dir = ExtractFileBaseDir(filepath);
						result += ReadFile(base_dir + matches.str(1)) + '\n';
					}
				}
				else
				{
					result += line_str + '\n';
				}
			}
		}
		else
		{
			CORE_LOG_ERROR("Could not open file '{0}'", filepath);
			ASSERT(false);
		}

		return result;
	}

	static GLenum ShaderTypeFromString(const std::string& type)
	{
		if (type == "vertex")
			return GL_VERTEX_SHADER;
		if (type == "fragment" || type == "pixel")
			return GL_FRAGMENT_SHADER;
		if (type == "compute")
		{
#ifdef __APPLE__
			CORE_LOG_ERROR("Compute shader is not supported on macOS (OpenGL 4.1). Consider using Metal instead.");
			ASSERT(false, "Compute shader not supported on macOS!");
			return 0;
#else
			return GL_COMPUTE_SHADER;
#endif
		}

		ASSERT(false, "Unknown shader type!");
		return 0;
	}

#ifdef __APPLE__
	/* macOS only supports OpenGL 4.1 (GLSL 4.10), need to convert GLSL version and features */
	static std::string ConvertGLSLToMacOSCompatible(const std::string& source)
	{
		std::string result = source;

		/* Replace version: 450/440/430/420 -> 410 */
		static const std::regex version_regex("#version\\s+4[542][0-9]\\s+core");
		result = std::regex_replace(result, version_regex, "#version 410 core");

		/* Remove layout(binding = X) for sampler uniforms (GLSL 4.20+ feature)
		 * e.g., "layout(binding = 0) uniform sampler2D u_Texture;" -> "uniform sampler2D u_Texture;"
		 */
		static const std::regex binding_regex("layout\\s*\\(\\s*binding\\s*=\\s*[0-9]+\\s*\\)\\s*(uniform)");
		result = std::regex_replace(result, binding_regex, "$1");

		/* Remove layout(std140, binding = X) for uniform blocks
		 * e.g., "layout(std140, binding = 0) uniform CameraData" -> "layout(std140) uniform CameraData"
		 */
		static const std::regex block_binding_regex("layout\\s*\\(\\s*std140\\s*,\\s*binding\\s*=\\s*[0-9]+\\s*\\)\\s*(uniform)");
		result = std::regex_replace(result, block_binding_regex, "layout(std140) $1");

		return result;
	}
#endif

	void OpenGLShader::PreProcessShaderSrc(const std::string& source, std::unordered_map<GLenum, std::string>& shader_sources)
	{
		PROFILE_FUNCTION();

		const char* type_token = "#type";
		const size_t type_token_length = strlen(type_token);
		size_t pos = source.find(type_token, 0); // Start of shader type declaration line
		while (pos != std::string::npos)
		{
			const size_t eol = source.find_first_of("\r\n", pos); // End of shader type declaration line
			ASSERT(eol != std::string::npos, "Syntax error");
			const size_t begin = pos + type_token_length + 1; // Start of shader type name (after "#type " keyword)
			std::string type = source.substr(begin, eol - begin);
			ASSERT(ShaderTypeFromString(type), "Invalid shader type specified");

			const size_t next_line_pos = source.find_first_not_of("\r\n", eol); // Start of shader code after shader type declaration line
			ASSERT(next_line_pos != std::string::npos, "Syntax error");
			pos = source.find(type_token, next_line_pos); // Start of next shader type declaration line

			shader_sources[ShaderTypeFromString(type)] = (pos == std::string::npos) ? source.substr(next_line_pos) : source.substr(next_line_pos, pos - next_line_pos);
		}
	}

#ifdef PLATFORM_WINDOWS
	static shaderc_shader_kind ShaderTypeToOpenGLShaderCKind(GLenum shader_type)
	{
		switch (shader_type)
		{
		case GL_VERTEX_SHADER:
			return shaderc_glsl_vertex_shader;
		case GL_FRAGMENT_SHADER:
			return shaderc_glsl_fragment_shader;
		case GL_COMPUTE_SHADER:
			return shaderc_glsl_compute_shader;
		default:
			CORE_LOG_ERROR("Unsupported shader type to shaderc kind: {}.", STRINGIFY(shader_type));
			ASSERT(false);
			return shaderc_glsl_default_vertex_shader;
		}
	}

	/* Convert GLSL source for SPIR-V compilation on Windows.
	 *  - Upgrades #version 410 core -> 450 core (required for SPIR-V)
	 *  - shaderc will auto-generate bindings for uniforms without explicit layout(binding)
	 */
	static std::string ConvertGLSLForSPIRV(const std::string& source)
	{
		std::string result = source;

		/* Upgrade version: 410 -> 450 */
		static const std::regex version_regex("#version\\s+410\\s+core");
		result = std::regex_replace(result, version_regex, "#version 450 core");

		return result;
	}

	void OpenGLShader::CompileShadersToOpenGL()
	{
		shaderc::Compiler compiler;
		shaderc::CompileOptions options;
		options.SetTargetEnvironment(shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);

		/* Auto-generate binding decorations for uniforms that lack explicit layout(binding=X).
		 * This allows us to share 410-style shaders (without binding on samplers) with macOS. */
		options.SetAutoBindUniforms(true);
		/* Preserve explicit layout(binding=X) and layout(location=X) from source */
		options.SetPreserveBindings(true);

		const bool optimize = false;
		if (optimize)
			options.SetOptimizationLevel(shaderc_optimization_level_performance);

		m_OpenGLSPIRVs.clear();

		auto cache_dir = GetShaderCacheDirectory();
		for (auto&& [shader_type, source] : m_OpenGLSourceCodes)
		{
			std::filesystem::path shader_path = m_Path;
			std::filesystem::path cache_path = cache_dir / (shader_path.filename().string() + GetOpenGLShaderCacheFileExtension(shader_type));

			std::ifstream in(cache_path, std::ios::in | std::ios::binary);
			bool cache_valid = false;
			if (in.is_open())
			{
				// Cache existed
				in.seekg(0, std::ios::end);
				auto size = in.tellg();
				in.seekg(0, std::ios::beg);

				/* 校验缓存有效性：SPIR-V 至少需包含一个 word（magic number），
				 * 空文件或过小的内容视为无效缓存，必须重新编译 */
				if (size >= (std::streamoff)sizeof(uint32_t))
				{
					/* 仅当缓存文件的修改时间不早于源文件时才视为有效，
					 * 避免源文件改动后一直使用旧缓存 */
					std::error_code ec;
					const auto src_time = std::filesystem::last_write_time(m_Path, ec);
					const auto cache_time = std::filesystem::last_write_time(cache_path, ec);
					if (!ec && src_time <= cache_time)
					{
						auto& data = m_OpenGLSPIRVs[shader_type];
						data.resize(static_cast<size_t>(size) / sizeof(uint32_t));
						in.read((char*)data.data(), size);
						cache_valid = !data.empty();
					}
				}
				in.close();
			}

			if (!cache_valid)
			{
				// Convert source for SPIR-V (upgrade version, etc.)
				std::string spirv_source = ConvertGLSLForSPIRV(source);

				// Compile cache
				shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(spirv_source, ShaderTypeToOpenGLShaderCKind(shader_type), m_Path.c_str(), options);
				if (result.GetCompilationStatus() != shaderc_compilation_status_success)
				{
					CORE_LOG_ERROR(result.GetErrorMessage());
					ASSERT(false);
				}

				auto& data = m_OpenGLSPIRVs[shader_type];
				data = std::vector<uint32_t>(result.cbegin(), result.cend());

				// Write data to cache file
				std::ofstream out(cache_path, std::ios::out | std::ios::binary);
				if (out.is_open())
				{
					out.write((char*)data.data(), data.size() * sizeof(uint32_t));
					out.flush();
					out.close();
				}
			}
		}

		/* 反射：从SPIR-V中取出sampler的layout(binding = X) */
		for (auto&& [shader_type, cached_data] : m_OpenGLSPIRVs)
		{
			ReflectFromSPIRV(cached_data);
		}

		/* 材质参数表恒走源码正则反射：SPIR-V 反射不产出裸 uniform 参数，参数表只能由它提供；
		 * sampler 绑定点仍以 SPIR-V 反射为准，两者互补、不冲突 */
		for (auto&& [shader_type, source] : m_OpenGLSourceCodes)
		{
			ReflectFromGLSLSource(source);
			if (shader_type == GL_VERTEX_SHADER)
				ReflectVertexAttributes(source);
		}
	}
#endif
	void OpenGLShader::CreateShaderProgram()
	{
		PROFILE_FUNCTION();

		// Create shader program
		GLuint program = glCreateProgram();
#ifdef HELIOS_DEBUG
		glObjectLabel(GL_PROGRAM, program, -1, m_DebugName.c_str());
#endif
		// Compile shaders
		std::vector<GLuint> compiled_shaders;
		
#ifdef PLATFORM_WINDOWS
		// Use shaderc to compile GLSL to SPIR-V on Windows
		CompileShadersToOpenGL();

		for (auto&& [shader_type, shader_data] : m_OpenGLSPIRVs)
		{
			GLuint shader = glCreateShader(shader_type);
			glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, shader_data.data(), shader_data.size() * sizeof(uint32_t));
			glSpecializeShader(shader, "main", 0, nullptr, nullptr);
			glAttachShader(program, shader);
#ifdef HELIOS_DEBUG
			glObjectLabel(GL_SHADER, shader, -1, m_DebugName.c_str());
#endif
			compiled_shaders.emplace_back(shader);
		}

		/* SPIRV 数据已经上传到 GPU，中间产物不再需要，立即释放以避免长期滞留内存。 */
		m_OpenGLSPIRVs.clear();
#else
		/* macOS/Linux 没有 shaderc，无法产出 SPIR-V，
		 * 直接从 GLSL 源码反射 layout(binding = X)。
		 * 注意：macOS 的 GLSL 4.10 不支持 sampler 上的 layout(binding)，
		 * 下面会在链接后用 glUniform1i 手动把 sampler 指向对应的 texture unit。 */
		m_Reflection.Clear();
		for (auto& source_code : m_OpenGLSourceCodes)
		{
			ReflectFromGLSLSource(source_code.second);
			if (source_code.first == GL_VERTEX_SHADER)
				ReflectVertexAttributes(source_code.second);
		}

		// Use traditional GLSL compilation on macOS/Linux
		for (auto& source_code : m_OpenGLSourceCodes)
		{
			// Create shader
			GLuint shader = glCreateShader(source_code.first);
			
#ifdef __APPLE__
			// Convert GLSL to macOS compatible version (4.10)
			std::string compatible_source = ConvertGLSLToMacOSCompatible(source_code.second);
			const auto* code = (const GLchar*)compatible_source.c_str();
#else
			const auto* code = (const GLchar*)source_code.second.c_str();
#endif
			glShaderSource(shader, 1, &code, 0);

			// Compile shader
			glCompileShader(shader);

			GLint is_compiled = 0;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &is_compiled);
			if (is_compiled == GL_FALSE)
			{
				GLint max_length = 0;
				glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &max_length);
				std::vector<GLchar> info_log(max_length);
				glGetShaderInfoLog(shader, max_length, &max_length, &info_log[0]);
				CORE_LOG_ERROR("Shader compiled failed ({0}):\n{1}", m_Path, info_log.data());
				ASSERT(false);

				// Delete current shader
				glDeleteShader(shader);

				// Delete compiled shaders
				for (auto it : compiled_shaders)
					glDeleteShader(it);

				break;
			}
			compiled_shaders.emplace_back(shader);
		}

		// Attach shaders to program
		for (const auto shader : compiled_shaders)
		{
			glAttachShader(program, shader);
		}
#endif

		/* 句柄转由成员保存，link 完成后再释放（见 LazyLink） */
		m_AttachedShaders = std::move(compiled_shaders);

		/* 这里只编译、挂载，不 link：推迟到第一次 Bind 执行（原因见 LazyLink） */
		m_ProgramID = program;
	}

	void OpenGLShader::LazyLink()
	{
		PROFILE_FUNCTION();

		if (m_IsLinked)
			return;

		const GLuint program = m_ProgramID;

		/* link 之前查到的位置全是无效值，缓存一律作废 */
		m_UniformLocationCache.clear();

		glLinkProgram(program);

		GLint is_linked;
		glGetProgramiv(program, GL_LINK_STATUS, &is_linked);
		if (is_linked == GL_FALSE)
		{
			GLint maxLength;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLength);

			std::vector<GLchar> info_log(maxLength);
			glGetProgramInfoLog(program, maxLength, &maxLength, info_log.data());
			CORE_LOG_ERROR("Shader linking failed ({0}):\n{1}", m_Path, info_log.data());
			ASSERT(false);

			glDeleteProgram(program);
			m_ProgramID = 0;
			return;
		}

#ifdef __APPLE__
		// On macOS, we need to manually bind uniform blocks since layout(binding=X) is not supported
		// Uniform Block bindings: View=0, Object=1, Material=2, Light=3
		{
			GLuint block_index;
			block_index = glGetUniformBlockIndex(program, "ViewUniformBuffer");
			if (block_index != GL_INVALID_INDEX)
				glUniformBlockBinding(program, block_index, 0);

			block_index = glGetUniformBlockIndex(program, "ObjectUniformBuffer");
			if (block_index != GL_INVALID_INDEX)
				glUniformBlockBinding(program, block_index, 1);

			block_index = glGetUniformBlockIndex(program, "MaterialUniformBuffer");
			if (block_index != GL_INVALID_INDEX)
				glUniformBlockBinding(program, block_index, 2);

			block_index = glGetUniformBlockIndex(program, "LightUniformBuffer");
			if (block_index != GL_INVALID_INDEX)
				glUniformBlockBinding(program, block_index, 3);

			block_index = glGetUniformBlockIndex(program, "UIUniformBuffer");
			if (block_index != GL_INVALID_INDEX)
				glUniformBlockBinding(program, block_index, 4);
		}
#endif

		/* link 后统一按反射结果设置每个 sampler 的纹理单元：Windows 驱动导入 SPIR-V 时
		 * 可能不应用 layout(binding)，macOS/Linux 会剥离 binding，都需要在这里修正；
		 * 个别驱动不暴露 uniform location 的，用 glGetActiveUniform 枚举补上 */
		if (!m_Reflection.SamplerBindings.empty())
		{
			GLint prev_program = 0;
			glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);

			glUseProgram(program);

			GLint num_active = 0;
			glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &num_active);
			std::vector<std::pair<GLint, GLint>> active_units;
			active_units.reserve(static_cast<size_t>(num_active));
			for (GLint i = 0; i < num_active; ++i)
			{
				char name[256] = {0};
				GLsizei length = 0;
				GLint size = 0;
				GLenum type = 0;
				glGetActiveUniform(program, i, sizeof(name), &length, &size, &type, name);

				const bool is_sampler = (type >= GL_SAMPLER_1D && type <= GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY);
				if (!is_sampler || size != 1)
					continue;

				std::string uniform_name(name, static_cast<size_t>(length > 0 ? length : std::strlen(name)));
				const auto bracket = uniform_name.find('[');
				if (bracket != std::string::npos)
					uniform_name = uniform_name.substr(0, bracket);

				const auto it = m_Reflection.SamplerBindings.find(uniform_name);
				if (it == m_Reflection.SamplerBindings.end())
					continue;
				const GLint location = glGetUniformLocation(program, name);
				if (location < 0)
					continue;
				active_units.emplace_back(location, static_cast<GLint>(it->second));
			}

			for (const auto& [sampler_name, binding] : m_Reflection.SamplerBindings)
			{
				const GLint location = glGetUniformLocation(program, sampler_name.c_str());
				if (location >= 0)
					glUniform1i(location, static_cast<GLint>(binding));
			}

			for (const auto& [location, value] : active_units)
				glUniform1i(location, value);

			glUseProgram(static_cast<GLuint>(prev_program));
		}

		m_IsLinked = true;

		/* 登记句柄到名字，供驱动警告标注用（见 AnnotateProgramHandlesInMessage） */
		ProgramNameRegistry()[program] = m_DebugName;

		std::string stages;
		for (const GLenum stage : { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_COMPUTE_SHADER })
		{
			if (m_OpenGLSourceCodes.find(stage) == m_OpenGLSourceCodes.end())
				continue;
			if (!stages.empty())
				stages += ", ";
			stages += GetOpenGLShaderStageName(stage);
		}

		CORE_LOG_INFO("[GL] Program #{0} linked: {1} (stages: {2})", program, m_DebugName, stages);

		for (const GLuint shader : m_AttachedShaders)
		{
			glDetachShader(program, shader);
			glDeleteShader(shader);
		}
		m_AttachedShaders.clear();
	}
}
