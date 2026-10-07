#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <string>
#include <unordered_map>
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

	/* 可见性组件：实体自身的显隐开关（层级行右端的眼睛）。「隐藏」= 挂组件且 m_Visible == false
	 * （缺席 = 可见）；沿父链传染，判定收口在 Scene::IsEntityVisible。隐藏的模型不进可见列表、
	 * 光源不参与光照、探针不烘焙；相机不受影响。 */
	struct VisibilityComponent : ComponentBase
	{
		bool m_Visible{ true };

		VisibilityComponent() = default;
		VisibilityComponent(const VisibilityComponent&) = default;
		VisibilityComponent(bool visible)
			: m_Visible(visible)
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

	/* 材质槽的实体级绑定（三态）：条目不存在 = 用模型槽表的默认；IsInstance=false = 覆盖
	 * （挂另一份材质资产，共享）；IsInstance=true = 实例（克隆出来的实体级独立材质）。
	 * 解析链见 ResolveSlotMaterial。 */
	struct MaterialSlotOverride
	{
		SharedPtr<Material> pMaterial; /* 覆盖（共享资产）或实例（独立） */
		std::string SourceAssetPath;   /* 实例的来源资产路径（"重置回来源"用；空 = 无来源） */
		bool IsInstance{ false };      /* 实例：参数独立、序列化全量内嵌 */
	};

	/* 模型组件 */
	struct ModelComponent : ComponentBase
	{
		SharedPtr<Model> m_Model{ nullptr };
		/* 槽名 → 实体级绑定（"覆盖"与"实例"，见 MaterialSlotOverride） */
		std::unordered_map<std::string, MaterialSlotOverride> m_SlotOverrides;
		/* 投影：开启后本模型写入阴影图（见 ShadowMap 的投射体提交）；
		 * 接受阴影：关闭后本模型不采样阴影（见 GBuffer / 前向着色器的 u_ReceiveShadow）。 */
		bool m_CastShadow{ true };
		bool m_ReceiveShadow{ true };

		ModelComponent() = default;

		/* 值语义：拷贝组件时，实例材质必须深拷贝 ——
		 * 组件的撤销快照（Capture/Restore）靠这条把实例参数一起保存 / 还原；
		 * "覆盖 / 默认"是共享资产，SharedPtr 浅拷贝保持共享。 */
		ModelComponent(const ModelComponent& other)
			: ComponentBase(other), m_Model(other.m_Model), m_SlotOverrides(other.m_SlotOverrides),
			  m_CastShadow(other.m_CastShadow), m_ReceiveShadow(other.m_ReceiveShadow)
		{
			for (auto& entry : m_SlotOverrides)
			{
				auto& slot_override = entry.second;
				if (slot_override.IsInstance && slot_override.pMaterial != nullptr)
					slot_override.pMaterial = slot_override.pMaterial->Clone();
			}
		}

		ModelComponent& operator=(const ModelComponent& other)
		{
			if (this != &other)
			{
				ModelComponent copy(other); /* 走深拷贝构造 */
				std::swap(m_Model, copy.m_Model);
				std::swap(m_SlotOverrides, copy.m_SlotOverrides);
				std::swap(m_CastShadow, copy.m_CastShadow);
				std::swap(m_ReceiveShadow, copy.m_ReceiveShadow);
			}
			return *this;
		}
	};

	/* 解析一个 MeshSegment 的最终材质（全引擎唯一入口）：
	 * 实体绑定（覆盖 / 实例） ?: 模型槽表默认 ?: 内置白模 */
	inline SharedPtr<Material> ResolveSlotMaterial(
		const Model& model, int slot_index,
		const std::unordered_map<std::string, MaterialSlotOverride>* slot_overrides)
	{
		const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
		if (slot == nullptr)
			return Material::BuiltinWhite();

		if (slot_overrides != nullptr)
		{
			const auto iter = slot_overrides->find(slot->Name);
			if (iter != slot_overrides->end() && iter->second.pMaterial != nullptr)
				return iter->second.pMaterial;
		}

		return (slot->pDefault != nullptr) ? slot->pDefault : Material::BuiltinWhite();
	}

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
