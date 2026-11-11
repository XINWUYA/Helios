#pragma once
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>

namespace Helios
{
	namespace UI
	{
		/* 屏幕摇杆：拖拽手柄，把半径方向归一化的值留在自己身上，由 UISystem 每帧取走。
		 * 只管交互与手柄位置，不知道谁在用它 —— 相机怎么走归脚本。
		 * RML 写法：<joystick name="move"><knob/></joystick>，样式里给 drag: drag。 */
		class ElementJoystick final : public Rml::Element
		{
		public:
			explicit ElementJoystick(const Rml::String& tag);

			/* 归一化值：x 向右、y 向下为正（RmlUi 的屏幕坐标口径），长度不超过 1。
			 * 拖拽期间以指针为准，松手后回到键盘通道的值 */
			[[nodiscard]] Rml::Vector2f GetAxis() const { return m_Dragging ? m_Axis : m_KeyAxis; }
			/* 键盘通道：宿主每帧按方向键写进来，不用模拟鼠标拖拽 */
			void SetKeyAxis(const Rml::Vector2f& axis);
			/* 控件名（name 属性）：脚本按这个名字取值 */
			[[nodiscard]] const Rml::String& GetControlName() const { return m_ControlName; }

			void ProcessDefaultAction(Rml::Event& event) override;
			void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;

		private:
			/* dragend 的默认动作阶段是 None，只能靠监听器收 */
			class DragEndListener final : public Rml::EventListener
			{
			public:
				explicit DragEndListener(ElementJoystick& owner) : m_Owner(owner) {}
				void ProcessEvent(Rml::Event& /*event*/) override { m_Owner.ResetAxis(); }

			private:
				ElementJoystick& m_Owner;
			};

			void BeginDrag();
			void UpdateAxis(float mouse_x, float mouse_y);
			void ResetAxis();
			void ApplyKnobOffset();
			/* 手柄最大行程（像素），按当前布局算 */
			[[nodiscard]] float TravelFromLayout();
			[[nodiscard]] Rml::Element* FindKnob();

			Rml::String m_ControlName{ "joystick" };
			/* 指针拖出来的值与拖拽状态 */
			Rml::Vector2f m_Axis{ 0.0f, 0.0f };
			bool m_Dragging{ false };
			/* 键盘推出来的值：拖拽结束后用它，两者不互相覆盖 */
			Rml::Vector2f m_KeyAxis{ 0.0f, 0.0f };
			/* 拖拽起点：圆心与最大行程（像素），dragstart 时按当前布局算一次 */
			Rml::Vector2f m_Center{ 0.0f, 0.0f };
			float m_Travel{ 1.0f };
			DragEndListener m_DragEndListener{ *this };
		};
	}
}
