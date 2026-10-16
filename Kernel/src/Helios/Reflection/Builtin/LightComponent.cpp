#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"
#include <algorithm>
#include <type_traits>

namespace Helios
{
	namespace
	{
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
				/* 点光 / 聚光的共有范围：旧光有则沿用；旧光没有（方向光）则用新光的默认值 */
				float range = 0.0f;
				bool has_range = false;
				/* 阴影配置（尺寸 / 偏移 / 级联这些调过的值）随对象重建沿用 */
				SharedPtr<ShadowMapInfo> shadow_info;
				if (light_component.m_Light != nullptr)
				{
					color = light_component.m_Light->GetColor();
					intensity = light_component.m_Light->GetIntensity();
					cast_shadow = light_component.m_Light->IsCastShadow();
					shadow_info = light_component.m_Light->GetShadowMapInfo();
					if (const auto* punctual = AsPunctualLight(light_component.m_Light.get()))
					{
						range = punctual->GetRange();
						has_range = true;
					}
				}

				light_component.m_Type = type;
				light_component.m_Light = Light::Create(type);
				if (light_component.m_Light != nullptr)
				{
					light_component.m_Light->SetColor(color);
					light_component.m_Light->SetIntensity(intensity);
					light_component.m_Light->SetIsCastShadow(cast_shadow);
					if (auto* punctual = AsPunctualLight(light_component.m_Light.get()); punctual != nullptr && has_range)
						punctual->SetRange(range);
					if (shadow_info != nullptr && light_component.m_Light->GetShadowMapInfo() != nullptr)
						*light_component.m_Light->GetShadowMapInfo() = *shadow_info;
				}
			};

