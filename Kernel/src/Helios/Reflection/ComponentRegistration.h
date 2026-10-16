#pragma once
#include "ComponentRegistry.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	/* 用组件类型生成带访问器与数据快照的注册器：注册场景组件的标准起手式 —— 名称、
	 * has / get / remove / add 与撤销快照全部按类型推导，注册文件接着链式声明字段即可。 */
	template <typename T>
	ComponentRegistrar MakeRegistrar(const char* name)
	{
		ComponentRegistrar registrar(name, std::type_index(typeid(T)));
		registrar.Accessors(
			[](Entity& e) -> bool { return e.HasComponent<T>(); },
			[](Entity& e) -> void* { return static_cast<void*>(&e.GetComponent<T>()); },
			[](Entity& e) { e.RemoveComponent<T>(); },
			[](Entity& e) { e.AddComponent<T>(); });
		registrar.Snapshot(
			[](Entity& e, ComponentSnapshot& out)
			{
				if (e.HasComponent<T>())
					out = ComponentSnapshot(e.GetComponent<T>());
			},
			[](Entity& e, const ComponentSnapshot& in)
			{
				const T* source = in.As<T>();
				if (source == nullptr)
					return;

				if (e.HasComponent<T>())
				{
					e.GetComponent<T>() = *source;
					return;
				}

				/* 按快照直接构造：组件的 OnAdded 会看到最终数据，
				 * 需要把自身注册到场景的组件（如反射探针）才不会挂错对象。 */
				e.AddComponent<T>(*source);
			});
		return registrar;
	}
}
