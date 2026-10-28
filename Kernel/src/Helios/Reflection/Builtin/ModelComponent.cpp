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
		/* 模型是资源引用：字段表只认 POD 与字符串，路径读写单独处理；
		 * 对象为空时不写属性，读的时候也接受属性缺失。 */
		void SaveModelPath(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& model_component = *static_cast<const ModelComponent*>(component);
			const auto& model = model_component.m_Model;
			if (model != nullptr)
			{
				/* 内建模型（BuiltinCube 等）没有资产文件：存身份名，加载经
				 * Model::Create 反查回 BuiltinModelType；文件模型存相对路径。 */
				BuiltinModelType builtin_type{};
				const bool is_builtin = model->TryGetBuiltinType(builtin_type);
				element->SetAttribute("ModelPath", is_builtin
					? model->GetPath().c_str()
					: RELATIVE_PATH(model->GetPath()).c_str());
			}

			/* 投影 / 接受阴影：与光源 IsCastShadow 同一口径，默认值也写出 */
			element->SetAttribute("CastShadow", model_component.m_CastShadow);
			element->SetAttribute("ReceiveShadow", model_component.m_ReceiveShadow);
			/* 骨骼蒙皮开关：模型没有蒙皮数据时写了也不起作用 */
			element->SetAttribute("EnableSkinning", model_component.m_EnableSkinning);

			/* 槽的实体级绑定：引用形态只写路径；实例形态全量内嵌（不依赖来源资产存活） */
			for (const auto& entry : model_component.m_SlotOverrides)
			{
				const auto& slot_override = entry.second;
				if (slot_override.pMaterial == nullptr)
					continue;

				auto* override_doc = element->InsertNewChildElement("MaterialOverride");
				override_doc->SetAttribute("Slot", entry.first.c_str());

				if (slot_override.IsInstance)
				{
					override_doc->SetAttribute("Instance", true);
					if (!slot_override.SourceAssetPath.empty())
						override_doc->SetAttribute("Source", RELATIVE_PATH(slot_override.SourceAssetPath).c_str());
					MaterialIO::WriteBody(override_doc, *slot_override.pMaterial);
				}
				else
				{
					/* "覆盖"挂的是材质资产；运行时构造的匿名材质没有路径可存，跳过 */
					const std::string& material_path = !slot_override.SourceAssetPath.empty()
						? slot_override.SourceAssetPath
						: slot_override.pMaterial->GetPath();
					if (material_path.empty())
						continue;
					override_doc->SetAttribute("Asset", RELATIVE_PATH(material_path).c_str());
				}
			}
		}

		void LoadModelPath(const tinyxml2::XMLElement* element, void* component)
		{
			auto& model_component = *static_cast<ModelComponent*>(component);

			const char* path = element->Attribute("ModelPath");
			if (path != nullptr && *path != '\0')
				model_component.m_Model = Model::Create(path);

			/* 旧场景没有这两个属性：缺省即可见设定（投影 / 接受阴影都开） */
			model_component.m_CastShadow = element->BoolAttribute("CastShadow", true);
			model_component.m_ReceiveShadow = element->BoolAttribute("ReceiveShadow", true);
			model_component.m_EnableSkinning = element->BoolAttribute("EnableSkinning", true);

			/* 槽绑定：引用形态走 AssetManager；实例形态读内嵌定义 */
			model_component.m_SlotOverrides.clear();
			for (const tinyxml2::XMLElement* override_doc = element->FirstChildElement("MaterialOverride"); override_doc; override_doc = override_doc->NextSiblingElement("MaterialOverride"))
			{
				const char* slot_name = override_doc->Attribute("Slot");
				if (slot_name == nullptr || *slot_name == '\0')
					continue;

				MaterialSlotOverride slot_override;
				if (override_doc->BoolAttribute("Instance"))
				{
					auto instance = CreateSharedPtr<Material>();
					if (!MaterialIO::ReadBody(override_doc, *instance))
						continue;
					slot_override.pMaterial = std::move(instance);
					slot_override.IsInstance = true;
					if (const char* source = override_doc->Attribute("Source"))
						slot_override.SourceAssetPath = ABSOLUTE_PATH(source);
				}
				else
				{
					const char* asset = override_doc->Attribute("Asset");
					if (asset == nullptr || *asset == '\0')
						continue;

					auto material = AssetManager::Instance().GetOrLoad<Material>(ABSOLUTE_PATH(asset));
					if (material == nullptr)
					{
						CORE_LOG_WARN("Failed to load override material '{}' for slot '{}'.", asset, slot_name);
						continue;
					}
					slot_override.pMaterial = std::move(material);
					slot_override.SourceAssetPath = ABSOLUTE_PATH(asset);
				}

				model_component.m_SlotOverrides[slot_name] = std::move(slot_override);
			}
		}
	}

	void RegisterModelComponent()
	{
		MakeRegistrar<ModelComponent>("Model")
			.SaveExtra(&SaveModelPath)
			.LoadExtra(&LoadModelPath)
			.Register();
	}
}
