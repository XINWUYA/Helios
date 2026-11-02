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
		/* 材质是资源引用：字段表只认 POD，路径与实例内嵌单独读写。
		 * 两种形态与模型槽一致 —— 引用只写路径，实例全量内嵌（不依赖来源资产存活） */
		void SaveGrassMaterial(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& slot = static_cast<const GrassComponent*>(component)->m_MaterialSlot;
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

		void LoadGrassMaterial(const tinyxml2::XMLElement* element, void* component)
		{
			auto& grass = *static_cast<GrassComponent*>(component);

			/* 老场景里的 LodDistance / CullDistance：新属性没写就按旧名字读一次 */
			if (element->Attribute("StartCullDistance") == nullptr
				&& element->Attribute("LodDistance") != nullptr)
				grass.m_StartCullDistance = element->FloatAttribute("LodDistance");
			if (element->Attribute("EndCullDistance") == nullptr
				&& element->Attribute("CullDistance") != nullptr)
				grass.m_EndCullDistance = element->FloatAttribute("CullDistance");

			grass.m_MaterialSlot = GrassMaterialSlot{};

			/* 实例形态：参数内嵌在场景里，来源只用于 Reset */
			for (const tinyxml2::XMLElement* instance_doc = element->FirstChildElement("MaterialOverride");
				instance_doc != nullptr; instance_doc = instance_doc->NextSiblingElement("MaterialOverride"))
			{
				if (!instance_doc->BoolAttribute("Instance"))
					continue;

				auto instance = CreateSharedPtr<Material>();
				if (!MaterialIO::ReadBody(instance_doc, *instance))
				{
					CORE_LOG_WARN("Failed to read inline grass material, falling back to the asset reference.");
					break;
				}

				grass.m_MaterialSlot.pMaterial = std::move(instance);
				grass.m_MaterialSlot.IsInstance = true;
				if (const char* source = instance_doc->Attribute("Source"))
					grass.m_MaterialSlot.SourceAssetPath = ABSOLUTE_PATH(source);
				return;
			}

			const char* path = element->Attribute("Material");
			if (path != nullptr && *path != '\0')
			{
				auto material = AssetManager::Instance().GetOrLoad<Material>(ABSOLUTE_PATH(path));
				if (material != nullptr)
				{
					grass.m_MaterialSlot.pMaterial = std::move(material);
					grass.m_MaterialSlot.SourceAssetPath = ABSOLUTE_PATH(path);
					return;
				}

				CORE_LOG_WARN("Failed to load grass material '{}', falling back to the builtin one.", path);
			}

			/* 没写材质也照画：用着色器默认参数兜住，别让组件静默变成空壳 */
			grass.m_MaterialSlot.pMaterial = GrassComponent::GetDefaultMaterial();
		}
	}

	void RegisterGrassBendComponent()
	{
		MakeRegistrar<GrassBendComponent>("GrassBend")
			.Field(&GrassBendComponent::m_Radius, "Radius", FieldType::Float, 0.02f)
			.Field(&GrassBendComponent::m_Enabled, "Enabled", FieldType::Bool)
			.Register();
	}

	void RegisterGrassComponent()
	{
		MakeRegistrar<GrassComponent>("Grass")
			.Field(&GrassComponent::m_Extent, "Extent", FieldType::Vec2, 0.1f)
			.Field(&GrassComponent::m_Density, "Density", FieldType::Float, 0.5f)
			.Field(&GrassComponent::m_Height, "Height", FieldType::Float, 0.01f)
			.Field(&GrassComponent::m_Width, "Width", FieldType::Float, 0.001f)
			.Field(&GrassComponent::m_StartCullDistance, "StartCullDistance", FieldType::Float, 0.5f)
			.Range(0.0f, 400.0f)
			.Field(&GrassComponent::m_EndCullDistance, "EndCullDistance", FieldType::Float, 0.5f)
			.Range(1.0f, 400.0f)
			.Field(&GrassComponent::m_MinDensityScale, "MinDensityScale", FieldType::Float, 0.01f)
			.Range(0.0f, 1.0f)
			.Field(&GrassComponent::m_WindDirection, "WindDirection", FieldType::Vec2, 0.01f)
			.Field(&GrassComponent::m_WindStrength, "WindStrength", FieldType::Float, 0.005f)
			.Field(&GrassComponent::m_WindSpeed, "WindSpeed", FieldType::Float, 0.01f)
			.Field(&GrassComponent::m_WindSpatialScale, "WindSpatialScale", FieldType::Float, 0.005f)
			.Field(&GrassComponent::m_WindCullDistance, "WindCullDistance", FieldType::Float, 0.5f)
			.Range(0.0f, 400.0f)
			.Field(&GrassComponent::m_ReceiveShadow, "ReceiveShadow", FieldType::Bool)
			.Field(&GrassComponent::m_CastShadow, "CastShadow", FieldType::Bool)
			.Field(&GrassComponent::m_BendRecover, "BendRecover", FieldType::Float, 0.05f)
			.Field(&GrassComponent::m_Seed, "Seed", FieldType::Int)
			.Field(&GrassComponent::m_ConformToTerrain, "ConformToTerrain", FieldType::Bool)
			.Field(&GrassComponent::m_SurfaceAlign, "SurfaceAlign", FieldType::Float, 0.01f)
			.Range(0.0f, 1.0f)
			.Slider()
			.Field(&GrassComponent::m_MaxSlope, "MaxSlope", FieldType::Float, 0.5f)
			.Range(0.0f, 90.0f)
			.Field(&GrassComponent::m_FollowTerrain, "FollowTerrain", FieldType::Bool)
			.Field(&GrassComponent::m_MaxInstances, "MaxInstances", FieldType::Int, 1000.0f)
			.Range(1000.0f, 4000000.0f)
			.SaveExtra(&SaveGrassMaterial)
			.LoadExtra(&LoadGrassMaterial)
			.Register();
	}
}
