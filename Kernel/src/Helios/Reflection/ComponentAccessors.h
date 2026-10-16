#pragma once
#include <cstddef>
#include <type_traits>
#include "Helios/Common/Common.h"

namespace Helios
{
	/* ---- 访问器字段 ----
	 * 值在组件持有的嵌套对象里（比如 camera->GetFov()），字节偏移表达不了，改用一对类型擦除的
	 * 回调读写；两者都不为空时 FieldDesc::Offset 就不再用了。 */

	using FieldGetFunc = void (*)(const void* component, void* out_value);
	using FieldSetFunc = void (*)(void* component, const void* in_value);

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
}
