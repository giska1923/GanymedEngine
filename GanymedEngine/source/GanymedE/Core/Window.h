#pragma once

#include <sstream>

#include "Core.h"
#include "GanymedE/events/Event.h"

namespace GanymedE {
	struct WindowProps {
		std::string Title;
		uint32_t Width;
		uint32_t Height;

		// Borderless fullscreen: an undecorated window sized to the primary monitor's
		// video mode, not an exclusive-mode swapchain. Width/Height are ignored when
		// set. Implemented on Windows; Linux/macOS honour it best-effort.
		bool Fullscreen;

		WindowProps(const std::string& title = "GanymedEngine",
			uint32_t width = DEFAULT_WINDOW_WIDTH,
			uint32_t height = DEFAULT_WINDOW_HEIGHT,
			bool fullscreen = false)
			: Title(title), Width(width), Height(height), Fullscreen(fullscreen) {}
	};

	// Interface representing a desktop system based Window

	class GE_API Window {
	public:
		using EventCallbackFn = std::function<void(Event&)>;

		virtual ~Window(){}

		virtual void OnUpdate() = 0;

		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;

		// Window attributes
		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		virtual void SetVSync(bool enabled) = 0;
		virtual bool IsVSync() const = 0;

		virtual void* GetNativeWindow() const = 0;

		static Window* Create(const WindowProps& props = WindowProps());
	};
}
