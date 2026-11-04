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
			const auto& grass = *static_cast<const GrassComponent*>(component);
			const auto& mask = grass.m_DensityMask;
			if (mask.size() == size_t(GrassComponent::kDensityMaskSize) * GrassComponent::kDensityMaskSize)
			{
				constexpr char digits[] = "0123456789abcdef";
				std::string encoded;
				for (size_t i = 0; i < mask.size();)
				{
					uint32_t run = 1;
					while (run < 65535u && i + run < mask.size() && mask[i + run] == mask[i])
						++run;
					encoded += digits[mask[i] >> 4u];
					encoded += digits[mask[i] & 15u];
					for (int shift = 12; shift >= 0; shift -= 4)
						encoded += digits[(run >> shift) & 15u];
					i += run;
				}
				auto* mask_node = element->InsertNewChildElement("DensityMask");
				mask_node->SetAttribute("Resolution", GrassComponent::kDensityMaskSize);
				mask_node->SetText(encoded.c_str());
			}
			const auto& slot = grass.m_MaterialSlot;
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
			grass.m_DensityMask.clear();
			grass.m_DensityMaskRevision = 0;
			if (const auto* mask_node = element->FirstChildElement("DensityMask");
				mask_node != nullptr && mask_node->UnsignedAttribute("Resolution") == GrassComponent::kDensityMaskSize)
			{
				const char* text = mask_node->GetText();
				if (text != nullptr)
				{
					const std::string_view encoded(text);
					std::vector<uint8_t> decoded;
					decoded.reserve(size_t(GrassComponent::kDensityMaskSize) * GrassComponent::kDensityMaskSize);
					const auto hex = [](char c) -> int
					{
						if (c >= '0' && c <= '9') return c - '0';
						if (c >= 'a' && c <= 'f') return c - 'a' + 10;
						return -1;
					};
					bool valid = !encoded.empty() && encoded.size() % 6u == 0u
						&& encoded.size() <= size_t(GrassComponent::kDensityMaskSize)
							* GrassComponent::kDensityMaskSize * 6u;
					for (size_t i = 0; valid && i < encoded.size(); i += 6u)
					{
						int parts[6];
						for (int digit = 0; digit < 6; ++digit)
						{
							parts[digit] = hex(encoded[i + digit]);
							valid &= parts[digit] >= 0;
						}
						if (!valid) break;
						const size_t count = size_t(parts[2] << 12 | parts[3] << 8 | parts[4] << 4 | parts[5]);
						valid = count > 0 && decoded.size() + count
							<= size_t(GrassComponent::kDensityMaskSize) * GrassComponent::kDensityMaskSize;
						if (valid)
							decoded.insert(decoded.end(), count, uint8_t(parts[0] << 4 | parts[1]));
					}
					if (valid && decoded.size() == size_t(GrassComponent::kDensityMaskSize) * GrassComponent::kDensityMaskSize)
						grass.m_DensityMask = std::move(decoded);
					else
						CORE_LOG_WARN("Invalid grass density mask; using full density.");
				}
			}

			/* 老场景里的 LodDistance / CullDistance：新属性没写就按旧名字读一次 */
			if (element->Attribute("StartCullDistance") == nullptr
				&& element->Attribute("LodDistance") != nullptr)
				grass.m_StartCullDistance = element->FloatAttribute("LodDistance");
			if (element->Attribute("EndCullDistance") == nullptr
				&& element->Attribute("CullDistance") != nullptr)
				grass.m_EndCullDistance = element->FloatAttribute("CullDistance");

			grass.m_MaterialSlot = InstanceMaterialSlot{};

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
			/* 声明顺序 = 场景文件的属性顺序（保持既有场景原样写出），
			 * 面板按 Group 归拢，所以同一组可以在后面隔几行接着声明。 */
			.Group("Patch")
			.Field(&GrassComponent::m_Extent, "Extent", FieldType::Vec2, 0.1f)
			.Field(&GrassComponent::m_Density, "Density", FieldType::Float, 0.5f)
			.Group("Blades")
			.Field(&GrassComponent::m_Height, "Height", FieldType::Float, 0.01f)
			.Field(&GrassComponent::m_Width, "Width", FieldType::Float, 0.001f)
			.Group("Distance")
			.Field(&GrassComponent::m_StartCullDistance, "StartCullDistance", FieldType::Float, 0.5f)
			.Range(0.0f, 400.0f)
			.Field(&GrassComponent::m_EndCullDistance, "EndCullDistance", FieldType::Float, 0.5f)
			.Range(1.0f, 400.0f)
			.Field(&GrassComponent::m_MinDensityScale, "MinDensityScale", FieldType::Float, 0.01f)
			.Range(0.0f, 1.0f)
			.Group("Wind")
			.Field(&GrassComponent::m_WindDirection, "WindDirection", FieldType::Vec2, 0.01f)
			.Field(&GrassComponent::m_WindStrength, "WindStrength", FieldType::Float, 0.005f)
			.Field(&GrassComponent::m_WindSpeed, "WindSpeed", FieldType::Float, 0.01f)
			.Field(&GrassComponent::m_WindSpatialScale, "WindSpatialScale", FieldType::Float, 0.005f)
			.Field(&GrassComponent::m_WindCullDistance, "WindCullDistance", FieldType::Float, 0.5f)
			.Range(0.0f, 400.0f)
			.Group("Shadows")
			.Field(&GrassComponent::m_ReceiveShadow, "ReceiveShadow", FieldType::Bool)
			.Field(&GrassComponent::m_CastShadow, "CastShadow", FieldType::Bool)
			.Field(&GrassComponent::m_ShadowFadeDistance, "ShadowFadeDistance", FieldType::Float, 0.5f)
			.Range(0.0f, 400.0f)
			/* 踩踏恢复归回叶片组，种子 / 株数预算 / 底面归回草地范围组 */
			.Group("Blades")
			.Field(&GrassComponent::m_BendRecover, "BendRecover", FieldType::Float, 0.05f)
			.Group("Patch")
			.Field(&GrassComponent::m_Seed, "Seed", FieldType::Int)
			.Group("Terrain")
			.Field(&GrassComponent::m_ConformToTerrain, "ConformToTerrain", FieldType::Bool)
			.Field(&GrassComponent::m_SurfaceAlign, "SurfaceAlign", FieldType::Float, 0.01f)
			.Range(0.0f, 1.0f)
			.Slider()
			.Field(&GrassComponent::m_MaxSlope, "MaxSlope", FieldType::Float, 0.5f)
			.Range(0.0f, 90.0f)
			.Field(&GrassComponent::m_FollowTerrain, "FollowTerrain", FieldType::Bool)
			.Group("Patch")
			.Field(&GrassComponent::m_MaxInstances, "MaxInstances", FieldType::Int, 1000.0f)
			.Range(1000.0f, 4000000.0f)
			.Field(&GrassComponent::m_DrawFlatUnderlay, "DrawFlatUnderlay", FieldType::Bool)
			.Group("Terrain")
			.Field(&GrassComponent::m_UseTerrainGrowth, "UseTerrainGrowth", FieldType::Bool)
			.Group("Blades")
			.Field(&GrassComponent::m_ClumpSize, "ClumpSize", FieldType::Float, 0.1f)
			.Range(0.25f, 8.0f)
			.Field(&GrassComponent::m_ClumpStrength, "ClumpStrength", FieldType::Float, 0.01f)
			.Range(0.0f, 1.0f)
			.Slider()
			.Group("Patch")
			.Field(&GrassComponent::m_StablePlacement, "StablePlacement", FieldType::Bool)
			.SaveExtra(&SaveGrassMaterial)
			.LoadExtra(&LoadGrassMaterial)
			.Register();
	}
}
