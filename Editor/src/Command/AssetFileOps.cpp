#include "Pch.h"
#include "Command/AssetFileOps.h"
#include <cctype>

namespace Helios
{
	namespace
	{
		/* 回收站里的文件名：在 Assets 内就带上相对路径（把分隔符换成 '_'，看得出它原来在哪），
		 * 否则只用文件名。人工去回收站里翻的时候认得出来。 */
		std::string TrashNameFor(const std::filesystem::path& target)
		{
			std::error_code error;
			const std::filesystem::path relative = std::filesystem::relative(target, g_AssetsPath, error);

			std::string name;
			if (!error && !relative.empty() && relative.native().rfind("..", 0) != 0)
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
	}

	std::filesystem::path AssetTrashRoot()
	{
		/* 与 Assets 平级：不进资源树（浏览器只列 Assets 下面的东西），也不会被打进包 */
		return g_AssetsPath.parent_path() / ".helios-trash";
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
			{ ".SCN", AssetFileKind::Scene },
			{ ".MTLGRAPH", AssetFileKind::MtlGraph },
			/* 着色器：.glsl 是源，.metal 是同一次编译的产物（落在 Assets/Cache/Shaders 下）——
			 * 两样都是"一个着色器"，筛 Shaders 时一起出来。 */
			{ ".GLSL", AssetFileKind::Shader }, { ".METAL", AssetFileKind::Shader },
			/* 模型：引擎自己只导 .obj（tinyobjloader），.mesh 是它旁边那份二进制网格缓存；
			 * .fbx / .dae / .stl 是随模型一起带进来的交换格式 —— 都算"这个模型的文件"。 */
			{ ".OBJ", AssetFileKind::Model }, { ".MESH", AssetFileKind::Model }, { ".FBX", AssetFileKind::Model },
			{ ".DAE", AssetFileKind::Model }, { ".STL", AssetFileKind::Model },
			/* 材质资产：.mtl 既是独立单条目材质资产，也是模型伴生的槽表（同一份 schema） */
			{ ".MTL", AssetFileKind::Material },
			/* 反射探针的烘焙缓存（保存场景时落盘，加载时直接恢复） */
			{ ".PROBE", AssetFileKind::Probe },
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
		std::error_code error;
		const std::filesystem::path relative = std::filesystem::relative(path, g_AssetsPath, error);

		if (!error && !relative.empty() && relative.native().rfind("..", 0) != 0)
			return PathToUtf8(relative);

		return PathToUtf8(path);
	}

	std::string AssetNameError(const std::string& name)
	{
		if (name.empty())
			return "名字不能为空";

		if (name == "." || name == "..")
			return "这个名字不合法";

		if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
			return "名字里不能有路径分隔符";

		/* 以 '.' 开头的东西资源树一律不列（见 BuildFileNodeTree），建了也看不见 */
		if (name.front() == '.')
			return "名字不能以 '.' 开头";

		/* Windows 上这些字符建不出文件；资源要跨平台，直接一起挡掉 */
		if (name.find_first_of("<>:\"|?*") != std::string::npos)
			return "名字里不能有 < > : \" | ? * 这些字符";

		return {};
	}

