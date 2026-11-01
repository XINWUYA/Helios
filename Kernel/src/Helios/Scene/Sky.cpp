#include "Pch.h"
#include "Sky.h"
#include "Components.h"
#include <algorithm>
#include <cmath>

namespace Helios
{
	namespace Sky
	{
		namespace
		{
			constexpr float kPi = 3.14159265358979323846f;
			constexpr float kTwoPi = kPi * 2.0f;
			/* 一天转一圈：星空与太阳都按这个角速度走 */
			constexpr float kHoursPerDay = 24.0f;

			/* 阳光透过率的积分段数：只在 CPU 上跑，一次一帧，取足够密
			 * （地平线附近的光学厚度是垂直方向的几十倍，段数少了会把日出日落算亮） */
			constexpr int kTransmittanceSamples = 128;

			/* 日地距离：偏心率 0.0167，近日点在 1 月 3 日前后 */
			constexpr float kEarthOrbitEccentricity = 0.0167f;
			constexpr float kDaysPerYear = 365.24f;
			/* 月地距离：偏心率 0.0549，近点月 27.55 天。近地点落在哪天无所谓，
			 * 取第 8 天当参考点，只决定大小变化的相位 */
			constexpr float kMoonOrbitEccentricity = 0.0549f;
			constexpr float kAnomalisticMonth = 27.55f;
			constexpr float kPerigeeDay = 8.0f;

			/* Bennett 公式：大气折射把天体抬高的量（角分），入参是视高度角（度）。
			 * 地平线上约 0.48 度，10 度时只剩 0.09 度 */
			float RefractionLift(float elevation_deg)
			{
				const float shifted = elevation_deg + 10.3f / (elevation_deg + 5.11f);
				return 1.02f / std::tan(glm::radians(shifted));
			}

			/* 罗德里格旋转：绕任意轴转一个角度 */
			glm::vec3 RotateAroundAxis(const glm::vec3& v, const glm::vec3& axis, float angle)
			{
				const float c = std::cos(angle);
				const float s = std::sin(angle);
				return v * c + glm::cross(axis, v) * s + axis * glm::dot(axis, v) * (1.0f - c);
			}

			/* 射线与球的远交点：起点在球内时返回正根，不相交返回 -1 */
			float RaySphereFar(const glm::vec3& origin, const glm::vec3& dir, float radius)
			{
				const float b = glm::dot(origin, dir);
				const float c = glm::dot(origin, origin) - radius * radius;
				const float discriminant = b * b - c;
				if (discriminant < 0.0f)
					return -1.0f;
				return -b + std::sqrt(discriminant);
			}

			/* 时刻归一化到 [0, 24) */
			float WrapHour(float hour)
			{
				hour = std::fmod(hour, kHoursPerDay);
				if (hour < 0.0f)
					hour += kHoursPerDay;
				return hour;
			}
		}

		glm::vec3 ComputeSunDirection(float hour, float latitude_deg, int day_of_year, float north_angle_deg)
		{
			const float latitude = glm::radians(latitude_deg);
			/* 赤纬：一年内在 ±23.44° 之间摆动，冬至（约 355 天）最负 */
			const float declination = glm::radians(-23.44f)
				* std::cos(kTwoPi * (static_cast<float>(day_of_year) + 10.0f) / 365.0f);
			/* 时角：正午为 0，每小时 15°，下午为正（偏西） */
			const float hour_angle = glm::radians((WrapHour(hour) - 12.0f) * 15.0f);

			/* 地平坐标系：x 朝东、y 天顶、z 朝南。东天顶南才是右手系，
			 * 朝北（-Z）看时右手边正是东；写成 z 朝北会把整片天镜像过去 */
			glm::vec3 local;
			local.x = -std::cos(declination) * std::sin(hour_angle);
			local.y = std::sin(latitude) * std::sin(declination)
				+ std::cos(latitude) * std::cos(declination) * std::cos(hour_angle);
			local.z = std::cos(declination) * std::sin(latitude) * std::cos(hour_angle)
				- std::sin(declination) * std::cos(latitude);

			return glm::normalize(RotateAroundAxis(local, glm::vec3(0.0f, 1.0f, 0.0f),
				glm::radians(north_angle_deg)));
		}

