#include "Pch.h"
#include "ComponentRegistry.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/Common/Utils.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include <imgui.h>
#include <tinyxml2.h>

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

	namespace
	{
		/* 字段值所在地址：偏移字段直接取址，访问器字段拷进缓冲 */
		const void* ResolveFieldAddress(const FieldDesc& field, const void* component, void* buffer)
		{
			if (field.Get != nullptr)
			{
				field.Get(component, buffer);
				return buffer;
			}
			return static_cast<const uint8_t*>(component) + field.Offset;
		}

		/* 反序列化目标地址：访问器字段先写进缓冲，随后由 CommitFieldAddress 提交 */
		void* ResolveFieldTarget(const FieldDesc& field, void* component, void* buffer)
		{
			if (field.Set != nullptr)
				return buffer;
			return static_cast<uint8_t*>(component) + field.Offset;
		}

		void CommitFieldAddress(const FieldDesc& field, void* component, void* target)
		{
			if (field.Set != nullptr)
				field.Set(component, target);
		}

		/* 属性名：默认取字段名，注册时可用 .SerializeName() 覆盖 */
		const char* SerializeAttributeName(const FieldDesc& field)
		{
			return (field.SerializeName != nullptr) ? field.SerializeName : field.Name;
		}
	}

	void SaveComponentToXml(const ComponentDesc& desc, const void* component, tinyxml2::XMLElement* element)
	{
		if (element == nullptr || component == nullptr)
			return;

		/* 自定义内容先写，使属性顺序与既有场景文件一致 */
		if (desc.SaveExtra != nullptr)
			desc.SaveExtra(element, component);

		alignas(16) uint8_t buffer[kFieldValueCapacity] = {};

		for (const FieldDesc& field : desc.Fields)
		{
			/* 条件不满足的字段不写入（如透视相机不写 HeightSize） */
			if (!EvaluateCondition(field.Condition, component))
				continue;

			const char* name = SerializeAttributeName(field);
			const void* value = ResolveFieldAddress(field, component, buffer);

			switch (field.Type)
			{
			case FieldType::Bool:   element->SetAttribute(name, *static_cast<const bool*>(value)); break;
			case FieldType::Float:  element->SetAttribute(name, *static_cast<const float*>(value)); break;
			case FieldType::Int:
			case FieldType::Enum:   element->SetAttribute(name, *static_cast<const int*>(value)); break;
			case FieldType::Vec2:   element->SetAttribute(name, ToString(*static_cast<const glm::vec2*>(value)).c_str()); break;
			case FieldType::Vec3:   element->SetAttribute(name, ToString(*static_cast<const glm::vec3*>(value)).c_str()); break;
			case FieldType::Vec4:
			case FieldType::Color:  element->SetAttribute(name, ToString(*static_cast<const glm::vec4*>(value)).c_str()); break;
			case FieldType::String: element->SetAttribute(name, static_cast<const std::string*>(value)->c_str()); break;
			default: break;
			}
		}
	}

	void LoadComponentFromXml(const ComponentDesc& desc, void* component, const tinyxml2::XMLElement* element)
	{
		if (element == nullptr || component == nullptr)
			return;

		const auto load_field = [element, component](const FieldDesc& field)
		{
			/* 属性名不存在即视为该字段未保存（条件不满足时保存端不会写），无需再判条件 */
			const tinyxml2::XMLAttribute* attribute = element->FindAttribute(SerializeAttributeName(field));
			if (attribute == nullptr)
				return;

			alignas(16) uint8_t buffer[kFieldValueCapacity] = {};
			void* target = ResolveFieldTarget(field, component, buffer);

			switch (field.Type)
			{
			case FieldType::Bool:   *static_cast<bool*>(target) = attribute->BoolValue(); break;
			case FieldType::Float:  *static_cast<float*>(target) = attribute->FloatValue(); break;
			case FieldType::Int:
			case FieldType::Enum:   *static_cast<int*>(target) = attribute->IntValue(); break;
			case FieldType::Vec2:   *static_cast<glm::vec2*>(target) = ToVec2(attribute->Value()); break;
			case FieldType::Vec3:   *static_cast<glm::vec3*>(target) = ToVec3(attribute->Value()); break;
			case FieldType::Vec4:
			case FieldType::Color:  *static_cast<glm::vec4*>(target) = ToVec4(attribute->Value()); break;
			case FieldType::String: *static_cast<std::string*>(target) = attribute->Value(); break;
			default: return;
			}

			CommitFieldAddress(field, component, target);
		};

		for (const FieldDesc& field : desc.Fields)
			load_field(field);

		if (desc.LoadExtra != nullptr)
			desc.LoadExtra(element, component);
	}

	namespace
	{
		/* ---- 自定义绘制：含嵌套对象或资源引用的组件 ----
		 * 这些字段靠访问器读写（如 camera->GetFov()），无法用「偏移 + 类型」表达，整块交给回调。 */

		/* 贴图与模型是资源引用：字段表只认 POD 与字符串，路径读写单独处理；
		 * 对象为空时不写属性，读的时候也接受属性缺失。 */
		void SaveSpriteTexture(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& texture = static_cast<const SpriteComponent*>(component)->m_Texture;
			if (texture != nullptr)
				element->SetAttribute("TexturePath", RELATIVE_PATH(texture->GetPath()).c_str());
		}

		void LoadSpriteTexture(const tinyxml2::XMLElement* element, void* component)
		{
			const char* path = element->Attribute("TexturePath");
			if (path != nullptr && *path != '\0')
				static_cast<SpriteComponent*>(component)->m_Texture = DeviceTexture::Create(path);
		}

		void SaveModelPath(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& model = static_cast<const ModelComponent*>(component)->m_Model;
			if (model != nullptr)
				element->SetAttribute("ModelPath", RELATIVE_PATH(model->GetPath()).c_str());
		}

		void LoadModelPath(const tinyxml2::XMLElement* element, void* component)
		{
			const char* path = element->Attribute("ModelPath");
			if (path != nullptr && *path != '\0')
				static_cast<ModelComponent*>(component)->m_Model = Model::Create(path);
		}

		/* 贴图是资源引用，需要拖入与悬停预览，故整块自定义绘制；
		 * BaseColor 与 TilingFactor 走 schema 字段（见注册处）。 */
		void DrawSpriteBlock(void* raw)
		{
			auto& component = *static_cast<SpriteComponent*>(raw);
			ImGuiExt::DrawTextureUI("Texture", component.m_Texture);
		}

		/* 投影类型决定哪些参数有意义：透视看 Fov，正交看 HeightSize */
		bool IsPerspectiveProjection(const void* component)
		{
			const auto& camera = static_cast<const CameraComponent*>(component)->m_Camera;
			return camera != nullptr && camera->GetProjectionType() == CameraProjectionType::Perspective;
		}

		bool IsOrthographicProjection(const void* component)
		{
			const auto& camera = static_cast<const CameraComponent*>(component)->m_Camera;
			return camera != nullptr && camera->GetProjectionType() == CameraProjectionType::Orthographic;
		}

		void DrawModelBlock(void* raw)
		{
			auto& component = *static_cast<ModelComponent*>(raw);
			auto& model = component.m_Model;
			if (!model)
				return;

			ImGui::PushID("ModelPath");
			ImGui::Columns(2);

			ImGui::SetColumnWidth(0, 100);
			ImGui::Text("ModelPath");
			ImGui::NextColumn();
			ImGui::TextWrapped("%s", model->GetPath().c_str());

			ImGui::Columns(1);
			ImGui::PopID();
		}

		/* 光源类型决定持有哪个 Light 对象，切换时必须重建；
		 * 颜色 / 强度 / 投影这些公共属性沿用切换前的值。 */
		FieldAccessor MakeLightTypeAccessor()
		{
			FieldAccessor accessor;
			accessor.ValueSize = sizeof(int);

			accessor.Get = [](const void* component, void* out_value)
			{
				const auto& light = static_cast<const LightComponent*>(component)->m_Light;
				if (light != nullptr)
					*static_cast<int*>(out_value) = static_cast<int>(light->GetLightType());
			};

			accessor.Set = [](void* component, const void* in_value)
			{
				auto& light_component = *static_cast<LightComponent*>(component);
				const auto type = static_cast<LightType>(*static_cast<const int*>(in_value));

				if (light_component.m_Light != nullptr && light_component.m_Light->GetLightType() == type)
					return;

				glm::vec4 color{ 1.0f };
				float intensity = 1.0f;
				bool cast_shadow = false;
				if (light_component.m_Light != nullptr)
				{
					color = light_component.m_Light->GetColor();
					intensity = light_component.m_Light->GetIntensity();
					cast_shadow = light_component.m_Light->IsCastShadow();
				}

				light_component.m_Type = type;
				light_component.m_Light = Light::Create(type);
				if (light_component.m_Light != nullptr)
				{
					light_component.m_Light->SetColor(color);
					light_component.m_Light->SetIntensity(intensity);
					light_component.m_Light->SetIsCastShadow(cast_shadow);
				}
			};

			return accessor;
		}

		/* 嵌套对象为空时整个组件块不显示 */
		bool HasLightObject(const void* component)
		{
			return static_cast<const LightComponent*>(component)->m_Light != nullptr;
		}

		/* 用组件类型生成带访问器的注册器 */
		template <typename T>
		ComponentRegistrar MakeRegistrar(const char* name)
		{
			ComponentRegistrar registrar(name, std::type_index(typeid(T)));
			registrar.Accessors(
				[](Entity& e) -> bool { return e.HasComponent<T>(); },
				[](Entity& e) -> void* { return static_cast<void*>(&e.GetComponent<T>()); },
				[](Entity& e) { e.RemoveComponent<T>(); },
				[](Entity& e) { e.AddComponent<T>(); });
			return registrar;
		}

		/* ==================== 内置组件注册 ====================
		 * 新增组件在这补一段就行，Inspector 和 Add 菜单自动生效。注册对象靠 ComponentRegistry 同 TU
		 * 被引用而链进来（静态库不会丢弃本文件）。 */
		struct BuiltinComponentRegistration
		{
			BuiltinComponentRegistration()
			{
				MakeRegistrar<NameComponent>("Name")
					.Field(&NameComponent::m_Name, "Name", FieldType::String)
					.NotSerialized()
					.NotAddable()
					.Register();

				MakeRegistrar<TransformComponent>("Transform")
					.Field(&TransformComponent::m_Position, "Position", FieldType::Vec3, 0.1f, 0.0f)
					/* 内部以弧度存储、Inspector 按角度编辑 */
					.Field(&TransformComponent::m_Rotation, "Rotation", FieldType::Vec3, 0.1f, 0.0f, "AngleDeg")
					.Field(&TransformComponent::m_Scale,    "Scale",    FieldType::Vec3, 0.1f, 1.0f)
					.NotAddable()
					.Register();

				MakeRegistrar<SpriteComponent>("Sprite")
					.SaveExtra(&SaveSpriteTexture)
					.LoadExtra(&LoadSpriteTexture)
					.Field(&SpriteComponent::m_BaseColor, "BaseColor", FieldType::Color)
					.Field(&SpriteComponent::m_TilingFactor, "TilingFactor", FieldType::Float, 0.1f, 1.0f)
					/* 未设置贴图时 TilingFactor 无意义，条件不满足则不显示（值保留） */
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::IsTrue, 0.0,
						[](const void* component)
						{
							return static_cast<const SpriteComponent*>(component)->m_Texture != nullptr;
						} })
					.CustomDraw(&DrawSpriteBlock)
					.Register();

				MakeRegistrar<CameraComponent>("Camera")
					.Field(&CameraComponent::m_IsPrimary, "IsPrimary", FieldType::Bool)
					.Field(&CameraComponent::m_IsFixedAspectRatio, "IsFixedAspectRatio", FieldType::Bool)
					/* 投影参数在嵌套的 Camera 对象里，经访问器读写 */
					.Field(MakeEnumAccessor<&CameraComponent::m_Camera,
						&Camera::GetProjectionType, &Camera::SetProjectionType>(),
						"ProjectionType", FieldType::Enum)
					.EnumOf<CameraProjectionType>()
					.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetFov, &Camera::SetFov>(),
						"Fov", FieldType::Float)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::IsTrue, 0.0, &IsPerspectiveProjection })
					.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetHeightSize, &Camera::SetHeightSize>(),
						"HeightSize", FieldType::Float)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::IsTrue, 0.0, &IsOrthographicProjection })
					.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetNearClip, &Camera::SetNearClip>(),
						"NearClip", FieldType::Float)
					.SerializeName("Near")
					.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetFarClip, &Camera::SetFarClip>(),
						"FarClip", FieldType::Float)
					.SerializeName("Far")
					.Register();

				MakeRegistrar<ModelComponent>("Model")
					.SaveExtra(&SaveModelPath)
					.LoadExtra(&LoadModelPath)
					.CustomDraw(&DrawModelBlock)
					.Register();

				/* 光源对象由构造参数决定，故逐类声明可添加形态；菜单名即光源类型名。 */
				MakeRegistrar<LightComponent>("Light")
					.Field(MakeLightTypeAccessor(), "Type", FieldType::Enum)
					.EnumOf<LightType>()
					.SerializeName("LightType")
					.Field(MakeAccessor<&LightComponent::m_Light, &Light::GetColor, &Light::SetColor>(),
						"Color", FieldType::Color)
					.SerializeName("LightColor")
					.Field(MakeAccessor<&LightComponent::m_Light, &Light::GetIntensity, &Light::SetIntensity>(),
						"Intensity", FieldType::Float)
					.SerializeName("LightIntensity")
					.Field(MakeAccessor<&LightComponent::m_Light, &Light::IsCastShadow, &Light::SetIsCastShadow>(),
						"CastShadow", FieldType::Bool)
					.SerializeName("IsCastShadow")
					.Visible(&HasLightObject)
					.Variant("Directional Light", [](Entity& entity)
						{
							entity.AddComponent<LightComponent>(LightType::Directional);
						})
					.Variant("Point Light", [](Entity& entity)
						{
							entity.AddComponent<LightComponent>(LightType::Point);
						})
					.Variant("Spot Light", [](Entity& entity)
						{
							entity.AddComponent<LightComponent>(LightType::Spot);
						})
					.Register();
			}
		};

		const BuiltinComponentRegistration s_BuiltinComponentRegistration;
	}
}
