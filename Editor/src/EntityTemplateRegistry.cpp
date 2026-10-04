#include "Pch.h"
#include "EntityTemplateRegistry.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	EntityTemplateRegistry& EntityTemplateRegistry::Instance()
	{
		/* 函数内静态：首次使用时构造，规避跨 TU 的静态初始化顺序问题
		 * （内部注册对象会在静态初始化期调用本函数）。 */
		static EntityTemplateRegistry s_Instance;
		return s_Instance;
	}

	EntityTemplateRegistrar::EntityTemplateRegistrar(const char* name)
	{
		m_Desc.Name = name;
	}

	EntityTemplateRegistrar& EntityTemplateRegistrar::Component(AddFunc add)
	{
		m_Desc.Components.push_back(add);
		return *this;
	}

	void EntityTemplateRegistrar::Register()
	{
		EntityTemplateRegistry::Instance().Add(std::move(m_Desc));
	}

	namespace
	{
		/* ==================== 内置实体预设 ====================
		 * 新增预设在这补一段就行、右键菜单自动生效。注册对象靠 Instance() 同 TU 被引用而链进来
		 * （静态库不会丢弃本文件）。 */
		struct BuiltinEntityTemplateRegistration
		{
			BuiltinEntityTemplateRegistration()
			{
				EntityTemplateRegistrar("Empty")
					.Register();

				EntityTemplateRegistrar("Sprite")
					.Component([](Entity& entity) { entity.AddComponent<SpriteComponent>(); })
					.Register();

				EntityTemplateRegistrar("Camera")
					.Component([](Entity& entity) { entity.AddComponent<CameraComponent>(); })
					.Register();

				EntityTemplateRegistrar("Model")
					.Component([](Entity& entity) { entity.AddComponent<ModelComponent>(); })
					.Register();

				EntityTemplateRegistrar("Reflection Probe")
					.Component([](Entity& entity) { entity.AddComponent<ReflectionProbeComponent>(); })
					.Register();

				EntityTemplateRegistrar("Directional Light")
					.Component([](Entity& entity) { entity.AddComponent<LightComponent>(LightType::Directional); })
					.Register();

				EntityTemplateRegistrar("Point Light")
					.Component([](Entity& entity) { entity.AddComponent<LightComponent>(LightType::Point); })
					.Register();

				EntityTemplateRegistrar("Spot Light")
					.Component([](Entity& entity) { entity.AddComponent<LightComponent>(LightType::Spot); })
					.Register();
			}
		};

		const BuiltinEntityTemplateRegistration s_BuiltinEntityTemplateRegistration;
	}
}
