#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Camera.h"
#include "Model.h"
#include "Light.h"
#include "ReflectionProbe.h"

namespace Helios
{
	class Scene;
	class Entity;

	/* 组件基类 */
	struct ComponentBase
	{
		virtual ~ComponentBase() = default;

		virtual void OnAdded(const Scene& scene, Entity& entity) {}
		virtual void OnRemoved(const Scene& scene, Entity& entity) {}
	};

	/* 实体名称组件 */
	struct NameComponent : ComponentBase
	{
		std::string m_Name;

		NameComponent() = default;
		NameComponent(const NameComponent&) = default;
		NameComponent(std::string name)
			: m_Name(std::move(name))
		{
		}
	};

	/* 空间变换组件 */
	struct TransformComponent : ComponentBase
	{
		glm::vec3 m_Position{ 0.0f, 0.0f, 0.0f };
		glm::vec3 m_Rotation{ 0.0f, 0.0f, 0.0f };
		glm::vec3 m_Scale{ 1.0f, 1.0f, 1.0f };

		TransformComponent() = default;
		TransformComponent(const TransformComponent&) = default;
		TransformComponent(const glm::vec3& position)
			: m_Position(position)
		{
		}

		glm::mat4 GetTransform() const
		{
			const glm::mat4 rotation_mat = glm::toMat4(glm::quat(m_Rotation));
			return glm::translate(glm::mat4(1.0f), m_Position)
				* rotation_mat
				* glm::scale(glm::mat4(1.0f), m_Scale);
		}
	};

	/* 父子层级：只有子实体持有。父子关系只有这一处数据源，子节点列表按需从注册表遍历出来
	 * （不额外维护）；根节点不挂。Transform 存相对父节点的局部变换，世界变换沿父链累积，
	 * 读写一律走 Scene 的层级接口。 */
	struct ParentComponent : ComponentBase
	{
		entt::entity m_Parent{ entt::null };

		ParentComponent() = default;
		ParentComponent(const ParentComponent&) = default;
		ParentComponent(entt::entity parent)
			: m_Parent(parent)
		{
		}
	};

	/* 场景相机组件 */
	struct CameraComponent : ComponentBase
	{
		SharedPtr<Camera> m_Camera{ nullptr };
		bool m_IsPrimary{ true };
		bool m_IsFixedAspectRatio{ false };

		CameraComponent()
		{
			m_Camera = CreateSharedPtr<Camera>();
		}
		CameraComponent(const CameraComponent&) = default;
		CameraComponent(const SharedPtr<Camera>& camera)
		{
			if (camera)
				m_Camera = camera;
			else
				m_Camera = CreateSharedPtr<Camera>();

		}

	};

	/* 图片精灵组件 */
	struct SpriteComponent : ComponentBase
	{
		SharedPtr<DeviceTexture> m_Texture{ nullptr };
		glm::vec4 m_BaseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		float m_TilingFactor{ 1.0f };

		SpriteComponent() = default;
		SpriteComponent(const SpriteComponent&) = default;
		SpriteComponent(const glm::vec4& basecolor)
			: m_BaseColor(basecolor)
		{
		}
	};

	/* 模型组件 */
	struct ModelComponent : ComponentBase
	{
		SharedPtr<Model> m_Model{ nullptr };

		ModelComponent() = default;
		ModelComponent(const ModelComponent&) = default;
	};

	/* 光源组件 */
	struct LightComponent : ComponentBase
	{
		SharedPtr<Light> m_Light{ nullptr };
		LightType m_Type{ LightType::Point };

		LightComponent() = default;
		LightComponent(const LightComponent&) = default;
		LightComponent(LightType type)
			: m_Type(type)
		{
			m_Light = Light::Create(type);
		}
	};

	/* 反射探针组件 */
	struct ReflectionProbeComponent : ComponentBase
	{
		SharedPtr<ReflectionProbe> m_ReflectionProbe;

		ReflectionProbeComponent()
		{
			m_ReflectionProbe = CreateSharedPtr<ReflectionProbe>();
		}
		ReflectionProbeComponent(const ReflectionProbeComponent&) = default;

		/* 加入场景时注册到反射探针管理器 */
		void OnAdded(const Scene& scene, Entity& entity) override;
		/* 移除出场景时从反射探针管理器注销 */
		void OnRemoved(const Scene& scene, Entity& entity) override;
	};

	/* 场景中需要接入on_construct/on_destroy信号分发的全部组件类型 */
	using SceneComponentList = std::tuple<
		NameComponent,
		TransformComponent,
		SpriteComponent,
		CameraComponent,
		ModelComponent,
		LightComponent,
		ReflectionProbeComponent
	>;
}
