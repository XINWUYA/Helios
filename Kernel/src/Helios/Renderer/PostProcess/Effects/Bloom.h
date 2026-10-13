#pragma once
#include "Helios/Scene/Components.h"
#include "Helios/Renderer/PostProcess/PostProcessEffectRegistry.h"

namespace Helios
{
	/* Bloom 渲染侧设置：解析后进视图快照（渲染缺省 = 关闭 —— 场景里没有后处理对象即此值） */
	struct BloomSettings
	{
		bool  Enabled{ false };
		float Threshold{ 0.8f };  /* 线性亮度阈值（max(r,g,b) 判定） */
		float Intensity{ 0.6f };  /* 合成强度 */
		float Scatter{ 0.7f };    /* 升采样贡献系数（越大光晕越宽亮） */
	};

	/* Bloom 场景侧组件：挂在「后处理对象」（PostProcessComponent）实体上。
	 * 字段扁平存储（注册表偏移字段的硬要求），解析时经 GetBloomSettings 组装成快照值。 */
	struct BloomComponent : ComponentBase
	{
		bool  m_Enabled{ true };  /* 新建组件即可见效果；取消勾选 = 关闭（组件保留） */
		float m_Threshold{ 0.8f };
		float m_Intensity{ 0.6f };
		float m_Scatter{ 0.7f };

		[[nodiscard]] BloomSettings GetBloomSettings() const
		{
			return BloomSettings{ m_Enabled, m_Threshold, m_Intensity, m_Scatter };
		}
	};

	namespace PostProcess
	{
		/* 效果自含登记（由内置清单调用）：只做效果注册表登记；
		 * 组件注册按既有惯例在 ComponentRegistry.cpp 补一段（"新增组件补一段"）。 */
		void RegisterBloomEffect();
	}
}