			return accessor;
		}

		/* ============ 点光 / 聚光的共有与专有参数 ============
		 * Range 长在 PunctualLight、Angle 长在 SpotLight，Light 基类不认识，所以手写访问器按类型取值：
		 * 类型不符时读保持缓冲、写不做任何事（配合字段的条件，两者都不会被触发）。 */

		/* 点光 / 聚光才有的参数 */
		bool IsPunctualLightComponent(const void* component)
		{
			const auto& light = static_cast<const LightComponent*>(component)->m_Light;
			return light != nullptr && AsPunctualLight(light.get()) != nullptr;
		}

		/* 聚光才有的参数（锥角） */
		bool IsSpotLightComponent(const void* component)
		{
			const auto& light = static_cast<const LightComponent*>(component)->m_Light;
			return light != nullptr && light->GetLightType() == LightType::Spot;
		}

		/* 光照范围（点光 / 聚光） */
		FieldAccessor MakeLightRangeAccessor()
		{
			FieldAccessor accessor;
			accessor.ValueSize = sizeof(float);

			accessor.Get = [](const void* component, void* out_value)
			{
				const auto& light = static_cast<const LightComponent*>(component)->m_Light;
				if (light == nullptr)
					return;
				if (const auto* punctual = AsPunctualLight(light.get()))
					*static_cast<float*>(out_value) = punctual->GetRange();
			};

			accessor.Set = [](void* component, const void* in_value)
			{
				const auto& light = static_cast<LightComponent*>(component)->m_Light;
				if (light == nullptr)
					return;
				if (auto* punctual = AsPunctualLight(light.get()))
					punctual->SetRange(*static_cast<const float*>(in_value));
			};

			return accessor;
		}

		/* 外锥全角（度；聚光），编辑时夹取到与渲染端一致的安全范围 */
		FieldAccessor MakeSpotAngleAccessor()
		{
			FieldAccessor accessor;
			accessor.ValueSize = sizeof(float);

			accessor.Get = [](const void* component, void* out_value)
			{
				const auto& light = static_cast<const LightComponent*>(component)->m_Light;
				if (light == nullptr || light->GetLightType() != LightType::Spot)
					return;
				*static_cast<float*>(out_value) = static_cast<const SpotLight*>(light.get())->GetAngle();
			};

			accessor.Set = [](void* component, const void* in_value)
			{
				const auto& light = static_cast<LightComponent*>(component)->m_Light;
				if (light == nullptr || light->GetLightType() != LightType::Spot)
					return;
				auto* spot_light = static_cast<SpotLight*>(light.get());
				spot_light->SetAngle(glm::clamp(*static_cast<const float*>(in_value), 1.0f, 170.0f));
			};

			return accessor;
		}

		/* ============ 阴影参数（ShadowMapInfo） ============
		 * 配置挂在 Light 的 SharedPtr<ShadowMapInfo> 上，用成员指针逐层解引用；光源或配置缺席时
		 * 读保持缓冲、写不做任何事。配置由 SetIsCastShadow(true) 备好，"勾选投影"之后字段立即可用。 */

		/* 勾选"投影"后才有可调的阴影参数（所有类型） */
		bool HasShadowSettings(const void* component)
		{
			const auto& light = static_cast<const LightComponent*>(component)->m_Light;
			return light != nullptr && light->IsCastShadow();
		}

		/* 级联数与阴影距离只对方向光有意义 */
		bool HasDirectionalShadowSettings(const void* component)
		{
			const auto& light = static_cast<const LightComponent*>(component)->m_Light;
			return light != nullptr && light->IsCastShadow() && light->GetLightType() == LightType::Directional;
		}

		/* ShadowMapInfo 标量字段的访问器；ClampMin / ClampMax 为编译期包络，
		 * 只约束写（渲染端另有兜底）：读到的总是实际存储值。
		 * 访问器是裸函数指针，无捕获，故包络走模板参数（C++20 浮点 NTTP）。 */
		template <auto Member, auto ConfigMember, double ClampMin = -1.0e30, double ClampMax = 1.0e30>
		FieldAccessor MakeShadowMapInfoAccessor()
		{
			using Value = typename Detail::MemberValue<decltype(ConfigMember)>::Type;

			FieldAccessor accessor;
			if constexpr (std::is_integral_v<Value>)
				accessor.ValueSize = sizeof(int);
			else
				accessor.ValueSize = sizeof(float);

			accessor.Get = [](const void* component, void* out_value)
			{
				const auto& light = static_cast<const LightComponent*>(component)->m_Light;
				if (light == nullptr)
					return;
				const auto& shadow_info = light->GetShadowMapInfo();
				if (shadow_info == nullptr)
					return;
				if constexpr (std::is_integral_v<Value>)
					*static_cast<int*>(out_value) = static_cast<int>(shadow_info.get()->*ConfigMember);
				else
					*static_cast<float*>(out_value) = static_cast<float>(shadow_info.get()->*ConfigMember);
			};

			accessor.Set = [](void* component, const void* in_value)
			{
				auto& light = static_cast<LightComponent*>(component)->m_Light;
				if (light == nullptr)
					return;
				const auto& shadow_info = light->GetShadowMapInfo();
				if (shadow_info == nullptr)
					return;
				if constexpr (std::is_integral_v<Value>)
				{
					const int clamped = static_cast<int>(
						std::clamp<double>(*static_cast<const int*>(in_value), ClampMin, ClampMax));
					shadow_info.get()->*ConfigMember = static_cast<Value>(clamped);
				}
				else
				{
					shadow_info.get()->*ConfigMember = static_cast<Value>(
						std::clamp<double>(*static_cast<const float*>(in_value), ClampMin, ClampMax));
				}
			};

			return accessor;
		}

		/* 嵌套对象为空时整个组件块不显示 */
		bool HasLightObject(const void* component)
		{
			return static_cast<const LightComponent*>(component)->m_Light != nullptr;
		}
	}

	void RegisterLightComponent()
	{
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
			/* 范围与锥角：条件同时驱动 UI（不适用类型不显示）与序列化（不适用不写出） */
			.Field(MakeLightRangeAccessor(), "Range", FieldType::Float)
			.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &IsPunctualLightComponent })
			.SerializeName("LightRange")
			.Field(MakeSpotAngleAccessor(), "Angle", FieldType::Float)
			.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &IsSpotLightComponent })
			.SerializeName("LightAngle")
			/* 阴影参数：勾选投影后出现（配置由 SetIsCastShadow 备好）；
			 * 级联数与阴影距离只对方向光有意义。条件同时驱动 UI 与序列化。 */
			.Field(MakeShadowMapInfoAccessor<&LightComponent::m_Light, &ShadowMapInfo::Size, 64.0, 8192.0>(),
				"Size", FieldType::Int, 8.0f)
			.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &HasShadowSettings })
			.SerializeName("ShadowSize")
			.Field(MakeShadowMapInfoAccessor<&LightComponent::m_Light, &ShadowMapInfo::ConstantBias>(),
				"Bias", FieldType::Float, 0.001f)
			.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &HasShadowSettings })
			.SerializeName("ShadowBias")
			.Field(MakeShadowMapInfoAccessor<&LightComponent::m_Light, &ShadowMapInfo::CascadeCnt, 1.0, 4.0>(),
				"CascadeCount", FieldType::Int)
			.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &HasDirectionalShadowSettings })
			.SerializeName("CascadeCount")
			.Field(MakeShadowMapInfoAccessor<&LightComponent::m_Light, &ShadowMapInfo::ShadowFar, 0.0, 1.0e30>(),
				"ShadowFar", FieldType::Float, 1.0f)
			.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &HasDirectionalShadowSettings })
			.SerializeName("ShadowFar")
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
}
