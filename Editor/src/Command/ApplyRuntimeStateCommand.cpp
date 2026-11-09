#include "Pch.h"
#include "ApplyRuntimeStateCommand.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Scene.h"
#include <cstring>

namespace Helios
{
	namespace
	{
		/* 文本字段可能指向堆上内存，按字节搬会写坏；数值 / 布尔 / 枚举 / 向量 / 掩码都是 POD */
		bool IsCopyableField(const FieldDesc& field)
		{
			if (field.Type == FieldType::String || field.Type == FieldType::AssetRef
				|| field.Type == FieldType::EntityRef)
				return false;

			return field.ValueSize > 0 && field.ValueSize <= kFieldValueCapacity;
		}

		void WriteField(const FieldDesc& field, void* component, const uint8_t* data)
		{
			/* 访问器字段走组件的 Set（相机 / 光源的参数在各自的运行对象里） */
			if (field.Set != nullptr)
			{
				field.Set(component, data);
				return;
			}

			std::memcpy(static_cast<uint8_t*>(component) + field.Offset, data, field.ValueSize);
		}

		std::string EntityName(Entity& entity)
		{
			if (!entity.HasComponent<NameComponent>())
				return {};
			return entity.GetComponent<NameComponent>().m_Name;
		}

		/* 地形高度不在组件里：搬参数会让编辑世界按新参数重建高度图，丢掉未保存的雕刻 */
		bool IsSkippedComponent(const ComponentDesc& desc)
		{
			return desc.Type == std::type_index(typeid(TerrainComponent));
		}
	}

	UniquePtr<ApplyRuntimeStateCommand> ApplyRuntimeStateCommand::Create(Entity runtime_entity, Entity edit_entity,
		std::string& out_reason)
	{
		if (!runtime_entity || !edit_entity)
		{
			out_reason = "Select an entity in the running world first.";
			return nullptr;
		}

		if (EntityName(runtime_entity) != EntityName(edit_entity))
		{
			out_reason = "The editing scene has a different entity at this handle.";
			return nullptr;
		}

		auto command = CreateUniquePtr<ApplyRuntimeStateCommand>();
		command->m_EditEntity = edit_entity;

		alignas(16) uint8_t buffer[kFieldValueCapacity]{};
		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (desc.Has == nullptr || desc.GetPtr == nullptr || IsSkippedComponent(desc))
				continue;
			if (!desc.Has(runtime_entity) || !desc.Has(edit_entity))
				continue;

			void* runtime_component = desc.GetPtr(runtime_entity);
			void* edit_component = desc.GetPtr(edit_entity);
			if (runtime_component == nullptr || edit_component == nullptr)
				continue;

			ComponentValues values;
			values.Desc = &desc;

			for (const FieldDesc& field : desc.Fields)
			{
				if (!IsCopyableField(field))
					continue;

				FieldValue value;
				value.Field = &field;
				value.Before.resize(field.ValueSize);
				value.After.resize(field.ValueSize);

				const void* runtime_value = ReadFieldValue(field, runtime_component, buffer);
				if (runtime_value != nullptr)
					std::memcpy(value.After.data(), runtime_value, field.ValueSize);

				const void* edit_value = ReadFieldValue(field, edit_component, buffer);
				if (edit_value != nullptr)
					std::memcpy(value.Before.data(), edit_value, field.ValueSize);

				values.Fields.emplace_back(std::move(value));
			}

			if (values.Fields.empty())
				continue;

			command->m_FieldCount += values.Fields.size();
			command->m_Components.emplace_back(std::move(values));
		}

		if (command->m_FieldCount == 0)
		{
			out_reason = "Nothing to apply on this entity.";
			return nullptr;
		}

		return command;
	}

	void ApplyRuntimeStateCommand::Apply(bool after)
	{
		for (const ComponentValues& values : m_Components)
		{
			void* component = values.Desc->GetPtr(m_EditEntity);
			if (component == nullptr)
				continue;

			for (const FieldValue& value : values.Fields)
				WriteField(*value.Field, component, after ? value.After.data() : value.Before.data());
		}
	}

	void ApplyRuntimeStateCommand::Do()
	{
		Apply(true);
	}

	void ApplyRuntimeStateCommand::Undo()
	{
		Apply(false);
	}
}
