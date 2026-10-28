#pragma once

#include <filesystem>
#include <string>

namespace Helios
{
	/* 项目描述：<项目根>/Project.xml
	 * 只放跨机器要对齐的少量字段，其余（布局、缓存）都由项目目录自己承载。 */
	struct ProjectConfig
	{
		std::string Name;
		std::string StartupScene; /* 相对项目根，空表示不自动打开 */
	};

	/* 读当前项目的 Project.xml；文件缺失或解析失败时用目录名兜底，不影响启动 */
	ProjectConfig LoadProjectConfig();

	/* 读指定目录的 Project.xml（启动页列最近工程时用，不必先切项目根） */
	ProjectConfig LoadProjectConfig(const std::filesystem::path& project_dir);

	/* 新建工程：建目录并写一份最小 Project.xml。目录已存在返回 false，不动现有内容。 */
	bool CreateProject(const std::filesystem::path& project_dir, const std::string& name);
}
