#pragma once
#include <filesystem>
#include <vector>

namespace Helios
{
	/* 最近打开的工程：跨项目，存用户目录（Windows 用 %APPDATA%/Helios，其余 ~/.helios）。
	 * 只记路径，工程名每次从 Project.xml 现读 —— 用户改了名字，列表跟着变。 */
	class RecentProjects
	{
	public:
		/* 读列表；文件还不存在时用引擎自带的 Projects/ 打底（第一次启动就有东西可点） */
		void Load();
		/* 记一笔（移到最前、去重、只留最近若干条）并落盘 */
		void Add(const std::filesystem::path& project_dir);
		/* 只从列表里划掉，不动磁盘上的工程 */
		void Remove(const std::filesystem::path& project_dir);

		[[nodiscard]] const std::vector<std::filesystem::path>& Items() const { return m_Items; }

	private:
		void Save() const;

		std::vector<std::filesystem::path> m_Items;
	};
}
