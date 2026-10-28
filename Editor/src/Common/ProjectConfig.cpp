#include "Pch.h"
#include "ProjectConfig.h"
#include "Helios/Common/AssetPath.h"
#include <tinyxml2.h>

namespace Helios
{
	ProjectConfig LoadProjectConfig()
	{
		return LoadProjectConfig(g_ProjectPath);
	}

	ProjectConfig LoadProjectConfig(const std::filesystem::path& project_dir)
	{
		ProjectConfig config;
		config.Name = PathToUtf8(project_dir.filename());

		const std::filesystem::path file = project_dir / "Project.xml";
		if (!std::filesystem::is_regular_file(file))
			return config;

		tinyxml2::XMLDocument document;
		FILE* file_handle = OpenUtf8File(file, "rb");
		if (file_handle == nullptr)
		{
			CORE_LOG_WARN("Project config is not readable: {}", PathToUtf8(file));
			return config;
		}

		const tinyxml2::XMLError result = document.LoadFile(file_handle);
		std::fclose(file_handle);
		if (result != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_WARN("Project config is not valid XML: {}", PathToUtf8(file));
			return config;
		}

		const tinyxml2::XMLElement* root = document.RootElement();
		if (root == nullptr)
			return config;

		if (const char* name = root->Attribute("Name"); name != nullptr && name[0] != '\0')
			config.Name = name;

		if (const char* scene = root->Attribute("StartupScene"); scene != nullptr)
			config.StartupScene = scene;

		return config;
	}

	bool CreateProject(const std::filesystem::path& project_dir, const std::string& name)
	{
		if (project_dir.empty() || name.empty())
			return false;

		std::error_code error;
		if (std::filesystem::exists(project_dir, error))
			return false;

		std::filesystem::create_directories(project_dir, error);
		if (error)
		{
			CORE_LOG_ERROR("Failed to create project directory: {}", PathToUtf8(project_dir));
			return false;
		}

		/* Scripts 先建出来：新工程往里放 .cpp 就有地方放，脚本模块也按它找源文件 */
		std::filesystem::create_directories(project_dir / "Scripts", error);

		tinyxml2::XMLDocument document;
		document.InsertEndChild(document.NewDeclaration());

		tinyxml2::XMLElement* root = document.NewElement("Project");
		root->SetAttribute("Name", name.c_str());
		document.InsertEndChild(root);

		const std::filesystem::path file = project_dir / "Project.xml";
		FILE* file_handle = OpenUtf8File(file, "wb");
		if (file_handle == nullptr)
		{
			CORE_LOG_ERROR("Failed to write project config: {}", PathToUtf8(file));
			return false;
		}

		const bool saved = document.SaveFile(file_handle) == tinyxml2::XML_SUCCESS;
		std::fclose(file_handle);

		if (saved)
			CORE_LOG_INFO("Project created: {}", PathToUtf8(project_dir));

		return saved;
	}
}
