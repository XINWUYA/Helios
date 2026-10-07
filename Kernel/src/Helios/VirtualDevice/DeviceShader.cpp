#include "Pch.h"
#include "DeviceShader.h"
#include "Helios/Renderer/Renderer.h"
#include "GraphicsAPI/OpenGL/OpenGLShader.h"
#ifdef PLATFORM_MACOS
#include "GraphicsAPI/Metal/MetalShader.h"
#endif
#include <regex>
#include <spirv_cross.hpp>

namespace Helios
{
	DeviceShader::DeviceShader(std::string path)
		: m_Path(std::move(path))
	{
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

	namespace
	{
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
		 * 后到的默认值只在前者没有时补上。 */
		const auto add_material_param = [this](ParamType type, const std::string& name, std::any default_value)
		{
			for (ReflectedMaterialParam& declared : m_Reflection.MaterialParams)
			{
				if (declared.Name != name)
					continue;
				if (!declared.Default.has_value() && default_value.has_value())
					declared.Default = std::move(default_value);
				return;
			}
			m_Reflection.MaterialParams.push_back({ type, name, std::move(default_value) });
		};

		/* 匹配：可选的 layout(binding = N) + uniform <type> <name>[数组]?;（裸 sampler 也是正经材质
		 * 参数，漏掉它就会进不了面板）；binding 拿不到时就不填 SamplerBindings，交给后端另行解析
		 * （Metal 从 MSL 的 [[texture(N)]] 拿）。 */
		static const std::regex sampler_regex(R"((?:layout\s*\(\s*binding\s*=\s*([0-9]+)\s*\)\s*)?uniform\s+(?:highp\s+|mediump\s+|lowp\s+)?([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*;)");

		for (auto it = std::sregex_iterator(source.begin(), source.end(), sampler_regex); it != std::sregex_iterator(); ++it)
		{
			const std::smatch& match = *it;
			const std::string type = match.str(2);
			const std::string name = match.str(3);

			/* 只关心纹理/采样器类资源，uniform block 的类型名不会以这些前缀开头 */
			const bool is_texture_like =
				type.rfind("sampler", 0) == 0 ||
				type.rfind("image", 0) == 0 ||
				type.rfind("texture", 0) == 0 ||
				type.rfind("isampler", 0) == 0 ||
				type.rfind("usampler", 0) == 0;
			if (!is_texture_like)
				continue;

			/* 有 layout(binding) 才填绑定点；裸 sampler 不臆造绑定点 */
			if (match.str(1).size() > 0)
				m_Reflection.SamplerBindings[name] = static_cast<uint32_t>(std::stoul(match.str(1)));
			add_material_param(ParamType::Texture, name, std::any{});
		}

		/* 非纹理材质参数：裸 uniform（int / float / vec2-4 / mat4，可选 layout 限定与源码默认值）。
		 * uniform block 的块头（`uniform Xxx {`）与块内成员都不带 `uniform` 关键字，不会命中。 */
		static const std::regex material_regex(
			R"((?:layout\s*\([^)]*\)\s*)?uniform\s+(?:highp\s+|mediump\s+|lowp\s+)?(int|float|vec[234]|mat4)\s+([A-Za-z_]\w*)\s*(?:=\s*([^;]+?))?\s*;)");

		for (auto it = std::sregex_iterator(source.begin(), source.end(), material_regex); it != std::sregex_iterator(); ++it)
		{
			const std::smatch& match = *it;
			const std::string type = match.str(1);
			const std::string name = match.str(2);

			const ParamType param_type =
				(type == "int") ? ParamType::Int :
				(type == "float") ? ParamType::Float :
				(type == "vec2") ? ParamType::Vec2 :
				(type == "vec3") ? ParamType::Vec3 :
				(type == "vec4") ? ParamType::Vec4 :
				ParamType::Mat4;

			std::any default_value;
			if (match.str(3).size() > 0 && param_type != ParamType::Mat4)
				default_value = ParseMaterialParamDefault(param_type, match.str(3));

			add_material_param(param_type, name, std::move(default_value));
		}
	}
}
