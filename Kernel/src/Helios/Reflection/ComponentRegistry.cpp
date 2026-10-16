#include "Pch.h"
#include "ComponentRegistry.h"
#include "Builtin/BuiltinComponents.h"

namespace Helios
{
	ComponentRegistry& ComponentRegistry::Instance()
	{
		/* 函数内静态：首次使用时构造，规避跨 TU 的静态初始化顺序问题
		 * （内部注册对象会在静态初始化期调用本函数）。 */
		static ComponentRegistry s_Instance;
		return s_Instance;
	}

	const ComponentDesc* ComponentRegistry::Find(std::type_index type) const
	{
		for (const auto& component : m_Components)
		{
			if (component.Type == type)
				return &component;
		}
		return nullptr;
	}

	ComponentRegistrar::ComponentRegistrar(const char* name, std::type_index type)
	{
		m_Desc.Name = name;
		m_Desc.Type = type;
	}

	ComponentRegistrar& ComponentRegistrar::Field(FieldAccessor accessor, const char* name, FieldType type,
	                                              float drag_speed, float reset_value, const char* semantics)
	{
		FieldDesc desc;
		desc.Name = name;
		desc.Type = type;
		desc.ValueSize = accessor.ValueSize;
		desc.DragSpeed = drag_speed;
		desc.ResetValue = reset_value;
		desc.Semantics = semantics;
		desc.Get = accessor.Get;
		desc.Set = accessor.Set;
		m_Desc.Fields.emplace_back(desc);
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::EnumNames(EnumNamesFunc names)
	{
		if (!m_Desc.Fields.empty())
			m_Desc.Fields.back().GetEnumNames = names;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::Accessors(HasFunc has, GetPtrFunc get, RemoveFunc remove, AddFunc add)
	{
		m_Desc.Has = has;
		m_Desc.GetPtr = get;
		m_Desc.Remove = remove;
		m_Desc.Add = add;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::CustomDraw(ComponentDrawFunc draw)
	{
		m_Desc.CustomDraw = draw;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::Visible(ComponentVisibleFunc visible)
	{
		m_Desc.Visible = visible;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::Snapshot(ComponentCaptureFunc capture, ComponentRestoreFunc restore)
	{
		m_Desc.Capture = capture;
		m_Desc.Restore = restore;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::NotAddable()
	{
		m_Desc.bAddable = false;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::Variant(const char* menu_name, AddFunc add)
	{
		m_Desc.Variants.push_back({ menu_name, add });
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::SerializeName(const char* name)
	{
		if (!m_Desc.Fields.empty())
			m_Desc.Fields.back().SerializeName = name;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::SaveExtra(ComponentSaveFunc save)
	{
		m_Desc.SaveExtra = save;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::LoadExtra(ComponentLoadFunc load)
	{
		m_Desc.LoadExtra = load;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::NotSerialized()
	{
		m_Desc.bSerializable = false;
		return *this;
	}

	void ComponentRegistrar::Register()
	{
		/* 未显式声明形态时，以默认构造作为唯一形态（菜单用组件名） */
		if (m_Desc.bAddable && m_Desc.Variants.empty() && m_Desc.Add != nullptr)
			m_Desc.Variants.push_back({ nullptr, m_Desc.Add });

		ComponentRegistry::Instance().Add(std::move(m_Desc));
	}

	ComponentRegistrar& ComponentRegistrar::When(FieldCondition condition)
	{
		if (!m_Desc.Fields.empty())
			m_Desc.Fields.back().Condition = condition;
		return *this;
	}

	namespace
	{
		/* 按依赖字段的类型读取其数值（供比较类条件使用） */
		double ReadDependNumber(FieldType type, const void* field)
		{
			switch (type)
			{
			case FieldType::Bool:  return *static_cast<const bool*>(field) ? 1.0 : 0.0;
			case FieldType::Int:   return static_cast<double>(*static_cast<const int*>(field));
			case FieldType::Float: return static_cast<double>(*static_cast<const float*>(field));
			default:               return 0.0;
			}
		}
	}

	bool EvaluateCondition(const FieldCondition& condition, const void* component)
	{
		if (component == nullptr)
			return true;

		if (condition.Predicate != nullptr)
			return condition.Predicate(component);

		/* 无依赖字段：不读组件内存 */
		if (condition.Op == ConditionOp::Always)
			return true;

		const void* field = static_cast<const uint8_t*>(component) + condition.DependOffset;

		switch (condition.Op)
		{
		case ConditionOp::IsTrue:   return *static_cast<const bool*>(field);
		case ConditionOp::IsFalse:  return !*static_cast<const bool*>(field);
		case ConditionOp::Equal:    return ReadDependNumber(condition.DependType, field) == condition.Expected;
		case ConditionOp::NotEqual: return ReadDependNumber(condition.DependType, field) != condition.Expected;
		case ConditionOp::Greater:  return ReadDependNumber(condition.DependType, field) > condition.Expected;
		case ConditionOp::Less:     return ReadDependNumber(condition.DependType, field) < condition.Expected;
		case ConditionOp::Always:   break;
		}
		return true;
	}

	const void* ReadFieldValue(const FieldDesc& field, const void* component, void* buffer)
	{
		if (component == nullptr)
			return nullptr;

		if (field.Get != nullptr)
		{
			field.Get(component, buffer);
			return buffer;
		}

		return static_cast<const uint8_t*>(component) + field.Offset;
	}

	namespace
	{
		/* 启动时注册全部内置组件（清单见 Builtin/BuiltinComponents.cpp）。
		 * 触发对象必须留在本文件：框架 TU 被强引用、不会被静态库丢弃，
		 * 清单与各组件注册文件靠这条调用链一起链进来。 */
		struct BuiltinComponentRegistration
		{
			BuiltinComponentRegistration()
			{
				RegisterBuiltinComponents();
			}
		};

		const BuiltinComponentRegistration s_BuiltinComponentRegistration;
	}
}
