#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>
#include "Helios/Common/Common.h"
#include "Helios/Common/Utils.h"

namespace tinyxml2
{
	class XMLElement;
}

namespace Helios
{
	class Entity;

	/* 组件字段元数据注册表：一个组件只注册一次，Inspector 和 Add 菜单都由这张表推导，新增组件
	 * 不用改 UI 代码。实现就是 vector + 线性查找（组件数量就个位数）。 */

	/* 字段类型：决定 Inspector 用什么控件渲染 */
	enum class FieldType : uint8_t
	{
		Bool,
		Int,
		Float,
		Vec2,
		Vec3,
		Vec4,
		Color,
		String,
		Enum,
		AssetRef,
	};

	/* 字段可见性条件：不满足时该字段不出现在 Inspector（值保留，序列化照常写出）。
	 * Always 是默认值 —— 未声明条件的字段必须始终显示，不能落进任何依赖字段的比较。 */
	enum class ConditionOp : uint8_t
	{
		Always,     /* 无条件显示（默认） */
		IsTrue,     /* 依赖字段为 true（bool） */
		IsFalse,    /* 依赖字段为 false（bool） */
		Equal,      /* 依赖字段 == 期望值 */
		NotEqual,
		Greater,
		Less,
	};

	struct FieldCondition
	{
		size_t      DependOffset{ 0 };               /* 依赖字段相对组件起始的偏移 */
		FieldType   DependType{ FieldType::Bool };   /* 依赖字段类型：决定按什么类型读取 */
		ConditionOp Op{ ConditionOp::Always };
		double      Expected{ 0.0 };                 /* Equal / NotEqual / Greater / Less 的期望值 */
		/* 逃生舱：偏移表达不了的复杂条件（如依赖嵌套对象的访问器结果）。
		 * 谓词无法导出为 schema JSON，导出时应标记为自定义条件。 */
		bool (*Predicate)(const void* component){ nullptr };
	};

	/* 依赖字段类型 -> FieldType（决定按什么类型读取依赖值） */
	template <typename M> struct FieldTypeOf { static constexpr FieldType Value = FieldType::Float; };
	template <> struct FieldTypeOf<bool> { static constexpr FieldType Value = FieldType::Bool; };
	template <> struct FieldTypeOf<int> { static constexpr FieldType Value = FieldType::Int; };
	template <> struct FieldTypeOf<float> { static constexpr FieldType Value = FieldType::Float; };
	template <> struct FieldTypeOf<double> { static constexpr FieldType Value = FieldType::Float; };

	/* ---- 访问器字段 ----
	 * 值在组件持有的嵌套对象里（比如 camera->GetFov()），字节偏移表达不了，改用一对类型擦除的
	 * 回调读写；两者都不为空时 FieldDesc::Offset 就不再用了。 */

	using FieldGetFunc = void (*)(const void* component, void* out_value);
	using FieldSetFunc = void (*)(void* component, const void* in_value);

	/* ---- 组件数据快照 ----
	 * 类型擦除地按值持有组件的一份拷贝，供「移除组件 / 删除实体」被撤销时还原内容。
	 * 只承载数据，不参与 Inspector 与序列化。 */

	class ComponentSnapshot
	{
	public:
		ComponentSnapshot() = default;

		template <typename T>
		explicit ComponentSnapshot(const T& component)
			: m_Value(std::make_shared<T>(component)), m_Type(typeid(T))
		{
		}

		[[nodiscard]] bool IsValid() const { return m_Value != nullptr; }
		[[nodiscard]] const std::type_index& GetType() const { return m_Type; }

		/* 快照为空或类型不符时返回 nullptr */
		template <typename T>
		[[nodiscard]] const T* As() const
		{
			if (m_Value == nullptr || m_Type != std::type_index(typeid(T)))
				return nullptr;
			return static_cast<const T*>(m_Value.get());
		}

	private:
		std::shared_ptr<void> m_Value;
		std::type_index       m_Type{ typeid(void) };
	};

	/* Capture 把组件拷进快照；Restore 把快照写回实体（实体尚无该组件时先添加） */
	using ComponentCaptureFunc = void (*)(Entity& entity, ComponentSnapshot& out);
	using ComponentRestoreFunc = void (*)(Entity& entity, const ComponentSnapshot& in);

	/* 编辑缓冲容量：需容纳 Bool/Int/Float/Vec2/Vec3/Vec4/Color 及按 int 编辑的枚举 */
	inline constexpr size_t kFieldValueCapacity = 64;

	struct FieldAccessor
	{
		FieldGetFunc Get{ nullptr };
		FieldSetFunc Set{ nullptr };
		size_t       ValueSize{ 0 };   /* 值的字节数 */
	};

