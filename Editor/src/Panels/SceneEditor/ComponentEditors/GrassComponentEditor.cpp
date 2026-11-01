#include "Pch.h"
#include "ComponentEditors.h"
#include "UI/MaterialSlotRow.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	namespace
	{
		/* 草地只有一个材质槽：行版式与模型槽共用，这里只管语义 ——
		 * 清掉就没有资产来源，回内建默认外观；Reset 从来源 .mtl 重新克隆。
		 * 返回是否改动：换材质要生成快照命令（撤销与场景脏标记都靠它） */
		bool DrawGrassMaterialBlock(void* raw)
		{
			auto& component = *static_cast<GrassComponent*>(raw);
			MaterialSlotOverride& slot = component.m_MaterialSlot;

			const std::string material_path = !slot.SourceAssetPath.empty()
				? slot.SourceAssetPath
				: (slot.pMaterial != nullptr ? slot.pMaterial->GetPath() : std::string());

			MaterialSlotRow::State state;
			state.Label = "Material";
			state.Path = material_path;
			state.EmptyText = "内置默认";
			state.HoverPath = material_path.empty() ? std::string() : RELATIVE_PATH(material_path);
			state.IsInstance = slot.IsInstance;
			/* 已经落在内建默认上就没有可清的东西 */
			state.CanClear = !material_path.empty();
			state.EmptyHint = "把 .mtl 拖到这里";
			state.ClearHint = "清掉来源，回到内建默认外观";
			state.ResetHint = "从来源 .mtl 重新克隆";

			const MaterialSlotRow::Result row = MaterialSlotRow::Draw(state);
			switch (row.Act)
			{
			case MaterialSlotRow::Action::Assign:
				slot.pMaterial = row.Picked;
				slot.SourceAssetPath = row.SourcePath;
				slot.IsInstance = false;
				return true;
			case MaterialSlotRow::Action::Clear:
				slot.pMaterial = GrassComponent::GetDefaultMaterial();
				slot.SourceAssetPath.clear();
				slot.IsInstance = false;
				return true;
			case MaterialSlotRow::Action::Reset:
			{
				auto source = AssetManager::Instance().GetOrLoad<Material>(slot.SourceAssetPath);
				if (source == nullptr)
					return false;

				slot.pMaterial = source->Clone();
				slot.IsInstance = true;
				return true;
			}
			default:
				return false;
			}
		}
	}

	void RegisterGrassComponentEditor()
	{
		ComponentRegistry::Instance().SetCustomDraw(typeid(GrassComponent), &DrawGrassMaterialBlock);
	}
}
