#pragma once
#include "Helios/Scene/Components.h"
#include "Helios/Renderer/PostProcess/PostProcessEffectRegistry.h"

namespace Helios
{
	/* TAA 渲染侧设置：解析后进视图快照（渲染缺省 = 关闭 —— 场景里没有后处理对象即此值） */
	struct TaaSettings
	{
		bool  Enabled{ false };
		/* 锐化补偿：时域累积会把边缘磨软，补一点回来（0 = 关闭） */
		float Sharpness{ 0.35f };
		/* 静止时历史帧的占比：0.9 = 当前帧吃一成，约十帧收敛 */
		float HistoryWeight{ 0.9f };
	};

	/* TAA 场景侧组件：挂在「后处理对象」实体上（与 BloomComponent 同构）。
	 * 字段扁平存储（注册表偏移字段的硬要求），解析时经 GetTaaSettings 组装成快照值。 */
	struct TaaComponent : ComponentBase
	{
		bool  m_Enabled{ true };
		float m_Sharpness{ 0.35f };
		float m_HistoryWeight{ 0.9f };

		[[nodiscard]] TaaSettings GetTaaSettings() const
		{
			return TaaSettings{ m_Enabled, m_Sharpness, m_HistoryWeight };
		}
	};

	namespace PostProcess
	{
		/* 效果自含登记（由内置清单调用） */
		void RegisterTaaEffect();
	}
}