	namespace Detail
	{
		/* 指针成员的宿主类型 */
		template <typename M> struct MemberOwner;
		template <typename C, typename M> struct MemberOwner<M C::*> { using Type = C; };

		/* 指针成员指向的共享对象的类型 */
		template <typename M> struct MemberSharedElement;
		template <typename C, typename E> struct MemberSharedElement<SharedPtr<E> C::*>
		{ using Type = E; };

		/* 常成员函数的返回类型 */
		template <typename F> struct GetterReturn;
		template <typename O, typename R> struct GetterReturn<R (O::*)() const>
		{ using Type = R; };

		/* 成员指针的值类型 */
		template <typename M> struct MemberValue;
		template <typename C, typename M> struct MemberValue<M C::*> { using Type = M; };
	}

	/* 由「组件成员（指向嵌套对象）+ 该对象的 getter / setter」生成读写回调。
	 * 嵌套对象为空时：读操作保持缓冲不变，写操作不做任何事。 */
	template <auto Member, auto Getter, auto Setter>
	FieldAccessor MakeAccessor()
	{
		using Component = typename Detail::MemberOwner<decltype(Member)>::Type;
		using Object    = typename Detail::MemberSharedElement<decltype(Member)>::Type;
		/* getter 可能按值或按引用返回，统一折算为可存储的值类型 */
		using Value     = std::decay_t<typename Detail::GetterReturn<decltype(Getter)>::Type>;

		FieldAccessor accessor;
		accessor.ValueSize = sizeof(Value);
		accessor.Get = [](const void* component, void* out_value)
		{
			const Object* object = (static_cast<const Component*>(component)->*Member).get();
			if (object != nullptr)
				*static_cast<Value*>(out_value) = (object->*Getter)();
		};
		accessor.Set = [](void* component, const void* in_value)
		{
			Object* object = (static_cast<Component*>(component)->*Member).get();
			if (object != nullptr)
				(object->*Setter)(*static_cast<const Value*>(in_value));
		};
		return accessor;
	}

	/* 枚举的访问器版本：编辑器按 int 编辑，读写时与枚举类型互转 */
	template <auto Member, auto Getter, auto Setter>
	FieldAccessor MakeEnumAccessor()
	{
		using Component = typename Detail::MemberOwner<decltype(Member)>::Type;
		using Object    = typename Detail::MemberSharedElement<decltype(Member)>::Type;
		using Value     = std::decay_t<typename Detail::GetterReturn<decltype(Getter)>::Type>;

		FieldAccessor accessor;
		accessor.ValueSize = sizeof(int);
		accessor.Get = [](const void* component, void* out_value)
		{
			const Object* object = (static_cast<const Component*>(component)->*Member).get();
			if (object != nullptr)
				*static_cast<int*>(out_value) = static_cast<int>((object->*Getter)());
		};
		accessor.Set = [](void* component, const void* in_value)
		{
			Object* object = (static_cast<Component*>(component)->*Member).get();
			if (object != nullptr)
				(object->*Setter)(static_cast<Value>(*static_cast<const int*>(in_value)));
		};
		return accessor;
	}

	/* 配置结构体字段的访问器：适用于嵌套对象把一组参数以结构体整体读写的情形
	 * （如 ReflectionProbe::GetBakeConfig / SetBakeConfig）。
	 * 读：取出配置副本后读其中的成员；写：取出副本、改成员、整体写回。 */
	template <auto Member, auto Getter, auto Setter, auto ConfigMember>
	FieldAccessor MakeConfigAccessor()
	{
		using Component = typename Detail::MemberOwner<decltype(Member)>::Type;
		using Object    = typename Detail::MemberSharedElement<decltype(Member)>::Type;
		using Config    = std::decay_t<typename Detail::GetterReturn<decltype(Getter)>::Type>;
		using Value     = typename Detail::MemberValue<decltype(ConfigMember)>::Type;

		FieldAccessor accessor;
		accessor.ValueSize = sizeof(Value);
		accessor.Get = [](const void* component, void* out_value)
		{
			const Object* object = (static_cast<const Component*>(component)->*Member).get();
			if (object == nullptr)
				return;

			const Config config = (object->*Getter)();
			*static_cast<Value*>(out_value) = config.*ConfigMember;
		};
		accessor.Set = [](void* component, const void* in_value)
		{
			Object* object = (static_cast<Component*>(component)->*Member).get();
			if (object == nullptr)
				return;

			Config config = (object->*Getter)();
			config.*ConfigMember = *static_cast<const Value*>(in_value);
			(object->*Setter)(config);
		};
		return accessor;
	}

	/* 枚举字段的名称提供者 */
	using EnumNamesFunc = std::vector<std::string> (*)();

