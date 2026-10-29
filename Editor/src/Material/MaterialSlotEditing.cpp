#include "Pch.h"
#include "MaterialSlotEditing.h"

namespace Helios::MaterialSlotEditing
{
	namespace
	{
		const MaterialSlotOverride* FindOverride(
			const std::unordered_map<std::string, MaterialSlotOverride>& overrides, const std::string& slot_name)
		{
			const auto iter = overrides.find(slot_name);
			return (iter != overrides.end()) ? &iter->second : nullptr;
		}
	}

	SharedPtr<Material> Resolve(const Model& model,
		const std::unordered_map<std::string, MaterialSlotOverride>& overrides,
		const std::string& slot_name)
	{
		if (const MaterialSlotOverride* slot_override = FindOverride(overrides, slot_name);
			slot_override != nullptr && slot_override->pMaterial != nullptr)
		{
			return slot_override->pMaterial;
		}

		if (const MaterialSlot* slot = model.GetSlotByName(slot_name);
			slot != nullptr && slot->pDefault != nullptr)
		{
			return slot->pDefault;
		}

		return Material::BuiltinWhite();
	}

	std::string SourcePath(const Model& model,
		const std::unordered_map<std::string, MaterialSlotOverride>& overrides,
		const std::string& slot_name)
	{
		if (const MaterialSlotOverride* slot_override = FindOverride(overrides, slot_name);
			slot_override != nullptr)
		{
			if (!slot_override->SourceAssetPath.empty())
				return slot_override->SourceAssetPath;
			if (slot_override->pMaterial != nullptr && !slot_override->pMaterial->GetPath().empty())
				return slot_override->pMaterial->GetPath();
		}

		const MaterialSlot* slot = model.GetSlotByName(slot_name);
		return (slot != nullptr) ? slot->SourcePath : std::string();
	}

	SharedPtr<Material> MakeInstance(const Model& model,
		std::unordered_map<std::string, MaterialSlotOverride>& overrides,
		const std::string& slot_name, bool& changed)
	{
		if (const MaterialSlotOverride* slot_override = FindOverride(overrides, slot_name);
			slot_override != nullptr && slot_override->IsInstance && slot_override->pMaterial != nullptr)
		{
			return slot_override->pMaterial;
		}

		/* 从"当前生效材质"克隆；来源文件一起带过去，实例保存后还能指回原来的 .mtl */
		SharedPtr<Material> source;
		std::string source_path;
		if (const MaterialSlotOverride* slot_override = FindOverride(overrides, slot_name);
			slot_override != nullptr && slot_override->pMaterial != nullptr)
		{
			source = slot_override->pMaterial;
			source_path = !slot_override->SourceAssetPath.empty()
				? slot_override->SourceAssetPath
				: source->GetPath();
		}
		else
		{
			const MaterialSlot* slot = model.GetSlotByName(slot_name);
			source = (slot != nullptr && slot->pDefault != nullptr) ? slot->pDefault : Material::BuiltinWhite();
			source_path = (slot != nullptr && !slot->SourcePath.empty()) ? slot->SourcePath : source->GetPath();
		}

		MaterialSlotOverride slot_override;
		slot_override.pMaterial = source->Clone();
		slot_override.IsInstance = true;
		slot_override.SourceAssetPath = std::move(source_path);
		overrides[slot_name] = std::move(slot_override);
		changed = true;

		return overrides[slot_name].pMaterial;
	}

	void WriteParam(const Model& model,
		std::unordered_map<std::string, MaterialSlotOverride>& overrides,
		const std::string& slot_name, ParamType type, const std::string& name,
		const std::any& value, bool& changed)
	{
		const SharedPtr<Material> instance = MakeInstance(model, overrides, slot_name, changed);
		if (instance == nullptr)
			return;

		if (type == ParamType::Texture)
			instance->SetTexture(name, std::any_cast<SharedPtr<DeviceTexture>>(value));
		else
			instance->SetParameters(type, name, value);

		/* 已经是实例时 MakeInstance 不再置位：值变了同样要记一笔（撤销快照） */
		changed = true;
	}

	void WriteRasterState(const Model& model,
		std::unordered_map<std::string, MaterialSlotOverride>& overrides,
		const std::string& slot_name, const RenderRasterState& state, bool& changed)
	{
		const SharedPtr<Material> instance = MakeInstance(model, overrides, slot_name, changed);
		if (instance == nullptr)
			return;

		instance->SetRasterState(state);
		changed = true;
	}

	void WriteRenderQueue(const Model& model,
		std::unordered_map<std::string, MaterialSlotOverride>& overrides,
		const std::string& slot_name, int32_t render_queue, bool& changed)
	{
		const SharedPtr<Material> instance = MakeInstance(model, overrides, slot_name, changed);
		if (instance == nullptr)
			return;

		instance->SetRenderQueue(render_queue);
		changed = true;
	}
}
