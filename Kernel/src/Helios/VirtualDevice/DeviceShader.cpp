#include "Pch.h"
#include "DeviceShader.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/ShaderCompiler.h"
#include "Helios/Renderer/GlslDeclScan.h"
#include "GraphicsAPI/OpenGL/OpenGLShader.h"
#ifdef PLATFORM_MACOS
#include "GraphicsAPI/Metal/MetalShader.h"
#endif
#include <regex>
#include <cctype>
#include <cstring>
#include <spirv_cross.hpp>

namespace Helios
{
	using namespace GlslScan;
	DeviceShader::DeviceShader(std::string path)
		: m_Path(std::move(path))
	{
	}

	uint32_t DeviceShader::ParseThreadGroupSize(const std::string& glsl_source)
	{
		static const std::regex local_size_regex(R"(local_size_x\s*=\s*(\d+))");
		std::smatch match;
		if (!std::regex_search(glsl_source, match, local_size_regex))
			return 64;

		const uint32_t parsed = static_cast<uint32_t>(std::stoul(match[1].str()));
		return (parsed > 0 && parsed <= 1024) ? parsed : 64;
	}

	/* 创建Shader */
	SharedPtr<DeviceShader> DeviceShader::Create(const std::string& filepath)
	{
		switch(Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLShader>(filepath);
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalShader>(filepath);
#endif
		default: 
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}

	/* 创建Shader */
	SharedPtr<DeviceShader> DeviceShader::Create(const std::string& name, const std::string& vertex_src, const std::string& pixel_src)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLShader>(name, vertex_src, pixel_src);
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalShader>(name, vertex_src, pixel_src);
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}

	/* 依赖文件的原始字节跟加载时一致：读一遍文件就够了，不用展开也不用编译 */
	bool DeviceShader::ContentUnchanged() const
	{
		if (m_RawHash.empty() || m_Dependencies.empty())
			return false;

		return ShaderCompiler::HashDependencies(m_Dependencies) == m_RawHash;
	}

	SharedPtr<DeviceShader> DeviceShader::PrepareReload(const std::string& filepath)
	{
		/* 编辑中的源码什么都可能有（比如超长数字让 stoul 抛异常），异常不能漏到调用方 */
		try
		{
			SharedPtr<DeviceShader> prepared = Create(filepath);
			if (prepared != nullptr)
				prepared->BuildDeviceObjects();
			return prepared;
		}
		catch (const std::exception& e)
		{
			CORE_LOG_ERROR("Shader reload threw for {}: {}", filepath, e.what());
		}
		catch (...)
		{
			CORE_LOG_ERROR("Shader reload threw an unknown exception for {}", filepath);
		}
		return nullptr;
	}

