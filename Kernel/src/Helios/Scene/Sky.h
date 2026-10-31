#pragma once
#include <glm/glm.hpp>

namespace Helios
{
	struct SkyComponent;

	/* 天空与昼夜（Time of Day）。作者参数挂在 SkyComponent 上，每帧换算成 SkyState 推给各视图，
	 * 渲染侧只读快照、不认识场景实体（与后处理同一套分工）。
	 *
	 * 模型是简化的单次散射：太阳 / 月亮位置按真太阳时与赤纬算，大气只做瑞利 + 米氏两档，
	 * 不做多次散射与臭氧吸收 —— 天顶偏暗一点、地平线偏暖一点，观感够用且全部可在着色器里
	 * 逐像素算出来，不需要任何烘焙资产。 */
	namespace Sky
	{
		/* 大气散射系数与标高：CPU 侧算太阳透过率、GPU 侧着色器同源，改一处要同步另一处 */
		struct Atmosphere
		{
			glm::vec3 RayleighBeta{ 5.8e-6f, 13.5e-6f, 33.1e-6f }; /* 1/m，蓝光散射最强 */
			float RayleighScaleHeight{ 8000.0f };                  /* m */
			float MieBeta{ 12.0e-6f };                            /* 1/m */
			float MieScaleHeight{ 1200.0f };                      /* m */
			float MieAnisotropy{ 0.80f };                         /* 米氏相函数的各向异性 */
			float PlanetRadius{ 6360.0e3f };                      /* m */
			float AtmosphereRadius{ 6420.0e3f };                  /* m */
		};

		/* 一帧的天空状态（每视图一份） */
		struct SkyState
		{
			float Hour{ 10.0f };
			glm::vec3 SunDirection{ 0.0f, 1.0f, 0.0f };   /* 指向太阳的单位向量 */
			glm::vec3 MoonDirection{ 0.0f, -1.0f, 0.0f }; /* 指向月亮 */
			glm::vec3 PoleDirection{ 0.0f, 0.0f, 1.0f };  /* 天球极轴，星空绕它转 */
			glm::vec3 SunColor{ 1.0f };                   /* 穿过大气后的阳光颜色（不含强度） */
			glm::vec3 MoonColor{ 0.72f, 0.80f, 1.0f };
			/* 月亮看到的太阳方向（月面着色用）：照亮比例严格等于 MoonPhase，
			 * 直接拿太阳方向当月面光源时，纬度一高满月也会被画成半月 */
			glm::vec3 MoonLightDirection{ 0.0f, 1.0f, 0.0f };
			float SunIntensity{ 1.0f };
			float MoonIntensity{ 0.0f };
			float StarIntensity{ 1.0f };
			float StarRotation{ 0.0f }; /* 星空绕极轴转过的角度（弧度） */
			float MoonPhase{ 0.5f };    /* 0 新月 / 0.5 满月 */
			float Exposure{ 1.5f };
			float Haze{ 1.0f }; /* 米氏散射强度缩放，越大越浑浊 */
			Atmosphere Atmos;
		};

		/* 太阳方向：hour 为当地真太阳时（0-24，12 点为正午），latitude / north_angle 单位为度，
		 * day_of_year 是一年中的第几天（决定赤纬：春分约 80、夏至约 172）。
		 * 返回世界空间单位向量：Y 朝上、+Z 朝北、+X 朝东（再按正北朝向绕 Y 旋转）。 */
		glm::vec3 ComputeSunDirection(float hour, float latitude_deg, int day_of_year, float north_angle_deg);

		/* 天球极轴：高度角等于纬度、方位朝北。星空与月亮都绕它转 */
		glm::vec3 ComputePoleDirection(float latitude_deg, float north_angle_deg);

		/* 阳光穿过整层大气后的透过率（从地面沿 sun_direction 积分），太阳在地平线下返回 0 */
		glm::vec3 ComputeSunTransmittance(const glm::vec3& sun_direction, const Atmosphere& atmosphere);

		/* 月亮方向：沿极轴相对太阳转过 phase 圈 —— 0 = 新月（贴着太阳）、0.5 = 满月（正对面） */
		glm::vec3 ComputeMoonDirection(const glm::vec3& sun_direction, float phase, const glm::vec3& pole_direction);

		/* 月亮被照亮的比例（0 新月 / 1 满月），直接由相位给 */
		float ComputeMoonIlluminatedFraction(float phase);

		/* 月球看到的太阳方向：把太阳方向投影到月亮的切平面再按相位转过去。
		 * 满月正好照满朝向地球的那一面，新月则是背光 —— 与 ComputeMoonIlluminatedFraction 一致 */
		glm::vec3 ComputeMoonLightDirection(const glm::vec3& moon_direction, const glm::vec3& sun_direction,
			float phase);

		/* 太阳辐照度相对光源强度的缩放：天空散射与方向光共用一个太阳，靠它换算到同一亮度量级 */
		inline constexpr float kSunIrradianceScale = 8.0f;

		/* 组件 → 一帧状态（纯函数，不改组件） */
		SkyState Resolve(const SkyComponent& sky);
	}
}
