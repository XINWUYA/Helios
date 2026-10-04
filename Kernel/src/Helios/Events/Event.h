#pragma once
#include <functional>
#include <sstream>
#include "Helios/Common/Common.h"
#include "Helios/Core/Profiler.h"

namespace Helios
{
	enum class EventType
	{
		None = 0,
		WindowClose, WindowResize, WindowFocus, WindowLostFocus, WindowMoved, WindowIconify,  // Windows Related
		AppTick, AppUpdate, AppRender,  // Application Related
		KeyPressed, KeyReleased, KeyTyped,  // Keyboard Related
		MouseButtonPressed, MouseButtonReleased, MouseMoved, MouseScrolled  // Mouse Related
	};

	enum EventCategory
	{
		None = 0,
		EventCategoryApplication = BIT(0),
		EventCategoryInput = BIT(1),
		EventCategoryKeyboard = BIT(2),
		EventCategoryMouse = BIT(3),
		EventCategoryMouseButton = BIT(4)
	};

#define EVENT_CLASS_TYPE(type)													\
	static EventType GetStaticType() { return EventType::type; }				\
	virtual EventType GetEventType() const override { return GetStaticType(); }	\
	virtual const char* GetName() const override { return STRINGIFY(type); }

	/*
	* 事件基类
	*/
	class IEvent
	{
	public:
		virtual ~IEvent() = default;

		bool Handled = false; // 标记事件是否应该停止，对于多层Layer，更上层的事件优先触发，若事件到达某一层后不再向下传递，才应返回true

		virtual EventType GetEventType() const = 0;
		virtual const char* GetName() const = 0;
		virtual int GetCategoryFlags() const = 0;
		virtual std::string ToString() const { return GetName(); }

		bool IsInCategory(EventCategory category)
		{
			return GetCategoryFlags() & category;
		}
	};

	/*
	* 事件分配器类
	*/
	class EventDispatcher
	{
	public:
		EventDispatcher(IEvent* event)
			: m_pEvent(event)
		{
		}

		// F will be deduced by the compiler
		template<typename T, typename F>
		bool Dispatch(const F& func)
		{
			if (m_pEvent->GetEventType() == T::GetStaticType())
			{
				m_pEvent->Handled |= func(static_cast<T*>(m_pEvent));
				return true;
			}
			return false;
		}
	private:
		IEvent* m_pEvent;
	};

	inline std::ostream& operator<<(std::ostream& os, const IEvent& e)
	{
		return os << e.ToString();
	}
}