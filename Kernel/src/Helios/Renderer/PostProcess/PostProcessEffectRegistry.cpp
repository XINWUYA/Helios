#include "Pch.h"
#include "PostProcessEffectRegistry.h"

namespace Helios
{
	namespace PostProcess
	{
		PostProcessEffectRegistry& PostProcessEffectRegistry::Instance()
		{
			/* 函数内静态：首次使用时构造，规避跨 TU 的静态初始化顺序问题
			 * （内置清单的注册对象会在静态初始化期调用本函数）。 */
			static PostProcessEffectRegistry s_Instance;
			return s_Instance;
		}

		void PostProcessEffectRegistry::Add(EffectDesc desc)
		{
			/* 有序插入：先 Phase、同 Phase 保持注册序（插到同相位段尾） */
			auto pos = m_Effects.begin();
			while (pos != m_Effects.end() && pos->Stage <= desc.Stage)
				++pos;
			m_Effects.insert(pos, std::move(desc));
		}
	}
}