	struct FieldDesc
	{
		const char* Name{ nullptr };        /* 显示名 */
		FieldType   Type{ FieldType::Float };
		size_t      Offset{ 0 };             /* 相对组件起始地址的字节偏移（访问器字段不使用） */
		size_t      ValueSize{ 0 };          /* 值的字节数 */
		float       DragSpeed{ 0.1f };       /* 数值拖拽步长 */
		float       ResetValue{ 0.0f };      /* 控件的重置值（如 Scale 用 1.0） */
		/* 语义标记：表达「存储形式 ≠ 编辑形式」的字段。
		 * 目前使用 "AngleDeg"：内部以弧度存储、Inspector 以角度显示与编辑。 */
		const char* Semantics{ nullptr };

		/* 序列化属性名：为空时取 Name（如 NearClip 存为 Near） */
		const char* SerializeName{ nullptr };

		/* 可见性条件：默认无条件（字段始终显示） */
		FieldCondition Condition{};

		/* 访问器字段的读写回调：非空时 Offset 无效，编辑器经缓冲读写 */
		FieldGetFunc Get{ nullptr };
		FieldSetFunc Set{ nullptr };

		/* 枚举名称提供者（FieldType::Enum 使用） */
		EnumNamesFunc GetEnumNames{ nullptr };
	};

	/* 含嵌套对象/资源引用的字段无法用「偏移 + 类型」表达，改用回调整块自定义绘制；返回 true 表示组件数据被改动 */
	using ComponentDrawFunc = bool (*)(void* component);

	/* 组件级可见性条件：返回 false 时整个组件块不显示（如组件持有的嵌套对象为空） */
	using ComponentVisibleFunc = bool (*)(const void* component);

	/* 自定义序列化：字段表负责 schema 字段，这里补充字段表表达不了的内容（资源引用路径等）。 */
	using ComponentSaveFunc = void (*)(tinyxml2::XMLElement* element, const void* component);
	using ComponentLoadFunc = void (*)(const tinyxml2::XMLElement* element, void* component);

	/* 类型擦除访问器：让 UI 无需知道具体组件类型即可 has / get / remove / add */
	using HasFunc    = bool (*)(Entity& entity);
	using GetPtrFunc = void* (*)(Entity& entity);
	using RemoveFunc = void (*)(Entity& entity);
	using AddFunc    = void (*)(Entity& entity);

	/* Add 菜单的一个条目：同一组件可以有多种可添加形态（如不同光源类型）。
	 * MenuName 为空表示使用组件名。 */
	struct ComponentVariant
	{
		const char* MenuName{ nullptr };
		AddFunc     Add{ nullptr };
	};

	struct ComponentDesc
	{
		const char*            Name{ nullptr };
		std::type_index        Type{ typeid(void) };
		std::vector<FieldDesc> Fields;

		HasFunc    Has{ nullptr };
		GetPtrFunc GetPtr{ nullptr };
		RemoveFunc Remove{ nullptr };
		AddFunc    Add{ nullptr };

		/* 可添加形态。未显式声明时，Register() 以默认构造补上一条。 */
		std::vector<ComponentVariant> Variants;

		ComponentDrawFunc CustomDraw{ nullptr };

		/* 组件级可见性条件：为空则始终显示 */
		ComponentVisibleFunc Visible{ nullptr };

		/* 组件数据快照：供撤销「移除组件 / 删除实体」还原内容 */
		ComponentCaptureFunc Capture{ nullptr };
		ComponentRestoreFunc Restore{ nullptr };

		/* 自定义序列化：SaveExtra 在字段表之前调用，属性顺序与既有场景文件一致 */
		ComponentSaveFunc SaveExtra{ nullptr };
		ComponentLoadFunc LoadExtra{ nullptr };

		/* 是否参与场景序列化 */
		bool bSerializable{ true };

		/* 是否出现在 "Add Component" 菜单（Name / Transform 由 CreateEntity 自动带上） */
		bool bAddable{ true };
	};

	/* 按字段表把组件写入 / 读出 XML 元素（Scene 序列化使用） */
	void SaveComponentToXml(const ComponentDesc& desc, const void* component, tinyxml2::XMLElement* element);
	void LoadComponentFromXml(const ComponentDesc& desc, void* component, const tinyxml2::XMLElement* element);

	/* 取字段当前值的地址：偏移字段直接取址，访问器字段经 Get 拷进 buffer 后返回 buffer。
	 * 用于编辑前后比较（属性面板据此生成改动命令）。 */
	const void* ReadFieldValue(const FieldDesc& field, const void* component, void* buffer);

	/* 求成员在类内的字节偏移（用真实对象取址，避免 UB） */
	template <typename T, typename M>
	size_t MemberOffset(M T::* member)
	{
		static T s_Instance{};
		return static_cast<size_t>(
			reinterpret_cast<const char*>(&(s_Instance.*member)) -
			reinterpret_cast<const char*>(&s_Instance));
	}

