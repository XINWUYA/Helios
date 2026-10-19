#include "Pch.h"
#include "ModelManifest.h"
#include "SceneCommon.h"
#include <fstream>
#include <iterator>
#include <tinyxml2.h>

namespace Helios::ModelManifest
{
	namespace
	{
		bool HasOverrideBody(const tinyxml2::XMLElement* element)
		{
			return element->BoolAttribute("Override")
				|| element->FirstChildElement("Parameters") != nullptr
				|| element->FirstChildElement("RasterState") != nullptr;
		}
	}

	bool Read(const std::string& path, std::string& out_geometry, std::vector<Slot>& out_slots)
	{
		const std::string absolute = ABSOLUTE_PATH(path);

		std::ifstream stream(PathFromUtf8(absolute), std::ios::binary);
		if (!stream)
		{
			CORE_LOG_ERROR("Failed to open model manifest: {}.", absolute);
			return false;
		}

		const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		tinyxml2::XMLDocument document;
		if (document.Parse(text.c_str()) != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_ERROR("Bad model manifest: {}.", absolute);
			return false;
		}

		const tinyxml2::XMLElement* root = document.FirstChildElement("Model");
		if (root == nullptr)
		{
			CORE_LOG_ERROR("Missing <Model> root: {}.", absolute);
			return false;
		}

		const char* geometry = root->Attribute("Geometry");
		if (geometry == nullptr || *geometry == '\0')
		{
			CORE_LOG_ERROR("Model manifest has no geometry: {}.", absolute);
			return false;
		}
		out_geometry = ABSOLUTE_PATH(geometry);

		for (const tinyxml2::XMLElement* element = root->FirstChildElement("Material");
			element != nullptr; element = element->NextSiblingElement("Material"))
		{
			Slot slot;
			if (const char* name = element->Attribute("Slot"))
				slot.Name = name;
			if (slot.Name.empty())
				slot.Name = "slot_" + std::to_string(out_slots.size());

			const char* asset = element->Attribute("Asset");
			if (asset != nullptr && *asset != '\0')
			{
				slot.AssetPath = ABSOLUTE_PATH(asset);
				if (HasOverrideBody(element))
				{
					auto override_material = CreateSharedPtr<Material>();
					if (MaterialIO::ReadBody(element, *override_material))
					{
						slot.pOverride = std::move(override_material);
						slot.HasRasterOverride = element->FirstChildElement("RasterState") != nullptr;
					}
				}
			}
			else
			{
				auto embedded = CreateSharedPtr<Material>();
				if (MaterialIO::ReadBody(element, *embedded))
					slot.pEmbedded = std::move(embedded);
			}

			out_slots.push_back(std::move(slot));
		}

		return true;
	}

	bool Write(const std::string& path, const std::string& geometry_path, const std::vector<Slot>& slots)
	{
		const std::string absolute = ABSOLUTE_PATH(path);

		const std::filesystem::path parent = PathFromUtf8(absolute).parent_path();
		if (!parent.empty())
		{
			std::error_code error;
			std::filesystem::create_directories(parent, error);
		}

		tinyxml2::XMLDocument document;
		document.InsertEndChild(document.NewDeclaration());
		auto* root = document.NewElement("Model");
		root->SetAttribute("Geometry", RELATIVE_PATH(geometry_path).c_str());
		document.InsertEndChild(root);

		for (const Slot& slot : slots)
		{
			if (slot.AssetPath.empty() && slot.pEmbedded == nullptr)
				continue;

			auto* element = root->InsertNewChildElement("Material");
			element->SetAttribute("Slot", slot.Name.c_str());

			if (!slot.AssetPath.empty())
			{
				element->SetAttribute("Asset", RELATIVE_PATH(slot.AssetPath).c_str());
				if (slot.pOverride != nullptr)
				{
					element->SetAttribute("Override", true);
					MaterialIO::WriteBody(element, *slot.pOverride, slot.HasRasterOverride);
				}
			}
			else
			{
				MaterialIO::WriteBody(element, *slot.pEmbedded);
			}
		}

		auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(
			OpenUtf8File(PathFromUtf8(absolute), "wb"), &std::fclose);
		if (!file)
		{
			CORE_LOG_ERROR("Failed to open model manifest for writing: {}.", absolute);
			return false;
		}

		if (document.SaveFile(file.get()) != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_ERROR("Failed to write model manifest: {}.", absolute);
			return false;
		}
		return true;
	}
}
