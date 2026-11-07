#pragma once
#include "Helios/Events/Event.h"

namespace Helios
{
	class HELIOS_API ILayer
	{
	public:
		/* priority：层序大小，数字大的更靠上层 —— 后渲染、先收事件。
		 * 只在同一区里比（普通层 / 覆盖层），普通层永远垫在覆盖层下面。 */
		ILayer(const std::string& name = "DefaultLayer", int priority = 0);
		virtual ~ILayer() = default;

		const std::string& GetName() const { return m_Name; }
		[[nodiscard]] int GetPriority() const { return m_Priority; }

		virtual void OnAttached() {}
		virtual void OnDetached() {}
		virtual void OnUpdate(float delta_time) {}
		virtual void OnImGuiRender() {}
		virtual void OnEvent(IEvent* event) {}

	protected:
		std::string m_Name{};
		int m_Priority{ 0 };
	};

}