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

		/* 是否投影；打开时把阴影配置一并备好（实现见 Light.cpp） */
		void SetIsCastShadow(bool enable);
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

		/* 实体旋转变化时重推光向：方向 = 旋转作用下的 -Y 规范朝向（跟图标基准朝向、默认光向同源）。
		 * 只在旋转变化时覆盖，脚本直接 SetDirection 的显式设定会保留。 */
		void SetTransform(const glm::mat4& transform) override;

	private:
		/* 光源方向 */
		glm::vec3 m_Direciton{ 0.0f, -1.0f, 0.0f };
	};


	/* 点光与聚光的共享部分：两者都是有位置、有衰减范围（Range）的点状光源，
	 * 光照衰减与阴影远平面都从这一份范围数据出发（单一来源）。 */
	class PunctualLight : public Light
	{
	public:
		PunctualLight() = default;
		~PunctualLight() override = default;

		/* 光照范围：衰减到 0 的距离（世界单位），同时作为阴影投影的远平面 */
		void SetRange(float range) { m_Range = range; }
		[[nodiscard]] float GetRange() const { return m_Range; }

	protected:
		float m_Range{ 10.0f };
	};


	/* 点光 */
	class PointLight final : public PunctualLight
	{
	public:
		PointLight() = default;
		~PointLight() override = default;

		/* 获取光源类型 */
		[[nodiscard]] LightType GetLightType() const override { return LightType::Point; }
	};


	/* 聚光 */
	class SpotLight final : public PunctualLight
	{
	public:
		SpotLight() = default;
		~SpotLight() override = default;

		/* 获取光源类型 */
		[[nodiscard]] LightType GetLightType() const override { return LightType::Spot; }

		/* 外锥全角（度，与 Camera Fov 同单位习惯） */
		void SetAngle(float angle) { m_Angle = angle; }
		[[nodiscard]] float GetAngle() const { return m_Angle; }

		/* 内锥全角：硬编码为外锥的固定比例，锥缘有一段半影过渡 */
		[[nodiscard]] float GetInnerAngle() const { return m_Angle * kInnerAngleRatio; }

		/* 光传播方向：实体旋转下的 -Z 前向（与场景 gizmo 的锥体轴向同源） */
		[[nodiscard]] glm::vec3 GetDirection() const;

		static constexpr float kInnerAngleRatio = 0.8f;

	private:
		float m_Angle{ 30.0f };
	};

	/* 从 Light 取点光/聚光公共接口：非点状光源（方向光等）返回 nullptr */
	[[nodiscard]] inline PunctualLight* AsPunctualLight(Light* light) noexcept
	{
		if (light == nullptr)
			return nullptr;
		switch (light->GetLightType())
		{
		case LightType::Point: return static_cast<PointLight*>(light);
		case LightType::Spot:  return static_cast<SpotLight*>(light);
		default:               return nullptr;
		}
	}

	[[nodiscard]] inline const PunctualLight* AsPunctualLight(const Light* light) noexcept
	{
		return AsPunctualLight(const_cast<Light*>(light));
	}


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