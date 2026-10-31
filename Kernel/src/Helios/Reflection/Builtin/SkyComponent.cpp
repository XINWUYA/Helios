#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	void RegisterSkyComponent()
	{
		/* 天空与昼夜：时间 / 观测点决定日月轨迹，曝光与雾霾决定观感。
		 * 自动推进打开后才需要走完一天的真实时长，关掉时该字段不写出。 */
		MakeRegistrar<SkyComponent>("Sky")
			.Field(&SkyComponent::m_Hour, "Hour", FieldType::Float, 0.02f, 10.0f)
			.Field(&SkyComponent::m_AutoAdvance, "AutoAdvance", FieldType::Bool)
			.Field(&SkyComponent::m_DayLengthSeconds, "DayLength", FieldType::Float, 5.0f, 600.0f)
			.WhenTrue(&SkyComponent::m_AutoAdvance)
			.Field(&SkyComponent::m_Latitude, "Latitude", FieldType::Float, 0.2f, 30.0f)
			.Field(&SkyComponent::m_NorthAngle, "NorthAngle", FieldType::Float, 0.5f)
			.Field(&SkyComponent::m_DayOfYear, "DayOfYear", FieldType::Int, 1.0f, 172.0f)
			.Field(&SkyComponent::m_MoonPhase, "MoonPhase", FieldType::Float, 0.005f, 0.5f)
			.Field(&SkyComponent::m_SunIntensity, "SunIntensity", FieldType::Float, 0.05f, 5.0f)
			.Field(&SkyComponent::m_MoonIntensity, "MoonIntensity", FieldType::Float, 0.002f, 0.12f)
			.Field(&SkyComponent::m_StarIntensity, "StarIntensity", FieldType::Float, 0.01f, 1.0f)
			.Field(&SkyComponent::m_Haze, "Haze", FieldType::Float, 0.01f, 1.0f)
			.Field(&SkyComponent::m_Exposure, "Exposure", FieldType::Float, 0.01f, 1.5f)
			.Register();
	}
}