		glm::vec3 ComputePoleDirection(float latitude_deg, float north_angle_deg)
		{
			const float latitude = glm::radians(latitude_deg);
			/* 与太阳同一个地平坐标系：极轴朝北天顶方向，即 -Z 一侧抬起来 */
			const glm::vec3 local(0.0f, std::sin(latitude), -std::cos(latitude));
			return glm::normalize(RotateAroundAxis(local, glm::vec3(0.0f, 1.0f, 0.0f),
				glm::radians(north_angle_deg)));
		}

		glm::vec3 ComputeSunTransmittance(const glm::vec3& sun_direction, const Atmosphere& atmosphere)
		{
			if (sun_direction.y <= 0.0f)
				return glm::vec3(0.0f);

			/* 观察点按海平面处理：海拔差在几百米以内，对颜色影响很小 */
			const glm::vec3 origin(0.0f, atmosphere.PlanetRadius, 0.0f);
			const float distance = RaySphereFar(origin, sun_direction, atmosphere.AtmosphereRadius);
			if (distance <= 0.0f)
				return glm::vec3(0.0f);

			const float step = distance / static_cast<float>(kTransmittanceSamples);
			float depth_rayleigh = 0.0f;
			float depth_mie = 0.0f;
			for (int i = 0; i < kTransmittanceSamples; ++i)
			{
				const glm::vec3 position = origin + sun_direction * (step * (static_cast<float>(i) + 0.5f));
				const float height = std::max(glm::length(position) - atmosphere.PlanetRadius, 0.0f);
				depth_rayleigh += std::exp(-height / atmosphere.RayleighScaleHeight) * step;
				depth_mie += std::exp(-height / atmosphere.MieScaleHeight) * step;
			}

			const glm::vec3 tau = atmosphere.RayleighBeta * depth_rayleigh
				+ glm::vec3(atmosphere.MieBeta * depth_mie);
			return glm::exp(-tau);
		}

		glm::vec3 ComputeMoonDirection(const glm::vec3& sun_direction, float phase, const glm::vec3& pole_direction)
		{
			const float angle = WrapHour(phase) * kTwoPi;
			return glm::normalize(RotateAroundAxis(sun_direction, pole_direction, angle));
		}

		float ComputeMoonIlluminatedFraction(float phase)
		{
			/* 新月照不到朝向地球的那面，满月正好照满 */
			return std::clamp((1.0f - std::cos(WrapHour(phase) * kTwoPi)) * 0.5f, 0.0f, 1.0f);
		}

		float ComputeSunAngularRadius(int day_of_year)
		{
			/* 视直径与距离成反比：近日点（1 月初）大 3.4%，远日点小 3.4% */
			const float anomaly = kTwoPi * (static_cast<float>(day_of_year) - 3.0f) / kDaysPerYear;
			const float distance = 1.0f - kEarthOrbitEccentricity * std::cos(anomaly);
			return kSunAngularRadius / distance;
		}

		float ComputeMoonAngularRadius(int day_of_year)
		{
			/* 近地点盘面最大、远地点最小，比值 (1+e)/(1-e) 约 1.12 —— 超级月亮那一档 */
			const float anomaly = kTwoPi * (static_cast<float>(day_of_year) - kPerigeeDay)
				/ kAnomalisticMonth;
			const float distance = 1.0f - kMoonOrbitEccentricity * std::cos(anomaly);
			return kMoonAngularRadius / distance;
		}

		float ComputeRefractionFlattening(float elevation)
		{
			/* 抬升量对高度角的导数就是竖直方向的放大率：取当前高度角处的差分。
			 * 抬升量单位是角分，除以 60 换到度，步长也按度算 */
			constexpr float kStepDegrees = 0.25f;
			const float height = glm::degrees(std::max(elevation, 0.0f));
			const float lift = RefractionLift(height);
			const float lift_above = RefractionLift(height + kStepDegrees);
			const float derivative = (lift_above - lift) / 60.0f / kStepDegrees;

			/* 下限 0.55：折射公式在贴地平线处发散，不能让它把盘压成一条线 */
			return std::clamp(1.0f + derivative, 0.55f, 1.0f);
		}

