#pragma once

#include "Event.h"
#include "GanymedE/Core/MouseButtonCodes.h"

namespace GanymedE {
	class GE_API MouseMovedEvent : public Event {
	public:
		MouseMovedEvent(const float x, const float y) : m_MouseX(x), m_MouseY(y){}

		inline float GetX() const { return m_MouseX; }
		inline float GetY() const { return m_MouseY; }

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseMovedEvent: " << m_MouseX << ", " << m_MouseY;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseMoved)
		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		float m_MouseX, m_MouseY;
	};

	class GE_API MouseScrolledEvent : public Event {
	public:
		MouseScrolledEvent(const float xOffset, const float yOffset) : m_OffsetX(xOffset), m_OffsetY(yOffset) {}

		inline float GetXOffset() const { return m_OffsetX; }
		inline float GetYOffset() const { return m_OffsetY; }

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseScrolledEvent: " << m_OffsetX << ", " << m_OffsetY;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseScrolled)
		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		float m_OffsetX, m_OffsetY;
	};

	class GE_API MouseButtonEvent : public Event {
	public:
		MouseCode GetMouseButton() const { return m_Button; }

		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput | EventCategoryMouseButton)
	protected:
		MouseButtonEvent(const MouseCode button) : m_Button(button) {}

		int m_Button;
	};

	class GE_API MouseButtonPressedEvent : public MouseButtonEvent {
	public:
		MouseButtonPressedEvent(const MouseCode button) : MouseButtonEvent(button){}

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseButtonPressedEvent: " << m_Button;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseButtonPressed)
	};

	class GE_API MouseButtonReleasedEvent : public MouseButtonEvent {
	public:
		MouseButtonReleasedEvent(const MouseCode button) : MouseButtonEvent(button) {}

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseButtonReleasedEvent: " << m_Button;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseButtonReleased)
	};
}
