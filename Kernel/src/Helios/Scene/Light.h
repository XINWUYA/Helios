#pragma once
#include "SceneObject.h"

namespace Helios
{
	/* 光源类型 */
	enum class LightType : uint8_t
	{
		Directional,
		Point,
		Spot,
		Area,
		Volume
	};

	/* 阴影信息 */
	struct ShadowMapInfo
	{
		uint32_t Size{ 1024 };
		uint8_t CascadeCnt{ 1 };
		/* 各级联正交视锥的"最小半边长"下限（世界单位）。只做大、不缩小按几何体 tight-fit 出来的真实
		 * 范围，不会把投射物挤出阴影图；填 0 = 完全 tight-fit。注意：它不是最大范围 / 钳制值 ——
		 * 把大范围钳到 radius 会让物体投到 [-1,1] 外、写不进阴影图。 */
		glm::vec4 CascadeRadius{ 0.0f };
		float ConstantBias{ 0.01f };
		float NormalBias{ 1.0f };
		float ShadowFar{ 0.0f };
	};

	/* 光源类 */
	class Light : public SceneObject
	{
	public:
		Light() = default;
		virtual ~Light() = default;

		/* 获取光源类型 */
		[[nodiscard]] virtual LightType GetLightType() const = 0;

		/* 光源颜色 */
		void SetColor(const glm::vec4& color) { m_Color = color; }
		[[nodiscard]] const glm::vec4& GetColor() const { return m_Color; }

		/* 光源强度 */
		void SetIntensity(float intensity) { m_Intensity = intensity; }
		[[nodiscard]] float GetIntensity() const { return m_Intensity; }

		/* 是否投影 */
		void SetIsCastShadow(bool enable) { m_IsCastShadow = enable; }
		[[nodiscard]] bool IsCastShadow() const { return m_IsCastShadow; }

		/* 阴影信息 */
		[[nodiscard]] const SharedPtr<ShadowMapInfo>& GetShadowMapInfo() const { return m_pShadowMapInfo; }
		void SetShadowMapInfo(const SharedPtr<ShadowMapInfo>& shadow_map_info) { m_pShadowMapInfo = shadow_map_info; }

		/* 创建指定类型光源 */
		static SharedPtr<Light> Create(LightType type);

	protected:
		/* 光源颜色 */
		glm::vec4 m_Color{ 1.0f };
		/* 光源强度 */
		float m_Intensity{ 1.0f };
		/* 是否投影 */
		bool m_IsCastShadow{ false };
		/* 阴影信息 */
		SharedPtr<ShadowMapInfo> m_pShadowMapInfo{ nullptr };
	};


	/* 方向光 */
	class DirectionalLight final : public Light
	{
	public:
		DirectionalLight() = default;
		~DirectionalLight() override = default;

		/* 获取光源类型 */
		[[nodiscard]] LightType GetLightType() const override { return LightType::Directional; }

		/* 光源方向 */
		[[nodiscard]] const glm::vec3& GetDirection() const { return m_Direciton; }
		void SetDirection(const glm::vec3& dir) { m_Direciton = dir; }

	private:
		/* 光源方向 */
		glm::vec3 m_Direciton{ 0.0f, -1.0f, 0.0f };
	};


	/* 点光 */
	class PointLight final : public Light
	{
	public:
		PointLight() = default;
		~PointLight() override = default;

		/* 获取光源类型 */
		[[nodiscard]] LightType GetLightType() const override { return LightType::Point; }

	private:
		/* 光源位置 */
		glm::vec3 m_LightPos{ 0.0f };
	};


	/* 聚光 */
	class SpotLight final : public Light
	{
	public:
		SpotLight() = default;
		~SpotLight() override = default;

		/* 获取光源类型 */
		[[nodiscard]] LightType GetLightType() const override { return LightType::Spot; }
	};


	/* 面光 */
	class AreaLight final : public Light
	{
	public:
		AreaLight() = default;
		~AreaLight() override = default;

		/* 获取光源类型 */
		[[nodiscard]] LightType GetLightType() const override { return LightType::Area; }
	};


	/* 体积光 */
	class VolumeLight final : public Light
	{
	public:
		VolumeLight() = default;
		~VolumeLight() override = default;

		/* 获取光源类型 */
		[[nodiscard]] LightType GetLightType() const override { return LightType::Volume; }
	};

}