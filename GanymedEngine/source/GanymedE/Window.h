#pragma once

#include "gepch.h"

#include "Core.h"
#include "events/Event.h"

namespace GanymedE {
	struct WindowProps {
		std::string Title;
		unsigned int Width;
		unsigned int Height;

		WindowProps(const std::string& title = "GanymedEngine",
			unsigned int width = DEFAULT_WINDOW_WIDTH,
			unsigned int height = DEFAULT_WINDOW_HEIGHT) : Title(title), Width(width), Height(height) {}
	};

	// Interface representing a desktop system based Window

	class GE_API Window {
	public:
		using EventCallbackFn = std::function<void(Event&)>;

		virtual ~Window(){}

		virtual void OnUpdate() = 0;

		virtual unsigned int GetWidth() const = 0;
		virtual unsigned int GetHeight() const = 0;

		// Window attributes
		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		virtual void SetVSync(bool enabled) = 0;
		virtual bool IsVSync() const = 0;

		static Window* Create(const WindowProps& props = WindowProps());
	};
}