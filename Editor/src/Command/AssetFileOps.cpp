#include "Pch.h"
#include "Command/AssetFileOps.h"

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
	}

	std::filesystem::path AssetTrashRoot()
	{
		/* 与 Assets 平级：不进资源树（浏览器只列 Assets 下面的东西），也不会被打进包 */
		return g_AssetsPath.parent_path() / ".helios-trash";
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
