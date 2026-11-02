#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/SceneCommon.h"
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		/* 材质是资源引用：字段表只认 POD，路径与实例内嵌单独读写，
		 * 两种形态与模型槽 / 草地槽一致（引用只写路径，实例全量内嵌） */
		void SaveTerrainMaterial(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& slot = static_cast<const TerrainComponent*>(component)->m_MaterialSlot;
			if (slot.pMaterial == nullptr)
				return;

			if (slot.IsInstance)
			{
				auto* instance_doc = element->InsertNewChildElement("MaterialOverride");
				instance_doc->SetAttribute("Instance", true);
				if (!slot.SourceAssetPath.empty())
					instance_doc->SetAttribute("Source", RELATIVE_PATH(slot.SourceAssetPath).c_str());
				MaterialIO::WriteBody(instance_doc, *slot.pMaterial);
				return;
			}

			const std::string& path = !slot.SourceAssetPath.empty()
				? slot.SourceAssetPath : slot.pMaterial->GetPath();
			if (!path.empty())
				element->SetAttribute("Material", RELATIVE_PATH(path).c_str());
		}

		void LoadTerrainMaterial(const tinyxml2::XMLElement* element, void* component)
		{
			auto& terrain = *static_cast<TerrainComponent*>(component);
			terrain.m_MaterialSlot = MaterialSlotOverride{};

			for (const tinyxml2::XMLElement* instance_doc = element->FirstChildElement("MaterialOverride");
				instance_doc != nullptr; instance_doc = instance_doc->NextSiblingElement("MaterialOverride"))
			{
				if (!instance_doc->BoolAttribute("Instance"))
					continue;

				auto instance = CreateSharedPtr<Material>();
				if (!MaterialIO::ReadBody(instance_doc, *instance))
				{
					CORE_LOG_WARN("Failed to read inline terrain material, falling back to the asset reference.");
					break;
				}

				terrain.m_MaterialSlot.pMaterial = std::move(instance);
				terrain.m_MaterialSlot.IsInstance = true;
				if (const char* source = instance_doc->Attribute("Source"))
					terrain.m_MaterialSlot.SourceAssetPath = ABSOLUTE_PATH(source);
				return;
			}

			const char* path = element->Attribute("Material");
			if (path != nullptr && *path != '\0')
			{
				auto material = AssetManager::Instance().GetOrLoad<Material>(ABSOLUTE_PATH(path));
				if (material != nullptr)
				{
					terrain.m_MaterialSlot.pMaterial = std::move(material);
					terrain.m_MaterialSlot.SourceAssetPath = ABSOLUTE_PATH(path);
					return;
				}

				CORE_LOG_WARN("Failed to load terrain material '{}', falling back to the builtin one.", path);
			}

			terrain.m_MaterialSlot.pMaterial = TerrainComponent::GetDefaultMaterial();
		}
	}

	void RegisterTerrainComponent()
	{
		MakeRegistrar<TerrainComponent>("Terrain")
			.Field(&TerrainComponent::m_Extent, "Extent", FieldType::Vec2, 1.0f)
			.Field(&TerrainComponent::m_Resolution, "Resolution", FieldType::Int, 1.0f)
			.Range(64.0f, 2048.0f)
			.Field(&TerrainComponent::m_HeightScale, "HeightScale", FieldType::Float, 0.5f)
			.Range(0.0f, 400.0f)
			.Slider()
			.Field(&TerrainComponent::m_Seed, "Seed", FieldType::Int, 1.0f)
			.Field(&TerrainComponent::m_LodRatio, "LodRatio", FieldType::Float, 0.1f)
			.Range(1.0f, 32.0f)
			.Slider()
			.Field(&TerrainComponent::m_MinNodeSize, "MinNodeSize", FieldType::Int, 1.0f)
			.Range(8.0f, 512.0f)
			.Slider()
			.Field(&TerrainComponent::m_ReceiveShadow, "ReceiveShadow", FieldType::Bool)
			.Field(&TerrainComponent::m_CastShadow, "CastShadow", FieldType::Bool)
			.SaveExtra(&SaveTerrainMaterial)
			.LoadExtra(&LoadTerrainMaterial)
			.Register();
	}
}
