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

		/* 日月视半径的基准值（弧度）：比真实值略大一点，低分辨率下盘面才有几像素。
		 * 这是 1 AU 与平均月地距离下的值，真实角径按距离换算 */
		inline constexpr float kSunAngularRadius = 0.008f;
		inline constexpr float kMoonAngularRadius = 0.012f;

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
			/* 日月视半径（弧度）：按到地球的距离换算（视直径与距离成反比），
			 * 再乘地平线放大 —— 贴近地平线时盘面看着更大，见 ComputeHorizonMagnification */
			float SunAngularRadius{ kSunAngularRadius };
			float MoonAngularRadius{ kMoonAngularRadius };
			/* 竖直方向的压扁因子（1 = 不压扁）：大气折射把天体抬高，盘的上下边缘
			 * 被抬得不一样多，贴地平线时盘面成了扁的椭圆 */
			float SunFlattening{ 1.0f };
			float MoonFlattening{ 1.0f };
			Atmosphere Atmos;
		};

		/* 太阳方向：hour 为当地真太阳时（0-24，12 点为正午），latitude / north_angle 单位为度，
		 * day_of_year 是一年中的第几天（决定赤纬：春分约 80、夏至约 172）。
		 * 返回世界空间单位向量：Y 朝上、+X 朝东、+Z 朝南（再按正北朝向绕 Y 旋转）。
		 * 东天顶南是右手系 —— 朝北（-Z）看时右手边正是东 */
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

		/* 太阳视半径：日地距离一年里按 ±1.67% 变化（近日点在 1 月初），视直径与距离成反比。
		 * 一天里几乎不动，变化落在季节尺度上 */
		float ComputeSunAngularRadius(int day_of_year);

		/* 月亮视半径：月地距离按 27.55 天的近点月周期变化 ±5.5%，
		 * 近地点时盘面比远地点大约一成（超级月亮那一档） */
		float ComputeMoonAngularRadius(int day_of_year);

		/* 大气折射对盘面的竖直压扁因子：抬升量 R(h) 随高度角变化，
		 * 盘的下边缘比上边缘抬得多，盘面就扁了。贴地平线约 0.86，十度以上基本是圆 */
		float ComputeRefractionFlattening(float elevation);

		/* 地平线附近的盘面放大因子（观感补偿，不是物理）。
		 * 日出日落时日月看着比高挂时大，是大脑拿地面景物当尺子量出来的错觉（月亮错觉），
		 * 真实角径反而因为折射与更远的地日距离略微更小。屏幕上的日月没有景深与地面参照，
		 * 照真实角径画反而显得日出日落时更小 —— 这里按高度角补回来，入参是高度角（弧度） */
		float ComputeHorizonMagnification(float elevation);

		/* 地平线放大的上限：1.3 表示贴地平线时盘面比天顶大 30%，到二十度左右回落到 1 */
		inline constexpr float kHorizonMagnification = 0.3f;

		/* 组件 → 一帧状态（纯函数，不改组件） */
		SkyState Resolve(const SkyComponent& sky);
	}
}