	/* 热重载：就地建一份新的再换进来，两步都走构造链路的同一份代码 */
	bool DeviceShader::Reload()
	{
		PROFILE_FUNCTION();

		if (m_Path.empty())
		{
			CORE_LOG_WARN("Shader was built from embedded source and cannot be reloaded");
			return false;
		}

		/* 只碰了文件（编辑器自动保存）不值得重建：这里挡在编译之前 */
		if (ContentUnchanged())
			return true;

		const auto start_time = std::chrono::steady_clock::now();
		const uint32_t generation_before = m_Generation;

		const SharedPtr<DeviceShader> prepared = PrepareReload(m_Path);
		if (prepared == nullptr || !ApplyPrepared(*prepared))
			return false;

		/* 依赖表变了但展开结果一样（比如 include 了一个空文件）：不换代，也不记日志 */
		if (m_Generation == generation_before)
			return true;

		const double elapsed_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start_time).count();
		CORE_LOG_INFO("[Shader] Reloaded {} (#{}, {:.1f} ms)",
			std::filesystem::path(m_Path).filename().string(), m_Generation, elapsed_ms);
		return true;
	}

	namespace
	{
		/* 声明行尾的"这是材质参数"标记：只有带标记的才进材质面板、才写进材质文件。
		 * 不标 = 引擎逐绘制绑的贴图（u_ShadowMap / u_BRDFLut 这类）、管线设的开关，或库内部声明。 */
		constexpr const char* kPropertyMarker = "@property";

		/* 解析 GLSL 字面量：`1.5f` / `0` / `-2`（去空白与 f 后缀；失败返回 false） */
		bool ParseFloatLiteral(const std::string& token, float& out)
		{
			size_t begin = 0;
			size_t end = token.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(token[begin])))
				++begin;
			while (end > begin && std::isspace(static_cast<unsigned char>(token[end - 1])))
				--end;

			std::string text = token.substr(begin, end - begin);
			if (!text.empty() && (text.back() == 'f' || text.back() == 'F'))
				text.pop_back();
			if (text.empty())
				return false;

			char* parsed_end = nullptr;
			const float value = std::strtof(text.c_str(), &parsed_end);
			if (parsed_end == text.c_str() || *parsed_end != '\0')
				return false;

			out = value;
			return true;
		}

		/* 解析 int 字面量：`0` / `-2` */
		bool ParseIntLiteral(const std::string& token, int& out)
		{
			size_t begin = 0;
			size_t end = token.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(token[begin])))
				++begin;
			while (end > begin && std::isspace(static_cast<unsigned char>(token[end - 1])))
				--end;

			const std::string text = token.substr(begin, end - begin);
			if (text.empty())
				return false;

			char* parsed_end = nullptr;
			const long value = std::strtol(text.c_str(), &parsed_end, 10);
			if (parsed_end == text.c_str() || *parsed_end != '\0')
				return false;

			out = static_cast<int>(value);
			return true;
		}

		/* 解析材质参数的源码默认值（`vec4(1.0f, 1.0f, 0.0f, 0.0f)` / `0.5f` / `1`）。
		 * 结果装进 std::any（与材质参数表同款类型）；解析不了就返回空 any，
		 * 面板按类型零值兜底 —— 默认值只是"显示口径"，解析宁缺毋滥。 */
		std::any ParseMaterialParamDefault(ParamType type, const std::string& expression)
		{
			if (type == ParamType::Int)
			{
				int value = 0;
				return ParseIntLiteral(expression, value) ? std::any(value) : std::any{};
			}

			if (type == ParamType::Float)
			{
				float value = 0.0f;
				return ParseFloatLiteral(expression, value) ? std::any(value) : std::any{};
			}

			/* vec2/3/4：vecN(a, b, …) —— 括号内按逗号拆分量，逐个按浮点字面量解析 */
			const int components = (type == ParamType::Vec2) ? 2 : (type == ParamType::Vec3) ? 3
				: (type == ParamType::Vec4) ? 4 : 0;
			if (components == 0)
				return std::any{};

			const size_t open = expression.find('(');
			const size_t close = expression.rfind(')');
			if (open == std::string::npos || close == std::string::npos || close <= open)
				return std::any{};

			std::vector<float> values;
			const std::string inner = expression.substr(open + 1, close - open - 1);
			size_t cursor = 0;
			while (cursor <= inner.size())
			{
				const size_t comma = inner.find(',', cursor);
				const std::string token = inner.substr(cursor,
					comma == std::string::npos ? std::string::npos : comma - cursor);

				float value = 0.0f;
				if (!ParseFloatLiteral(token, value))
					return std::any{};
				values.push_back(value);

				if (comma == std::string::npos)
					break;
				cursor = comma + 1;
			}

			if (static_cast<int>(values.size()) != components)
				return std::any{};

			switch (type)
			{
			case ParamType::Vec2: return std::any(glm::vec2(values[0], values[1]));
			case ParamType::Vec3: return std::any(glm::vec3(values[0], values[1], values[2]));
			case ParamType::Vec4: return std::any(glm::vec4(values[0], values[1], values[2], values[3]));
			default: return std::any{};
			}
		}

		/* 去掉首尾空白 */
		std::string Trim(const std::string& text)
		{
			size_t begin = 0;
			size_t end = text.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
				++begin;
			while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
				--end;
			return text.substr(begin, end - begin);
		}

		/* 注释里有没有这条指令：@range 不会命中 @range2（后面必须是括号 / 空白 / 结尾） */
		bool HasDirective(const std::string& comment, const char* directive)
		{
			size_t pos = comment.find(directive);
			while (pos != std::string::npos)
			{
				const size_t after = pos + std::strlen(directive);
				if (after >= comment.size() || comment[after] == '('
					|| std::isspace(static_cast<unsigned char>(comment[after])))
					return true;
				pos = comment.find(directive, after);
			}
			return false;
		}

		/* 取 `@名字(...)` 括号里的文本；没这条指令、或后面没括号时返回空 */
		std::string DirectiveArg(const std::string& comment, const char* directive)
		{
			size_t pos = comment.find(directive);
			while (pos != std::string::npos)
			{
				const size_t after = pos + std::strlen(directive);
				if (after >= comment.size())
					return {};

				/* 紧跟空白 = 光秃秃一条指令（如 @property），没有括号参数 */
				if (std::isspace(static_cast<unsigned char>(comment[after])))
					return {};

				if (comment[after] == '(')
				{
					const size_t close = comment.find(')', after);
					if (close == std::string::npos)
						return {};
					return Trim(comment.substr(after + 1, close - after - 1));
				}

				pos = comment.find(directive, after);
			}
			return {};
		}

		/* 按逗号拆参数（保留空项，交给调用方判长度） */
		std::vector<std::string> SplitArgs(const std::string& text)
		{
			std::vector<std::string> args;
			size_t begin = 0;
			while (true)
			{
				const size_t comma = text.find(',', begin);
				args.push_back(Trim(text.substr(begin,
					comma == std::string::npos ? std::string::npos : comma - begin)));
				if (comma == std::string::npos)
					break;
				begin = comma + 1;
			}
			return args;
		}

		/* 行内 uniform 声明的参数名：`uniform sampler2D u_X;` / `layout(...) uniform vec4 u_Y = ...;`。
		 * uniform 块头（`uniform Xxx {`）没有分号，返回空。 */
		std::string UniformNameInLine(const std::string& line)
		{
			const size_t uniform_pos = line.find("uniform");
			if (uniform_pos == std::string::npos)
				return {};

			const size_t semicolon = line.find(';', uniform_pos);
			if (semicolon == std::string::npos)
				return {};

			std::string decl = line.substr(uniform_pos + 7, semicolon - uniform_pos - 7);
			if (const size_t assign = decl.find('='); assign != std::string::npos)
				decl.erase(assign);
			if (const size_t bracket = decl.find('['); bracket != std::string::npos)
				decl.erase(bracket);

			/* 类型之后最后一个标识符就是参数名 */
			const size_t name_end = decl.find_last_not_of(" \t\r\n");
			if (name_end == std::string::npos)
				return {};
			const size_t name_start = decl.find_last_of(" \t\r\n", name_end);
			return (name_start == std::string::npos)
				? decl.substr(0, name_end + 1)
				: decl.substr(name_start + 1, name_end - name_start);
		}

		/* 把声明行尾注释解析成面板规范（认不出的指令就当普通注释，不影响参数本身） */
		MaterialPropertySpec ParsePropertySpec(const std::string& comment)
		{
			MaterialPropertySpec spec;
			spec.Label = DirectiveArg(comment, kPropertyMarker);

			spec.Widget = DirectiveArg(comment, "@type");
			for (char& character : spec.Widget)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

			const std::vector<std::string> range = SplitArgs(DirectiveArg(comment, "@range"));
			float min_value = 0.0f;
			float max_value = 0.0f;
			if (range.size() >= 2 && ParseFloatLiteral(range[0], min_value)
				&& ParseFloatLiteral(range[1], max_value) && min_value < max_value)
			{
				spec.HasRange = true;
				spec.Min = min_value;
				spec.Max = max_value;
				if (range.size() >= 3)
					ParseFloatLiteral(range[2], spec.Step);
			}

			const std::vector<std::string> show_if = SplitArgs(DirectiveArg(comment, "@showif"));
			if (show_if.size() == 2 && !show_if[0].empty())
			{
				float value = 0.0f;
				if (show_if[1] == "true")
				{
					spec.ShowIfParam = show_if[0];
					spec.ShowIfValue = 1.0f;
				}
				else if (show_if[1] == "false")
				{
					spec.ShowIfParam = show_if[0];
					spec.ShowIfValue = 0.0f;
				}
				else if (ParseFloatLiteral(show_if[1], value))
				{
					spec.ShowIfParam = show_if[0];
					spec.ShowIfValue = value;
				}
			}

			return spec;
		}

		/* 第二项是不是 external（不区分大小写） */
		bool IsExternalKeyword(const std::string& text)
		{
			static constexpr const char* kExternal = "external";
			if (text.size() != std::strlen(kExternal))
				return false;
			for (size_t i = 0; i < text.size(); ++i)
			{
				if (std::tolower(static_cast<unsigned char>(text[i])) != kExternal[i])
					return false;
			}
			return true;
		}
	}

	/* 变体维度：逐行找 @variant(NAME) / @variant(NAME, external)。行尾标注绑到该行声明的参数，
	 * 单独一行或写了 external 的就是外部维度 —— 值不在材质里取。 */
	std::vector<ShaderVariantSpec> DeviceShader::ParseVariantSpecs(const std::string& source)
	{
		std::vector<ShaderVariantSpec> specs;
		if (source.empty())
			return specs;

		const auto add = [&specs](ShaderVariantSpec spec)
		{
			for (const ShaderVariantSpec& existing : specs)
			{
				if (existing.Name == spec.Name)
					return; /* 同名先到者定 */
			}
			specs.push_back(std::move(spec));
		};

		size_t line_begin = 0;
		while (line_begin <= source.size())
		{
			const size_t line_end = source.find('\n', line_begin);
			const std::string line = source.substr(line_begin,
				line_end == std::string::npos ? std::string::npos : line_end - line_begin);

			if (line.find("@variant") != std::string::npos)
			{
				const std::vector<std::string> args = SplitArgs(DirectiveArg(line, "@variant"));
				if (!args.empty() && !args[0].empty())
				{
					ShaderVariantSpec spec;
					spec.Name = args[0];
					const bool external = args.size() > 1 && IsExternalKeyword(args[1]);
					if (!external)
						spec.ParamName = UniformNameInLine(line);
					/* 绑不到参数的（单独一行写的）自然按外部维度处理 */
					spec.Source = (external || spec.ParamName.empty())
						? ShaderVariantSource::External
						: ShaderVariantSource::Param;
					add(std::move(spec));
				}
			}

			if (line_end == std::string::npos)
				break;
			line_begin = line_end + 1;
		}
		return specs;
	}

	void DeviceShader::ReflectFromSPIRV(const std::vector<uint32_t>& spirv)
	{
		if (spirv.empty())
			return;

		try
		{
			m_Reflection.Clear();

			spirv_cross::Compiler compiler(spirv);
			const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

			/* GLSL 风格的组合采样器：uniform sampler2D / samplerCube / sampler2DArray ... */
			for (const auto& resource : resources.sampled_images)
			{
				const uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
				std::string name = resource.name.empty() ? compiler.get_name(resource.id) : resource.name;
				if (!name.empty())
					m_Reflection.SamplerBindings[name] = binding;
			}

			/* 分离式纹理（texture2D + sampler），Vulkan 风格 Shader 会走这里 */
			for (const auto& resource : resources.separate_images)
			{
				const uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
				std::string name = resource.name.empty() ? compiler.get_name(resource.id) : resource.name;
				if (!name.empty())
					m_Reflection.SamplerBindings[name] = binding;
			}

			for (const auto& resource : resources.separate_samplers)
			{
				const uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
				std::string name = resource.name.empty() ? compiler.get_name(resource.id) : resource.name;
				if (!name.empty())
					m_Reflection.SamplerStateBindings[name] = binding;
			}

			/* Storage image（compute） */
			for (const auto& resource : resources.storage_images)
			{
				const uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
				std::string name = resource.name.empty() ? compiler.get_name(resource.id) : resource.name;
				if (!name.empty())
					m_Reflection.SamplerBindings[name] = binding;
			}

			/* uniform 块：派发计算时要按 binding 绑到 uniform 缓冲，其余按 storage 缓冲处理 */
			for (const auto& resource : resources.uniform_buffers)
				m_Reflection.UniformBlockBindings.insert(
					compiler.get_decoration(resource.id, spv::DecorationBinding));
		}
		catch (const std::exception& e)
		{
			CORE_LOG_ERROR("SPIRV-Cross reflection failed: {}", e.what());
		}
	}

	void DeviceShader::ReflectFromGLSLSource(const std::string& source)
	{
		if (source.empty())
			return;


		/* 材质参数表按名去重（顶点 / 片元各调用一次）：先到者定类型，
		 * 后到的默认值只在前者没有时补上，材质标记取或、面板规范取有内容的那份。 */
		const auto add_material_param = [this](ParamType type, const std::string& name, std::any default_value,
			bool property, const MaterialPropertySpec& spec)
		{
			for (ReflectedMaterialParam& declared : m_Reflection.MaterialParams)
			{
				if (declared.Name != name)
					continue;
				if (!declared.Default.has_value() && default_value.has_value())
					declared.Default = std::move(default_value);
				declared.Property = declared.Property || property;
				if (!spec.Label.empty() || !spec.Widget.empty() || spec.HasRange || !spec.ShowIfParam.empty())
					declared.PropertySpec = spec;
				return;
			}
			m_Reflection.MaterialParams.push_back({ type, name, std::move(default_value), property, spec });
		};

		/* 单遍扫出全部 uniform 声明，再按「贴图 → 标量 → 块」的原顺序落地，
		 * 材质参数表的先后顺序与改造前一致 */
		std::vector<ScannedDecl> decls;
		ScanUniformDecls(source, decls);

		/* 贴图 / 采样器：裸 sampler 也要进反射表；绑定点拿不到时交给后端解析
		 * （Metal 从 MSL 的 [[texture(N)]] 拿） */
		for (const ScannedDecl& decl : decls)
		{
			if (decl.DeclKind != ScannedDecl::Kind::Texture || decl.HasInit)
				continue;

			uint32_t binding = 0;
			if (decl.HasLayout && ParseOnlyBinding(decl.Qualifiers, binding))
				m_Reflection.SamplerBindings[decl.Name] = binding;

			add_material_param(ParamType::Texture, decl.Name, std::any{},
				HasDirective(decl.Comment, kPropertyMarker), ParsePropertySpec(decl.Comment));
		}

		/* 非纹理材质参数：裸 uniform（int / float / vec2-4 / mat4，可选 layout 限定与源码默认值） */
		for (const ScannedDecl& decl : decls)
		{
			if (decl.DeclKind != ScannedDecl::Kind::Scalar || decl.HasArray)
				continue;

			const std::string& type = decl.TypeName;
			const bool known = type == "int" || type == "float" || type == "vec2"
				|| type == "vec3" || type == "vec4" || type == "mat4";
			if (!known)
				continue;

			const ParamType param_type =
				(type == "int") ? ParamType::Int :
				(type == "float") ? ParamType::Float :
				(type == "vec2") ? ParamType::Vec2 :
				(type == "vec3") ? ParamType::Vec3 :
				(type == "vec4") ? ParamType::Vec4 :
				ParamType::Mat4;

			std::any default_value;
			if (decl.HasInit && param_type != ParamType::Mat4)
				default_value = ParseMaterialParamDefault(param_type, decl.InitExpr);

			add_material_param(param_type, decl.Name, std::move(default_value),
				HasDirective(decl.Comment, kPropertyMarker), ParsePropertySpec(decl.Comment));
		}

		/* uniform 块（`layout(std140, binding = N) uniform Block {`）的 binding：
		 * 只认块声明，裸 uniform 不带花括号、不会命中 */
		for (const ScannedDecl& decl : decls)
		{
			/* 只认带 layout 的块声明（`layout(std140, binding = N) uniform Block {`） */
			if (decl.DeclKind != ScannedDecl::Kind::Block || !decl.HasLayout)
				continue;

			uint32_t binding = 0;
			if (ParseAnyBinding(decl.Qualifiers, binding))
				m_Reflection.UniformBlockBindings.insert(binding);
		}

		/* 变体维度：顶点 / 片元各调一次，同名只留一份 */
		for (const ShaderVariantSpec& spec : ParseVariantSpecs(source))
		{
			if (m_Reflection.FindVariantSpec(spec.Name) == nullptr)
				m_Reflection.VariantSpecs.push_back(spec);
		}
	}

	/* 顶点属性：layout(location = N) in <type> <name>; —— 位置由着色器给出，
	 * 顶点布局按名字认领，不再依赖元素在缓冲里的先后顺序 */
	void DeviceShader::ReflectVertexAttributes(const std::string& source)
	{
		if (source.empty())
			return;

		/* 扫 `layout(location = N) in ...;`，替代原来的一条全文正则 */
		size_t cursor = 0;
		while (true)
		{
			const size_t layout_pos = FindWord(source, cursor, "layout");
			if (layout_pos == std::string::npos)
				return;

			cursor = layout_pos + std::strlen("layout");

			size_t pos = SkipSpaces(source, cursor);
			if (pos >= source.size() || source[pos] != '(')
				continue;

			pos = SkipSpaces(source, pos + 1);
			if (!IsWordAt(source, pos, "location"))
				continue;

			pos = SkipSpaces(source, pos + std::strlen("location"));
			if (pos >= source.size() || source[pos] != '=')
				continue;

			pos = SkipSpaces(source, pos + 1);
			const size_t digits_begin = pos;
			while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9')
				++pos;
			if (pos == digits_begin)
				continue;

			const uint32_t location =
				static_cast<uint32_t>(std::stoul(source.substr(digits_begin, pos - digits_begin)));

			pos = SkipSpaces(source, pos);
			if (pos >= source.size() || source[pos] != ')')
				continue;

			pos = SkipSpaces(source, pos + 1);
			if (!IsWordAt(source, pos, "in"))
				continue;

			const size_t after_in = pos + std::strlen("in");
			pos = SkipSpaces(source, after_in);
			if (pos == after_in)
				continue; /* in 后面要有空白 */

			const size_t semi = source.find(';', pos);
			if (semi == std::string::npos)
				continue;

			cursor = semi + 1;

			const std::string declaration = source.substr(pos, semi - pos);
			const size_t name_end = declaration.find_last_not_of(" \t\r\n");
			if (name_end == std::string::npos)
				continue;

			const size_t name_start = declaration.find_last_of(" \t\r\n", name_end);
			const std::string name = (name_start == std::string::npos)
				? declaration.substr(0, name_end + 1)
				: declaration.substr(name_start + 1, name_end - name_start);

			if (!name.empty())
				m_Reflection.VertexAttributeLocations[name] = location;
		}
	}
}