		float ComputeHorizonMagnification(float elevation)
		{
			/* 二十度以上不再放大。落到地平线以下的盘看不见，保持满额即可，
			 * 免得半个日面被地平线切掉时尺寸往回缩 */
			const float height = glm::degrees(std::max(elevation, 0.0f));
			return 1.0f + kHorizonMagnification * (1.0f - glm::smoothstep(0.0f, 20.0f, height));
		}

		glm::vec3 ComputeMoonLightDirection(const glm::vec3& moon_direction, const glm::vec3& sun_direction,
			float phase)
		{
			const glm::vec3 moon = glm::normalize(moon_direction);

			/* 太阳在月亮切平面上的分量：决定明暗界线的朝向（朝阳的那半边亮） */
			glm::vec3 tangent = sun_direction - moon * glm::dot(sun_direction, moon);
			if (glm::length(tangent) < 1.0e-4f)
			{
				const glm::vec3 reference = std::abs(moon.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f)
					: glm::vec3(1.0f, 0.0f, 0.0f);
				tangent = glm::cross(reference, moon);
			}
			tangent = glm::normalize(tangent);

			/* 相位 0 时太阳在月亮背后（新月）、0.5 时正对地球这一面（满月）。
			 * 注意方向约定：这里给的是"从月亮指向太阳"的方向 */
			const float angle = WrapHour(phase) * kTwoPi;
			return glm::normalize(moon * std::cos(angle) + tangent * std::sin(angle));
		}

		SkyState Resolve(const SkyComponent& sky)
		{
			SkyState state;
			state.Hour = WrapHour(sky.m_Hour);
			state.Atmos = Atmosphere{};
			state.MoonPhase = std::clamp(sky.m_MoonPhase, 0.0f, 1.0f);
			state.Haze = std::max(sky.m_Haze, 0.0f);
			state.Exposure = std::max(sky.m_Exposure, 0.0f);
			state.StarIntensity = std::max(sky.m_StarIntensity, 0.0f);

			state.PoleDirection = ComputePoleDirection(sky.m_Latitude, sky.m_NorthAngle);
			state.SunDirection = ComputeSunDirection(sky.m_Hour, sky.m_Latitude, sky.m_DayOfYear,
				sky.m_NorthAngle);
			state.MoonDirection = ComputeMoonDirection(state.SunDirection, state.MoonPhase,
				state.PoleDirection);

			/* 阳光颜色就是大气透过率：日出日落时蓝光被吃掉，自然偏红 */
			state.SunColor = ComputeSunTransmittance(state.SunDirection, state.Atmos);
			state.SunIntensity = std::max(sky.m_SunIntensity, 0.0f);

			/* 月亮本身不发光：亮度按被照亮的比例给，落下地平线就没有月光 */
			const float illuminated = ComputeMoonIlluminatedFraction(state.MoonPhase);
			const float above_horizon = glm::smoothstep(-0.02f, 0.10f, state.MoonDirection.y);
			state.MoonIntensity = std::max(sky.m_MoonIntensity, 0.0f) * illuminated * above_horizon;
			state.MoonLightDirection = ComputeMoonLightDirection(state.MoonDirection, state.SunDirection,
				state.MoonPhase);

			/* 星空按恒星时转：一天一圈，方向与太阳一致（东升西落） */
			state.StarRotation = state.Hour / kHoursPerDay * kTwoPi;

			/* 视半径按距离换算；压扁与放大都按盘心高度角算 */
			const float sun_elevation = std::asin(std::clamp(state.SunDirection.y, -1.0f, 1.0f));
			const float moon_elevation = std::asin(std::clamp(state.MoonDirection.y, -1.0f, 1.0f));
			state.SunAngularRadius = ComputeSunAngularRadius(sky.m_DayOfYear)
				* ComputeHorizonMagnification(sun_elevation);
			state.MoonAngularRadius = ComputeMoonAngularRadius(sky.m_DayOfYear)
				* ComputeHorizonMagnification(moon_elevation);
			state.SunFlattening = ComputeRefractionFlattening(sun_elevation);
			state.MoonFlattening = ComputeRefractionFlattening(moon_elevation);
			return state;
		}
	}
}
