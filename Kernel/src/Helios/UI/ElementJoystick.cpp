#include "Pch.h"
#include "ElementJoystick.h"

#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Transform.h>
#include <RmlUi/Core/TransformPrimitive.h>

namespace Helios
{
	namespace UI
	{
		namespace
		{
			/* 手柄能走的最大行程占比：留一圈边，拖到最边上手柄也还在底盘里 */
			constexpr float kTravelRatio = 0.34f;
		}

		ElementJoystick::ElementJoystick(const Rml::String& tag)
			: Rml::Element(tag)
		{
			AddEventListener(Rml::EventId::Dragend, &m_DragEndListener);
		}

		void ElementJoystick::OnAttributeChange(const Rml::ElementAttributes& changed_attributes)
		{
			Rml::Element::OnAttributeChange(changed_attributes);

			if (changed_attributes.find("name") != changed_attributes.end())
				m_ControlName = GetAttribute<Rml::String>("name", "joystick");
		}

		void ElementJoystick::ProcessDefaultAction(Rml::Event& event)
		{
			switch (event.GetId())
			{
			case Rml::EventId::Dragstart:
				BeginDrag();
				break;
			case Rml::EventId::Drag:
				UpdateAxis(event.GetParameter<float>("mouse_x", 0.0f), event.GetParameter<float>("mouse_y", 0.0f));
				break;
			default:
				Rml::Element::ProcessDefaultAction(event);
				break;
			}
		}

		void ElementJoystick::SetKeyAxis(const Rml::Vector2f& axis)
		{
			if (m_KeyAxis == axis)
				return;

			m_KeyAxis = axis;
			/* 拖拽期间手柄归指针，键盘只改值不动画面 */
			if (!m_Dragging)
				ApplyKnobOffset();
		}

		void ElementJoystick::BeginDrag()
		{
			/* 圆心与行程按拖拽开始那一刻的布局算：拖拽途中不会重排 */
			const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Border);
			m_Center = GetAbsoluteOffset(Rml::BoxArea::Border) + size * 0.5f;
			m_Travel = std::max(std::min(size.x, size.y) * kTravelRatio, 1.0f);
			m_Dragging = true;
		}

		void ElementJoystick::UpdateAxis(float mouse_x, float mouse_y)
		{
			Rml::Vector2f delta(mouse_x - m_Center.x, mouse_y - m_Center.y);
			const float length = delta.Magnitude();
			if (length > m_Travel)
				delta = delta * (m_Travel / length);

			m_Axis = delta / m_Travel;
			ApplyKnobOffset();
		}

		void ElementJoystick::ResetAxis()
		{
			/* 松手：指针那份归零，手柄如果还按着方向键就回到键盘的值 */
			m_Axis = Rml::Vector2f(0.0f, 0.0f);
			m_Dragging = false;
			ApplyKnobOffset();
		}

		void ElementJoystick::ApplyKnobOffset()
		{
			Rml::Element* knob = FindKnob();
			if (knob == nullptr)
				return;

			const Rml::Vector2f axis = GetAxis();
			const float travel = m_Dragging ? m_Travel : TravelFromLayout();

			/* 只改变换：不进布局，不重编译几何（见设计文档 D13） */
			const Rml::Property property = Rml::Transform::MakeProperty(
				{ Rml::Transforms::Translate2D{ axis.x * travel, axis.y * travel } });
			knob->SetProperty(Rml::PropertyId::Transform, property);
		}

		float ElementJoystick::TravelFromLayout()
		{
			const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Border);
			return std::max(std::min(size.x, size.y) * kTravelRatio, 1.0f);
		}

		Rml::Element* ElementJoystick::FindKnob()
		{
			Rml::ElementList knobs;
			GetElementsByTagName(knobs, "knob");
			return knobs.empty() ? nullptr : knobs.front();
		}
	}
}
