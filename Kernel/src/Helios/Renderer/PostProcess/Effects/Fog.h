#pragma once
#include "Helios/Scene/Components.h"
#include "Helios/Renderer/PostProcess/PostProcessEffectRegistry.h"

namespace Helios
{
	/* 雾渲染侧设置：解析后进视图快照（场景里没有后处理对象 = 关闭） */
	struct FogSettings
	{
		bool  Enabled{ false };
		float Density{ 0.02f };        /* 基准高度处的消光系数（1 / 世界单位） */
		float HeightFalloff{ 0.05f };  /* 高度衰减，0 = 均匀雾 */
		float BaseHeight{ 0.0f };      /* 密度基准高度（世界 Y） */
		float StartDistance{ 0.0f };   /* 这段距离以内不施雾 */
		float MaxOpacity{ 1.0f };      /* 最大不透明度，留一点底 */
		float SkyAffect{ 1.0f };       /* 天空受雾的权重，0 = 天空不吃雾 */
		bool  UseSkyColor{ true };     /* 雾色跟天空同源，随昼夜自动变 */
		glm::vec3 Color{ 0.68f, 0.76f, 0.88f }; /* 关掉同源时手填的雾色 */
		float SunScatter{ 0.5f };      /* 朝太阳方向的内散射强度 */
		float Anisotropy{ 0.6f };      /* 米氏相函数各向异性 */
	};

	/* 雾场景侧组件：挂在「后处理对象」实体上，与 BloomComponent 同构。
	 * 字段扁平存储，解析时经 GetFogSettings 组装成快照值。 */
	struct FogComponent : ComponentBase
	{
		bool  m_Enabled{ true };
		float m_Density{ 0.02f };
		float m_HeightFalloff{ 0.05f };
		float m_BaseHeight{ 0.0f };
		float m_StartDistance{ 0.0f };
		float m_MaxOpacity{ 1.0f };
		float m_SkyAffect{ 1.0f };
		bool  m_UseSkyColor{ true };
		glm::vec3 m_Color{ 0.68f, 0.76f, 0.88f };
		float m_SunScatter{ 0.5f };
		float m_Anisotropy{ 0.6f };

		[[nodiscard]] FogSettings GetFogSettings() const
		{
			return FogSettings{ m_Enabled, m_Density, m_HeightFalloff, m_BaseHeight,
				m_StartDistance, m_MaxOpacity, m_SkyAffect, m_UseSkyColor, m_Color, m_SunScatter,
				m_Anisotropy };
		}
	};

	namespace PostProcess
	{
		/* 效果自含登记（由内置清单调用） */
		void RegisterFogEffect();
	}
}
