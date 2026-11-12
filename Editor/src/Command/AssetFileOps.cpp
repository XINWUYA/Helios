#include "Pch.h"
#include "AssetFileOps.h"
#include "Helios/Core/TextureSettings.h"
#include "Helios/Scene/TerrainAsset.h"
#include <algorithm>
#include <cctype>
#include <fstream>

namespace Helios
{
	namespace
	{
		/* 回收站里的文件名：在资源根内就带上相对路径（把分隔符换成 '_'，看得出它原来在哪），
		 * 否则只用文件名。人工去回收站里翻的时候认得出来。 */
		std::string TrashNameFor(const std::filesystem::path& target)
		{
			const std::filesystem::path relative = ToAssetRelative(target);

			std::string name;
			if (!relative.empty() && relative.is_relative())
				name = PathToUtf8(relative);
			else
				name = PathToUtf8(target.filename());

			std::replace(name.begin(), name.end(), '/', '_');
			std::replace(name.begin(), name.end(), '\\', '_');
			return name;
		}

		/* prefix 是不是 path 的祖先（或与它相等）—— 按路径分量逐段比，不拼字符串：
		 * 分隔符风格（/ 与 \）与 "." / ".." 之类的词法差别都由 path 自己归一化掉。 */
		bool IsPathPrefix(const std::filesystem::path& prefix, const std::filesystem::path& path)
		{
			std::error_code error;
			const std::filesystem::path prefix_abs = std::filesystem::absolute(prefix, error).lexically_normal();
			if (error)
				return false;
			const std::filesystem::path path_abs = std::filesystem::absolute(path, error).lexically_normal();
			if (error)
				return false;

			auto prefix_component = prefix_abs.begin();
			auto path_component = path_abs.begin();

			for (; prefix_component != prefix_abs.end(); ++prefix_component, ++path_component)
			{
				if (path_component == path_abs.end() || *path_component != *prefix_component)
					return false;
			}

			return true;
		}

		/* 脚本类名从文件名来：非法字符换下划线，首字符不是字母就补个 S 打头。
		 * 模板里的类名跟文件名对得上，用户不用先想名字再对齐两处。 */
		std::string ScriptClassName(const std::string& stem)
		{
			std::string class_name = stem.empty() ? std::string("NewScript") : stem;

			for (char& character : class_name)
			{
				if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_')
					character = '_';
			}

			if (!std::isalpha(static_cast<unsigned char>(class_name.front())))
				class_name.insert(class_name.begin(), 'S');

			return class_name;
		}

		/* 引擎内置 shader 里只有这个目录是"给场景物体选的材质"，其余是管线内部用的
		 * （后处理 / 天空 / 覆盖层 / 计算） */
		constexpr const char* kEngineMaterialShaderDir = "Shaders/MaterialShaders/";

		/* shader 清单缓存：整棵工程扫一遍是 O(文件数)，材质下拉不该每次打开都付这个钱。
		 * 资源树一有增删改名就置脏（见 InvalidateShaderOptions），换工程时按根路径再比一次。 */
		struct ShaderOptionCache
		{
			std::filesystem::path ProjectRoot;
			std::vector<std::string> Options;
			bool Dirty{ true };
		};

		ShaderOptionCache& GetShaderOptionCache()
		{
			static ShaderOptionCache cache;
			return cache;
		}

		/* 一个根下的候选：收 .glsl 且源码里带 stage 声明（与编译器切分的判据一致），
		 * directory 为空 = 不限目录。Cache 目录（编译产物）整棵跳过。 */
		void ScanShaderOptions(const std::filesystem::path& root, const std::string& directory,
			std::vector<std::string>& options)
		{
			/* 目录按小写比：资源目录是用户自己建的，大小写不该影响能不能选到 */
			std::string directory_lower = directory;
			for (char& character : directory_lower)
			{
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character - 'A' + 'a');
			}

