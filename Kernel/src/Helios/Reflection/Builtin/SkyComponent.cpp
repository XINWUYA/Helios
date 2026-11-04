#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	void RegisterSkyComponent()
	{
		/* 天空与昼夜：分节按参数实际作用于什么来切 ——
		 * 时刻管一天走到哪，观测点管太阳轨迹怎么斜、朝哪边，
		 * 日期管赤纬（夏至的太阳比冬至高）与月相，三者互不替代，各占一节。
		 * 自动推进打开后才需要走完一天的真实时长，关掉时该字段不写出。
		 * 时间与天文的几项都有确定区间，面板按它钳制拖拽与手输；
		 * 时刻与年内第几天要来回扫，用滑块拖比拖拽手感更直接。
		 * 曝光与日月星强度都是"天空有多亮"的缩放，归一节；
		 * 雾霾只改米氏散射的浑浊度，不碰亮度，所以单列。 */
		MakeRegistrar<SkyComponent>("Sky")
			.Group("Time")
			.Field(&SkyComponent::m_Hour, "Hour", FieldType::Float, 0.02f, 10.0f)
			.Range(0.0f, 24.0f)
			.Slider()
			.Field(&SkyComponent::m_AutoAdvance, "AutoAdvance", FieldType::Bool)
			.Field(&SkyComponent::m_DayLengthSeconds, "DayLength", FieldType::Float, 5.0f, 600.0f)
			.WhenTrue(&SkyComponent::m_AutoAdvance)
			.Group("Location")
			.Field(&SkyComponent::m_Latitude, "Latitude", FieldType::Float, 0.2f, 30.0f)
			.Range(-90.0f, 90.0f)
			.Field(&SkyComponent::m_NorthAngle, "NorthAngle", FieldType::Float, 0.5f)
			.Range(0.0f, 360.0f)
			.Group("Date")
			.Field(&SkyComponent::m_DayOfYear, "DayOfYear", FieldType::Int, 1.0f, 172.0f)
			.Range(1.0f, 366.0f)
			.Slider()
			.Field(&SkyComponent::m_MoonPhase, "MoonPhase", FieldType::Float, 0.005f, 0.5f)
			.Range(0.0f, 1.0f)
			.Group("Brightness")
			.Field(&SkyComponent::m_SunIntensity, "SunIntensity", FieldType::Float, 0.05f, 5.0f)
			.Field(&SkyComponent::m_MoonIntensity, "MoonIntensity", FieldType::Float, 0.002f, 0.12f)
			.Field(&SkyComponent::m_StarIntensity, "StarIntensity", FieldType::Float, 0.01f, 1.0f)
			.Group("Atmosphere")
			.Field(&SkyComponent::m_Haze, "Haze", FieldType::Float, 0.01f, 1.0f)
			.Range(0.0f, 1.0f)
			/* 曝光接着亮度那一节（分组按名字归拢，面板里挨在一起） */
			.Group("Brightness")
			.Field(&SkyComponent::m_Exposure, "Exposure", FieldType::Float, 0.01f, 1.5f)
			.Register();
	}
}
