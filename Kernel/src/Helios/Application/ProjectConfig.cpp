#include "Pch.h"
#include "ProjectConfig.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Scene/GrassField.h"
#include "Helios/Scene/SkeletonModel.h"
#include "Helios/Scene/TerrainVirtualTexture.h"
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		constexpr const char* kProjectFile = "Project.xml";
		constexpr const char* kRenderingElement = "Rendering";

		/* 引擎当前值：文件里没写的项就用它，老项目不会因为缺字段变样 */
		ProjectRendering CaptureRendering()
		{
			ProjectRendering rendering{};
			rendering.MSAA = Renderer::IsMSAAEnabled();
			rendering.GPUSkinning = SkeletonModel::IsGlobalGpuSkinningEnabled();
			rendering.GpuGrassPlacement = GrassField::IsGlobalGpuPlacementEnabled();
			rendering.GpuGrassIndirect = GrassField::IsGlobalIndirectDrawEnabled();
			rendering.GpuGrassChunkMask = GrassField::IsGlobalChunkMaskEnabled();
			rendering.GrassBatching = GrassField::IsGlobalBatchingEnabled();
			rendering.GrassDensityScale = GrassField::GetGlobalDensityScale();
			rendering.TerrainVirtualTexture = TerrainVirtualTexture::IsEnabled();
			rendering.TerrainVirtualTextureTexels = TerrainVirtualTexture::GetPageTexels();
			return rendering;
		}

		void ReadBool(const tinyxml2::XMLElement* element, const char* name, bool& target)
		{
			bool value = false;
			if (element->QueryBoolAttribute(name, &value) == tinyxml2::XML_SUCCESS)
				target = value;
		}

		void ReadFloat(const tinyxml2::XMLElement* element, const char* name, float& target)
		{
			float value = 0.0f;
			if (element->QueryFloatAttribute(name, &value) == tinyxml2::XML_SUCCESS)
				target = value;
		}

		void ReadUint(const tinyxml2::XMLElement* element, const char* name, uint32_t& target)
		{
			unsigned value = 0;
			if (element->QueryUnsignedAttribute(name, &value) == tinyxml2::XML_SUCCESS)
				target = value;
		}

		void WriteRendering(tinyxml2::XMLElement* element, const ProjectRendering& rendering)
		{
			element->SetAttribute("MSAA", rendering.MSAA ? 1 : 0);
			element->SetAttribute("GPUSkinning", rendering.GPUSkinning ? 1 : 0);
			element->SetAttribute("GpuGrassPlacement", rendering.GpuGrassPlacement ? 1 : 0);
			element->SetAttribute("GpuGrassIndirect", rendering.GpuGrassIndirect ? 1 : 0);
			element->SetAttribute("GpuGrassChunkMask", rendering.GpuGrassChunkMask ? 1 : 0);
			element->SetAttribute("GrassBatching", rendering.GrassBatching ? 1 : 0);
			element->SetAttribute("GrassDensityScale", rendering.GrassDensityScale);
			element->SetAttribute("TerrainVirtualTexture", rendering.TerrainVirtualTexture ? 1 : 0);
			element->SetAttribute("TerrainVirtualTextureTexels", rendering.TerrainVirtualTextureTexels);
		}
	}

	ProjectConfig LoadProjectConfig()
	{
		return LoadProjectConfig(g_ProjectPath);
	}

	ProjectConfig LoadProjectConfig(const std::filesystem::path& project_dir)
	{
		ProjectConfig config;
		config.Name = PathToUtf8(project_dir.filename());
		config.Rendering = CaptureRendering();

		const std::filesystem::path file = project_dir / kProjectFile;
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

		if (const tinyxml2::XMLElement* rendering = root->FirstChildElement(kRenderingElement))
		{
			ReadBool(rendering, "MSAA", config.Rendering.MSAA);
			ReadBool(rendering, "GPUSkinning", config.Rendering.GPUSkinning);
			ReadBool(rendering, "GpuGrassPlacement", config.Rendering.GpuGrassPlacement);
			ReadBool(rendering, "GpuGrassIndirect", config.Rendering.GpuGrassIndirect);
			ReadBool(rendering, "GpuGrassChunkMask", config.Rendering.GpuGrassChunkMask);
			ReadBool(rendering, "GrassBatching", config.Rendering.GrassBatching);
			ReadFloat(rendering, "GrassDensityScale", config.Rendering.GrassDensityScale);
			ReadBool(rendering, "TerrainVirtualTexture", config.Rendering.TerrainVirtualTexture);
			ReadUint(rendering, "TerrainVirtualTextureTexels", config.Rendering.TerrainVirtualTextureTexels);
		}

		return config;
	}

	bool SaveProjectConfig(const ProjectConfig& config)
	{
		if (g_ProjectPath.empty())
			return false;

		const std::filesystem::path file = g_ProjectPath / kProjectFile;

		/* 读改写：菜单只动画质开关，文件里别的字段和内容都不碰 */
		tinyxml2::XMLDocument document;
		bool parsed = false;
		if (FILE* handle = OpenUtf8File(file, "rb"))
		{
			parsed = document.LoadFile(handle) == tinyxml2::XML_SUCCESS;
			std::fclose(handle);
		}

		if (!parsed)
		{
			if (std::filesystem::is_regular_file(file))
				CORE_LOG_WARN("Project config is not readable, rewriting it: {}", PathToUtf8(file));

			document.Clear();
			document.InsertEndChild(document.NewDeclaration());
		}

		tinyxml2::XMLElement* root = document.RootElement();
		if (root == nullptr)
		{
			root = document.NewElement("Project");
			document.InsertEndChild(root);
		}

		root->SetAttribute("Name", config.Name.c_str());
		if (config.StartupScene.empty())
			root->DeleteAttribute("StartupScene");
		else
			root->SetAttribute("StartupScene", config.StartupScene.c_str());

		tinyxml2::XMLElement* rendering = root->FirstChildElement(kRenderingElement);
		if (rendering == nullptr)
		{
			rendering = document.NewElement(kRenderingElement);
			root->InsertEndChild(rendering);
		}
		WriteRendering(rendering, config.Rendering);

		FILE* handle = OpenUtf8File(file, "wb");
		if (handle == nullptr)
		{
			CORE_LOG_ERROR("Failed to write project config: {}", PathToUtf8(file));
			return false;
		}

		const bool saved = document.SaveFile(handle) == tinyxml2::XML_SUCCESS;
		std::fclose(handle);
		return saved;
	}

	void ApplyProjectSettings(const ProjectConfig& config)
	{
		const ProjectRendering& rendering = config.Rendering;
		Renderer::SetMSAAEnabled(rendering.MSAA);
		SkeletonModel::SetGlobalGpuSkinningEnabled(rendering.GPUSkinning);
		GrassField::SetGlobalGpuPlacementEnabled(rendering.GpuGrassPlacement);
		GrassField::SetGlobalIndirectDrawEnabled(rendering.GpuGrassIndirect);
		GrassField::SetGlobalChunkMaskEnabled(rendering.GpuGrassChunkMask);
		GrassField::SetGlobalBatchingEnabled(rendering.GrassBatching);
		GrassField::SetGlobalDensityScale(rendering.GrassDensityScale);
		TerrainVirtualTexture::SetEnabled(rendering.TerrainVirtualTexture);
		TerrainVirtualTexture::SetPageTexels(rendering.TerrainVirtualTextureTexels);
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

		const std::filesystem::path file = project_dir / kProjectFile;
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