	std::string AssetMoveError(const std::filesystem::path& from, const std::filesystem::path& target_dir)
	{
		std::error_code error;

		/* 移动不改名：落点就是"目标目录 / 源自己的名字" */
		if (from.empty() || PathToUtf8(from.filename()).empty())
			return "源的名字为空";

		if (!std::filesystem::exists(from, error) || error)
			return "源已经不在了";

		if (!std::filesystem::is_directory(target_dir, error) || error)
			return "目标不是目录";

		/* 拖回它所在的目录（含"拖到它自己身上"）= 原位放下，不算一次移动 */
		if (target_dir / from.filename() == from)
			return "它已经在这个目录里";

		/* 不能把目录搬进它自己或它的子孙里（磁盘上必然失败，先挡下来） */
		if (IsPathPrefix(from, target_dir))
			return "不能移动到自身内部";

		/* 目标里已有同名项：与重命名同一条原则 —— 不覆盖（换名字是用户自己的事） */
		if (std::filesystem::exists(target_dir / from.filename(), error) && !error)
			return "目标目录里已有同名项";

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

	std::string AssetFileTemplate(const std::string& extension)
	{
		/* 扩展名比较一律大小写不敏感：资源树认类型时也这么做（BuildFileNodeTree 把扩展名转大写） */
		std::string ext = extension;
		for (char& character : ext)
		{
			if (character >= 'A' && character <= 'Z')
				character = static_cast<char>(character - 'A' + 'a');
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

		/* 单材质资产：与内置白模同一套配置（GBufferMaterial.glsl + 默认贴图满配）——
		 * 零外部依赖、编辑器延迟与前向两条路径都直接可预览，用户随后改参数 / 换贴图。 */
		if (ext == ".mtl")
		{
			return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				"<Materials Count=\"1\">\n"
				"    <Material ID=\"0\" ShaderPath=\"Shaders/DeferredShaders/GBufferMaterial.glsl\">\n"
				"        <Parameters>\n"
				"            <ParamInfo Type=\"0\" Name=\"u_AlbedoTexture\"><Texture Path=\"Textures/White.png\" Slot=\"0\"><LoadConfig IsFlipV=\"false\" IsGenMips=\"true\" SamplerType=\"0\"/></Texture></ParamInfo>\n"
				"            <ParamInfo Type=\"0\" Name=\"u_SpecularTexture\"><Texture Path=\"Textures/Black.png\" Slot=\"1\"><LoadConfig IsFlipV=\"false\" IsGenMips=\"true\" SamplerType=\"0\"/></Texture></ParamInfo>\n"
				"            <ParamInfo Type=\"0\" Name=\"u_NormalTexture\"><Texture Path=\"Textures/normal.png\" Slot=\"2\"><LoadConfig IsFlipV=\"false\" IsGenMips=\"true\" SamplerType=\"0\"/></Texture></ParamInfo>\n"
				"            <ParamInfo Type=\"0\" Name=\"u_BumpTexture\"><Texture Path=\"Textures/Black.png\" Slot=\"3\"><LoadConfig IsFlipV=\"false\" IsGenMips=\"true\" SamplerType=\"0\"/></Texture></ParamInfo>\n"
				"            <ParamInfo Type=\"0\" Name=\"u_DisplacementTexture\"><Texture Path=\"Textures/Black.png\" Slot=\"4\"><LoadConfig IsFlipV=\"false\" IsGenMips=\"true\" SamplerType=\"0\"/></Texture></ParamInfo>\n"
				"            <ParamInfo Type=\"0\" Name=\"u_RoughnessTexture\"><Texture Path=\"Textures/White.png\" Slot=\"5\"><LoadConfig IsFlipV=\"false\" IsGenMips=\"true\" SamplerType=\"0\"/></Texture></ParamInfo>\n"
				"            <ParamInfo Type=\"0\" Name=\"u_MetallicTexture\"><Texture Path=\"Textures/Black.png\" Slot=\"6\"><LoadConfig IsFlipV=\"false\" IsGenMips=\"true\" SamplerType=\"0\"/></Texture></ParamInfo>\n"
				"            <ParamInfo Type=\"0\" Name=\"u_EmissiveTexture\"><Texture Path=\"Textures/Black.png\" Slot=\"7\"><LoadConfig IsFlipV=\"false\" IsGenMips=\"true\" SamplerType=\"0\"/></Texture></ParamInfo>\n"
				"            <ParamInfo Type=\"0\" Name=\"u_AmbientTexture\"><Texture Path=\"Textures/Black.png\" Slot=\"8\"><LoadConfig IsFlipV=\"false\" IsGenMips=\"true\" SamplerType=\"0\"/></Texture></ParamInfo>\n"
				"        </Parameters>\n"
				"    </Material>\n"
				"</Materials>\n";
		}

		/* 认不出的（.glsl / .txt / .mat …）：空文件。模板内容不该由我们凭空发明，
		 * 用户要什么自己写 —— 至少文件已经在正确的目录里了。 */
		return {};
	}

	bool MoveAssetToTrash(const std::filesystem::path& trash_root, const std::filesystem::path& target,
		std::filesystem::path& out_trashed, std::string& out_error)
	{
		std::error_code error;
		if (!std::filesystem::exists(target, error) || error)
		{
			out_error = "找不到它：" + PathToUtf8(target);
			return false;
		}

		std::filesystem::create_directories(trash_root, error);
		if (error)
		{
			out_error = "建不了回收站目录 " + PathToUtf8(trash_root) + "：" + error.message();
			return false;
		}

		const std::filesystem::path destination = MakeUniquePath(trash_root, TrashNameFor(target));

		std::filesystem::rename(target, destination, error);
		if (error)
		{
			/* 跨盘（回收站与资源不在同一个卷）搬不动：说清原因，别装作删掉了 */
			out_error = "移到回收站失败：" + error.message();
			return false;
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
			out_error = "回收站里已经没有它了：" + PathToUtf8(trashed);
			return false;
		}

		std::filesystem::create_directories(target.parent_path(), error);

		std::filesystem::rename(trashed, target, error);
		if (error)
		{
			out_error = "从回收站搬回来失败：" + error.message();
			return false;
		}

		return true;
	}
}
