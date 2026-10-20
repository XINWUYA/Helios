#pragma once
#include "Helios/Events/Event.h"

namespace Helios
{
	class HELIOS_API ILayer
	{
	public:
		ILayer(const std::string& name = "DefaultLayer");
		virtual ~ILayer() = default;

		const std::string& GetName() const { return m_Name; }

		virtual void OnAttached() {}
		virtual void OnDetached() {}
		virtual void OnUpdate(float delta_time) {}
		virtual void OnImGuiRender() {}
		virtual void OnEvent(IEvent* event) {}

	protected:
		std::string m_Name{};
	};

}