	/* 由「依赖成员 + 比较方式」构造条件（数据化，可导出为 schema JSON） */
	template <typename T, typename M>
	FieldCondition MakeMemberCondition(M T::* member, ConditionOp op, double expected = 0.0)
	{
		FieldCondition condition;
		condition.DependOffset = MemberOffset(member);
		condition.DependType = FieldTypeOf<M>::Value;
		condition.Op = op;
		condition.Expected = expected;
		return condition;
	}

	/* 判定条件是否满足 */
	[[nodiscard]] bool EvaluateCondition(const FieldCondition& condition, const void* component);

	class ComponentRegistry
	{
	public:
		static ComponentRegistry& Instance();

		[[nodiscard]] const ComponentDesc* Find(std::type_index type) const;
		[[nodiscard]] const std::vector<ComponentDesc>& All() const { return m_Components; }

		void Add(ComponentDesc desc) { m_Components.emplace_back(std::move(desc)); }

	private:
		ComponentRegistry() = default;

		std::vector<ComponentDesc> m_Components;
	};

	/* 链式注册构建器：以临时对象使用，末尾 .Register() 提交 */
	class ComponentRegistrar
	{
	public:
		ComponentRegistrar(const char* name, std::type_index type);

		template <typename T, typename M>
		ComponentRegistrar& Field(M T::* member, const char* name, FieldType type,
		                          float drag_speed = 0.1f, float reset_value = 0.0f,
		                          const char* semantics = nullptr)
		{
			FieldDesc desc;
			desc.Name = name;
			desc.Type = type;
			desc.Offset = MemberOffset(member);
			desc.ValueSize = sizeof(M);
			desc.DragSpeed = drag_speed;
			desc.ResetValue = reset_value;
			desc.Semantics = semantics;
			m_Desc.Fields.emplace_back(desc);
			return *this;
		}

		/* 访问器字段：值不在组件体内，由 MakeAccessor / MakeEnumAccessor 提供的回调读写 */
		ComponentRegistrar& Field(FieldAccessor accessor, const char* name, FieldType type,
		                          float drag_speed = 0.1f, float reset_value = 0.0f,
		                          const char* semantics = nullptr);

		ComponentRegistrar& Accessors(HasFunc has, GetPtrFunc get, RemoveFunc remove, AddFunc add);
		ComponentRegistrar& CustomDraw(ComponentDrawFunc draw);

		/* 组件数据快照的读写（MakeRegistrar 已按组件类型生成，通常无需手工调用） */
		ComponentRegistrar& Snapshot(ComponentCaptureFunc capture, ComponentRestoreFunc restore);

		/* 组件级可见性条件：不满足时整个组件块不显示 */
		ComponentRegistrar& Visible(ComponentVisibleFunc visible);

		/* 序列化属性名：作用于最近添加的那个字段 */
		ComponentRegistrar& SerializeName(const char* name);

		/* 自定义序列化：字段表之外的补充内容 */
		ComponentRegistrar& SaveExtra(ComponentSaveFunc save);
		ComponentRegistrar& LoadExtra(ComponentLoadFunc load);

		/* 不参与场景序列化 */
		ComponentRegistrar& NotSerialized();

		ComponentRegistrar& NotAddable();

		/* 追加一个可添加形态（menu_name 为空表示用组件名）。
		 * 声明了形态后菜单只用它们；Add 仍保留默认构造，供反序列化创建组件。 */
		ComponentRegistrar& Variant(const char* menu_name, AddFunc add);

		/* 枚举名称：作用于最近添加的那个字段 */
		template <typename Enum>
		ComponentRegistrar& EnumOf()
		{
			return EnumNames([]() -> std::vector<std::string> { return Helios::GetEnumNames<Enum>(); });
		}

		ComponentRegistrar& EnumNames(EnumNamesFunc names);

		/* 可见性条件：作用于最近添加的那个字段，须紧跟在一个 .Field(...) 之后 */
		ComponentRegistrar& When(FieldCondition condition);

		template <typename T>
		ComponentRegistrar& WhenTrue(bool T::* member)
		{
			return When(MakeMemberCondition(member, ConditionOp::IsTrue));
		}

		template <typename T>
		ComponentRegistrar& WhenFalse(bool T::* member)
		{
			return When(MakeMemberCondition(member, ConditionOp::IsFalse));
		}

		template <typename T, typename M>
		ComponentRegistrar& WhenEq(M T::* member, double expected)
		{
			return When(MakeMemberCondition(member, ConditionOp::Equal, expected));
		}

		/* 提交到注册表 */
		void Register();

	private:
		ComponentDesc m_Desc;
	};
}