			std::error_code error;
			for (std::filesystem::recursive_directory_iterator iter(root,
					std::filesystem::directory_options::skip_permission_denied, error), end;
				iter != end; iter.increment(error))
			{
				if (error)
					break;

				const std::filesystem::directory_entry& item = *iter;
				if (item.is_directory())
				{
					if (item.path().filename() == "Cache")
						iter.disable_recursion_pending();
					continue;
				}

				if (!item.is_regular_file())
					continue;

				std::string extension = PathToUtf8(item.path().extension());
				for (char& character : extension)
				{
					if (character >= 'A' && character <= 'Z')
						character = static_cast<char>(character - 'A' + 'a');
				}
				if (extension != ".glsl")
					continue;

				const std::string relative = PathToUtf8(ToAssetRelative(item.path()));

				std::string lowered = relative;
				for (char& character : lowered)
				{
					if (character >= 'A' && character <= 'Z')
						character = static_cast<char>(character - 'A' + 'a');
				}
				if (!directory_lower.empty() && lowered.rfind(directory_lower, 0) != 0)
					continue;

				bool has_stage = false;
				std::ifstream source(item.path(), std::ios::binary);
				for (std::string line; std::getline(source, line); )
				{
					if (line.find("#type") != std::string::npos)
					{
						has_stage = true;
						break;
					}
				}
				if (!has_stage)
					continue;

				/* 项目根先扫，同名的内置 shader 不再重复列一遍 */
				if (std::find(options.begin(), options.end(), relative) != options.end())
					continue;

				options.emplace_back(relative);
			}
		}
	}

	std::filesystem::path AssetTrashRoot()
	{
		/* 与项目根平级、按项目分目录：不进资源树，也不会被打进包 */
		return g_ProjectPath.parent_path() / ".helios-trash" / g_ProjectPath.filename();
	}

	AssetFileKind AssetFileKindOf(const std::string& extension)
	{
		/* 后缀一律转大写再比，大小写不敏感（调用方给 ".GLSL" 也能对上）。 */
		std::string upper;
		upper.reserve(extension.size());
		for (const char character : extension)
			upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));

		static constexpr struct { const char* Extension; AssetFileKind Kind; } kByExtension[] = {
			/* 贴图：与纹理解码器（stb_image）能解的格式对齐 —— 网格缩略图与属性面板的
			 * 预览 / 尺寸行对每一种都出得来。.dds 也在贴图档（它确实是贴图容器），
			 * 但解码器解不出像素：浏览器对它回落成图片图标、详情卡没有尺寸行。 */
			{ ".PNG", AssetFileKind::Image }, { ".JPG", AssetFileKind::Image },
			{ ".JPEG", AssetFileKind::Image }, { ".BMP", AssetFileKind::Image },
			{ ".TGA", AssetFileKind::Image }, { ".PSD", AssetFileKind::Image },
			{ ".GIF", AssetFileKind::Image }, { ".HDR", AssetFileKind::Image },
			{ ".DDS", AssetFileKind::Image },
			/* 烘好的块数据：stb 解不了，但 Metal 后端能直接上传，缩略图与尺寸行都有 */
			{ ".ASTC", AssetFileKind::Image },
			/* 矢量图：.svg 是编辑器图标源，资源面板直接按文件渲染预览（不进纹理解码器） */
			{ ".SVG", AssetFileKind::Svg },
			{ ".SCN", AssetFileKind::Scene },
			{ ".MTLGRAPH", AssetFileKind::MtlGraph },
			/* 着色器：.glsl 是源，.metal 是同一次编译的产物（落在项目 Cache/Shaders 下）——
			 * 两样都是"一个着色器"，筛 Shaders 时一起出来。 */
			{ ".GLSL", AssetFileKind::Shader }, { ".METAL", AssetFileKind::Shader },
			/* 模型：源格式 + .mesh（几何 payload）+ .model（清单：几何 + 槽表） */
			{ ".OBJ", AssetFileKind::Model }, { ".MESH", AssetFileKind::Model }, { ".FBX", AssetFileKind::Model },
			{ ".GLTF", AssetFileKind::Model }, { ".GLB", AssetFileKind::Model },
			{ ".DAE", AssetFileKind::Model }, { ".STL", AssetFileKind::Model },
			{ ".MODEL", AssetFileKind::Model },
			/* 材质：一层 .mtl（单条材质资产） */
			{ ".MTL", AssetFileKind::Material },
			{ ".TERRAIN", AssetFileKind::Terrain },
			/* 反射探针的烘焙缓存（保存场景时落盘，加载时直接恢复） */
			{ ".PROBE", AssetFileKind::Probe },
			/* 动画片段（属性面板的 Animator 卡拖进去就能播） */
			{ ".ANIM", AssetFileKind::Animation },
			/* 运行时 UI：文档 / 样式 / 字符串表，三类都算"界面资产" */
			{ ".RML", AssetFileKind::UI }, { ".RCSS", AssetFileKind::UI }, { ".UILANG", AssetFileKind::UI },
			/* 脚本源码：项目 Scripts 下的 C++ 文件，浏览器里按头文件 / 源文件分两档 */
			{ ".H", AssetFileKind::CppHeader }, { ".HPP", AssetFileKind::CppHeader },
			{ ".CPP", AssetFileKind::CppSource }, { ".CC", AssetFileKind::CppSource },
		};

		for (const auto& entry : kByExtension)
		{
			if (upper == entry.Extension)
				return entry.Kind;
		}

		return AssetFileKind::Other;
	}

	std::string AssetRelativePath(const std::filesystem::path& path)
	{
		/* 引擎内置资源同样给相对路径：项目里放同名文件就能覆盖它 */
		return PathToUtf8(ToAssetRelative(path));
	}

	std::string AssetNameError(const std::string& name)
	{
		if (name.empty())
			return "Name cannot be empty";

		if (name == "." || name == "..")
			return "Invalid name";

		if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
			return "Name cannot contain path separators";

		/* 以 '.' 开头的东西资源树一律不列（见 BuildFileNodeTree），建了也看不见 */
		if (name.front() == '.')
			return "Name cannot start with '.'";

		/* Windows 上这些字符建不出文件；资源要跨平台，直接一起挡掉 */
		if (name.find_first_of("<>:\"|?*") != std::string::npos)
			return "Name cannot contain < > : \" | ? *";

		return {};
	}

	std::string AssetMoveError(const std::filesystem::path& from, const std::filesystem::path& target_dir)
	{
		std::error_code error;

		/* 移动不改名：落点就是"目标目录 / 源自己的名字" */
		if (from.empty() || PathToUtf8(from.filename()).empty())
			return "Source name is empty";

		if (!std::filesystem::exists(from, error) || error)
			return "Source no longer exists";

		if (!std::filesystem::is_directory(target_dir, error) || error)
			return "Target is not a folder";

		/* 拖回它所在的目录（含"拖到它自己身上"）= 原位放下，不算一次移动 */
		if (target_dir / from.filename() == from)
			return "Already in this folder";

		/* 不能把目录搬进它自己或它的子孙里（磁盘上必然失败，先挡下来） */
		if (IsPathPrefix(from, target_dir))
			return "Cannot move into itself";

		/* 目标里已有同名项：与重命名同一条原则 —— 不覆盖（换名字是用户自己的事） */
		if (std::filesystem::exists(target_dir / from.filename(), error) && !error)
			return "Target folder already has an item with the same name";

		return {};
	}

	std::filesystem::path MakeUniquePath(const std::filesystem::path& dir, const std::string& file_name)
	{
		/* 拆出主干与扩展名：序号要插在主干后面，`hero 2.png` 而不是 `hero.png 2` */
		const std::filesystem::path requested = PathFromUtf8(file_name);
		const std::string stem = PathToUtf8(requested.stem());
		const std::string extension = PathToUtf8(requested.extension());

		std::error_code error;
		std::filesystem::path candidate = dir / PathFromUtf8(stem + extension);

		for (int index = 2; std::filesystem::exists(candidate, error) && !error; ++index)
			candidate = dir / PathFromUtf8(stem + " " + std::to_string(index) + extension);

		return candidate;
	}

	std::string AssetFileTemplate(const std::string& extension, const std::string& stem)
	{
		/* 扩展名比较一律大小写不敏感：资源树认类型时也这么做（BuildFileNodeTree 把扩展名转大写） */
		std::string ext = extension;
		for (char& character : ext)
		{
			if (character >= 'A' && character <= 'Z')
				character = static_cast<char>(character - 'A' + 'a');
		}

		/* UI 文档：一份能直接挂到 Canvas 上的最小文档，样式从同名 .rcss 来 */
		if (ext == ".rml")
		{
			return "<rml>\n"
				"<head>\n"
				"\t<link type=\"text/css\" href=\"" + stem + ".rcss\" />\n"
				"\t<title>" + stem + "</title>\n"
				"</head>\n"
				"<body>\n"
				"\t<div id=\"root\">Hello UI</div>\n"
				"</body>\n"
				"</rml>\n";
		}

		/* UI 样式：只给 body 定基础字号与颜色，其余按块自己写 */
		if (ext == ".rcss")
		{
			return "body\n"
				"{\n"
				"\tdisplay: block;\n"
				"\tmargin: 0px;\n"
				"\twidth: 100%;\n"
				"\theight: 100%;\n"
				"\tfont-family: Open Sans;\n"
				"\tfont-size: 16dp;\n"
				"\tcolor: #f2f4f8;\n"
				"}\n";
		}

		/* 空场景：跟 Serializer 写出的"没有任何实体"的场景同构 —— 加载器只要求 <Scene>/<Entities>，
		 * 实体循环一次都不进。跟 File > New Scene 是同一个状态，只是这一步直接落盘。 */
		if (ext == ".scn")
		{
			return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				"<Scene>\n"
				"    <Entities>\n"
				"    </Entities>\n"
				"</Scene>\n";
		}

		/* 空材质图：节点与连线都为 0（Asset 里的样例文件就是这套元素：
		 * MaterialGraph 带 NodeCount / LinkCount，下面是 Nodes / Links 两个列表）。 */
		if (ext == ".mtlgraph")
		{
			return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				"<MaterialGraph NodeCount=\"0\" LinkCount=\"0\">\n"
				"    <Nodes>\n"
				"    </Nodes>\n"
				"    <Links>\n"
				"    </Links>\n"
				"</MaterialGraph>\n";
		}

		/* 材质资产：与内置白模同一套配置（PBRStandard.glsl + 默认贴图满配）——
		 * 零外部依赖、编辑器前向与延迟（经 G-Buffer 转换）都是正确外观，用户随后改参数 / 换贴图。 */
		if (ext == ".mtl")
		{
			return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				"<Material ShaderPath=\"Shaders/MaterialShaders/PBRStandard.glsl\">\n"
				"    <Parameters>\n"
				"        <ParamInfo Type=\"0\" Name=\"u_AlbedoTexture\" Value=\"Textures/White.png\"/>\n"
				"        <ParamInfo Type=\"0\" Name=\"u_SpecularTexture\" Value=\"Textures/Black.png\"/>\n"
				"        <ParamInfo Type=\"0\" Name=\"u_NormalTexture\" Value=\"Textures/normal.png\"/>\n"
				"        <ParamInfo Type=\"0\" Name=\"u_BumpTexture\" Value=\"Textures/Black.png\"/>\n"
				"        <ParamInfo Type=\"0\" Name=\"u_DisplacementTexture\" Value=\"Textures/Black.png\"/>\n"
				"        <ParamInfo Type=\"0\" Name=\"u_RoughnessTexture\" Value=\"Textures/White.png\"/>\n"
				"        <ParamInfo Type=\"0\" Name=\"u_MetallicTexture\" Value=\"Textures/Black.png\"/>\n"
				"        <ParamInfo Type=\"0\" Name=\"u_EmissiveTexture\" Value=\"Textures/Black.png\"/>\n"
				"        <ParamInfo Type=\"0\" Name=\"u_AmbientTexture\" Value=\"Textures/Black.png\"/>\n"
				"    </Parameters>\n"
				"</Material>\n";
		}

		/* 地形：二进制容器，给一份带平缓起伏的初始高度图（空阵列看不出是地形） */
		if (ext == ".terrain")
		{
			std::vector<uint8_t> bytes;
			const TerrainAsset::Data data = TerrainAsset::MakeDefault();
			if (!TerrainAsset::Encode(data, bytes))
				return std::string();

			return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		}

		/* 空动画：时长 1 秒、循环，没有轨道（挂到 Animator 上不报错，先给个可播的壳） */
		if (ext == ".anim")
		{
			return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				"<Animation Duration=\"1\" Loop=\"1\">\n"
				"</Animation>\n";
		}

		/* GLSL 着色器：两段式（顶点 + 片元），挂到材质上就能画出色块。
		 * 引擎统一块照 Uniforms.glsl 抄了一份，自包含才能放在项目任何目录下。 */
		if (ext == ".glsl")
		{
			return R"GLSL(#type vertex
#version 410 core

/* 引擎统一变量块（与 Builtin/Uniforms.glsl 同一份布局）：binding 0 视图、binding 1 物体 */
layout(std140, binding = 0) uniform ViewUniformBuffer
{
	mat4 u_ViewMat;
	mat4 u_ProjectionMat;
	mat4 u_ViewProjectionMat;
	vec3 u_ViewPos;
	uint u_FrameCounter;
};

layout(std140, binding = 1) uniform ObjectUniformBuffer
{
	mat4 u_Local2WorldMat;
	int  u_ObjectId;
	float u_ReceiveShadow;
};

/* 顶点属性按引擎约定：0 位置 / 1 法线 / 2 颜色 / 3 UV / 4 切线 */
layout(location = 0) in vec3 a_Position;

void main()
{
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);
}


#type fragment
#version 410 core

/* 物体块要在片元里再声明一次（同名同布局）；ObjectId 供视口点击拾取 */
layout(std140, binding = 1) uniform ObjectUniformBuffer
{
	mat4 u_Local2WorldMat;
	int  u_ObjectId;
	float u_ReceiveShadow;
};

/* 带 @property 的 uniform 会出现在材质面板；@type(color) 让 vec4 画成取色器 */
uniform vec4 u_Color = vec4(0.8f, 0.8f, 0.9f, 1.0f); // @property @type(color)

layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out float ObjectId;

void main()
{
	OutFragColor = u_Color;
	ObjectId = float(u_ObjectId);
}
)GLSL";
		}

		/* C++ 脚本：类名与文件名一致，建完直接重编就能挂到实体上 */
		if (ext == ".cpp")
		{
			const std::string name = ScriptClassName(stem);

			std::string content;
			content += "/* " + name + ": script template. OnUpdate runs once per frame in Runtime mode. */\n";
			content += "#include \"Helios/Script/ScriptRegistry.h\"\n";
			content += "#include \"Helios/Scene/Components.h\"\n";
			content += "\n";
			content += "namespace\n";
			content += "{\n";
			content += "\tclass " + name + " : public Helios::Script\n";
			content += "\t{\n";
			content += "\tpublic:\n";
			content += "\t\tvoid OnUpdate(float delta_time) override\n";
			content += "\t\t{\n";
			content += "\t\t\t(void)delta_time;\n";
			content += "\t\t}\n";
			content += "\n";
			content += "\t\tfloat m_Speed{ 1.0f };\n";
			content += "\t};\n";
			content += "}\n";
			content += "\n";
			content += "HELIOS_SCRIPT(" + name + ",\n";
			content += "\tHELIOS_SCRIPT_FIELD(&" + name + "::m_Speed, \"Speed\", Helios::FieldType::Float))\n";
			return content;
		}

		/* 认不出的（.txt / .json …）：空文件。模板内容不该由我们凭空发明，
		 * 用户要什么自己写 —— 至少文件已经在正确的目录里了。 */
		return {};
	}

	bool MoveAssetToTrash(const std::filesystem::path& trash_root, const std::filesystem::path& target,
		std::filesystem::path& out_trashed, std::string& out_error)
	{
		std::error_code error;
		if (!std::filesystem::exists(target, error) || error)
		{
			out_error = "Not found: " + PathToUtf8(target);
			return false;
		}

		std::filesystem::create_directories(trash_root, error);
		if (error)
		{
			out_error = "Cannot create trash folder " + PathToUtf8(trash_root) + ": " + error.message();
			return false;
		}

		const std::filesystem::path destination = MakeUniquePath(trash_root, TrashNameFor(target));

		std::filesystem::rename(target, destination, error);
		if (error)
		{
			/* 跨盘（回收站与资源不在同一个卷）搬不动：说清原因，别装作删掉了 */
			out_error = "Move to trash failed: " + error.message();
			return false;
		}

		/* 贴图设置跟着进回收站；落点用 "回收站文件 + .texmeta"，撤销时按同样规则找回 */
		const std::filesystem::path target_meta = PathFromUtf8(TextureSettingsStore::MetaPath(PathToUtf8(target)));
		std::error_code meta_error;
		if (std::filesystem::is_regular_file(target_meta, meta_error))
		{
			const std::filesystem::path trashed_meta = PathFromUtf8(TextureSettingsStore::MetaPath(PathToUtf8(destination)));
			std::filesystem::rename(target_meta, trashed_meta, meta_error);
			if (meta_error)
				CORE_LOG_WARN("Failed to move texture settings to trash: {0} ({1})",
					PathToUtf8(target_meta), meta_error.message());
		}

		out_trashed = destination;
		return true;
	}

	bool RestoreAssetFromTrash(const std::filesystem::path& trashed, const std::filesystem::path& target,
		std::string& out_error)
	{
		std::error_code error;
		if (!std::filesystem::exists(trashed, error) || error)
		{
			out_error = "Not in trash: " + PathToUtf8(trashed);
			return false;
		}

		std::filesystem::create_directories(target.parent_path(), error);

		std::filesystem::rename(trashed, target, error);
		if (error)
		{
			out_error = "Restore from trash failed: " + error.message();
			return false;
		}

		/* 回收站里的伴生设置一起搬回 */
		const std::filesystem::path trashed_meta = PathFromUtf8(TextureSettingsStore::MetaPath(PathToUtf8(trashed)));
		std::error_code meta_error;
		if (std::filesystem::is_regular_file(trashed_meta, meta_error))
		{
			const std::filesystem::path target_meta = PathFromUtf8(TextureSettingsStore::MetaPath(PathToUtf8(target)));
			std::filesystem::rename(trashed_meta, target_meta, meta_error);
			if (meta_error)
				CORE_LOG_WARN("Failed to restore texture settings: {0} ({1})",
					PathToUtf8(trashed_meta), meta_error.message());
		}

		return true;
	}

	const std::vector<std::string>& CollectShaderOptions()
	{
		ShaderOptionCache& cache = GetShaderOptionCache();

		/* 换工程时清单里的相对路径已经不对了：按根路径再比一次，不同就重扫 */
		if (!cache.Dirty && cache.ProjectRoot == g_ProjectPath)
			return cache.Options;

		cache.ProjectRoot = g_ProjectPath;
		cache.Dirty = false;
		cache.Options.clear();

		/* 工程根整棵树都算（shader 放哪一层都能选到），引擎根只收 MaterialShaders */
		ScanShaderOptions(g_ProjectPath, {}, cache.Options);
		ScanShaderOptions(g_EngineResPath, kEngineMaterialShaderDir, cache.Options);

		std::sort(cache.Options.begin(), cache.Options.end());
		return cache.Options;
	}

	void InvalidateShaderOptions()
	{
		GetShaderOptionCache().Dirty = true;
	}
}
