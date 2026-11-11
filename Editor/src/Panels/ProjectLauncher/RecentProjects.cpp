#include "Pch.h"
#include "RecentProjects.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Common/PathUtils.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace Helios
{
	namespace
	{
		constexpr size_t kMaxRecentProjects = 10;

		/* 用户级状态目录：Windows 走 %APPDATA%/Helios，macOS / Linux 走 ~/.helios */
		std::filesystem::path UserStateDirectory()
		{
#if defined(PLATFORM_WINDOWS)
			const char* base = std::getenv("APPDATA");
			const char* folder = "Helios";
#else
			const char* base = std::getenv("HOME");
			const char* folder = ".helios";
#endif
			if (base == nullptr || *base == '\0')
				return {};

			return PathFromUtf8(base) / folder;
		}

		std::filesystem::path ListFile()
		{
			const std::filesystem::path directory = UserStateDirectory();
			if (directory.empty())
				return {};

			return directory / "recent_projects.txt";
		}

		/* 统一成绝对路径：列表里存的是什么，下次就按什么开 */
		std::filesystem::path Normalize(const std::filesystem::path& path)
		{
			std::error_code error;
			const std::filesystem::path absolute = std::filesystem::weakly_canonical(path, error);
			if (!error)
				return absolute;

			std::filesystem::path fallback = std::filesystem::absolute(path, error);
			return error ? path : fallback;
		}

		bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b)
		{
			return a.lexically_normal() == b.lexically_normal();
		}

		/* 第一次运行时的默认列表：引擎自带的 Projects/ 下每个带 Project.xml 的目录 */
		void SeedFromEngineProjects(std::vector<std::filesystem::path>& items)
		{
			const std::filesystem::path projects = GetEngineProjectsDirectory();

			std::error_code error;
			if (!std::filesystem::is_directory(projects, error))
				return;

			for (const auto& entry : std::filesystem::directory_iterator(
				projects, std::filesystem::directory_options::skip_permission_denied, error))
			{
				if (!entry.is_directory(error))
					continue;
				if (!std::filesystem::is_regular_file(entry.path() / "Project.xml", error))
					continue;

				items.push_back(Normalize(entry.path()));
			}

			std::sort(items.begin(), items.end());
		}
	}

	void RecentProjects::Load()
	{
		m_Items.clear();

		const std::filesystem::path file = ListFile();
		const bool has_file = !file.empty() && std::filesystem::is_regular_file(file);

		if (has_file)
		{
			FILE* handle = OpenUtf8File(file, "rb");
			if (handle != nullptr)
			{
				char line[4096];
				while (m_Items.size() < kMaxRecentProjects && std::fgets(line, sizeof(line), handle) != nullptr)
				{
					std::string text(line);
					while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
						text.pop_back();
					if (text.empty())
						continue;

					std::filesystem::path path;
					if (TryPathFromUtf8(text, path))
						m_Items.push_back(Normalize(path));
				}
				std::fclose(handle);
			}
		}
		else
		{
			SeedFromEngineProjects(m_Items);
			if (!m_Items.empty())
				Save();
		}
	}

	void RecentProjects::Add(const std::filesystem::path& project_dir)
	{
		if (project_dir.empty())
			return;

		const std::filesystem::path path = Normalize(project_dir);

		std::erase_if(m_Items, [&path](const std::filesystem::path& item) { return SamePath(item, path); });
		m_Items.insert(m_Items.begin(), path);

		if (m_Items.size() > kMaxRecentProjects)
			m_Items.resize(kMaxRecentProjects);

		Save();
	}

	void RecentProjects::Remove(const std::filesystem::path& project_dir)
	{
		const std::filesystem::path path = Normalize(project_dir);
		const size_t before = m_Items.size();

		std::erase_if(m_Items, [&path](const std::filesystem::path& item) { return SamePath(item, path); });
		if (m_Items.size() != before)
			Save();
	}

	void RecentProjects::Save() const
	{
		const std::filesystem::path file = ListFile();
		if (file.empty())
			return;

		std::error_code error;
		std::filesystem::create_directories(file.parent_path(), error);

		FILE* handle = OpenUtf8File(file, "wb");
		if (handle == nullptr)
			return;

		for (const std::filesystem::path& item : m_Items)
		{
			const std::string line = PathToUtf8(item);
			std::fwrite(line.data(), 1, line.size(), handle);
			std::fputc('\n', handle);
		}

		std::fclose(handle);
	}
}
