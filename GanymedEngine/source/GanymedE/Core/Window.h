#pragma once

#include <sstream>

#include "Core.h"
#include "GanymedE/events/Event.h"

namespace GanymedE {

	// Client-pixel rects reported by a custom title bar so the platform path can
	// hit-test without asking ImGui (which cannot run inside WM_NCHITTEST).
	struct WindowHitRect
	{
		float X = 0.0f;
		float Y = 0.0f;
		float W = 0.0f;
		float H = 0.0f;
	};

	static constexpr uint32_t kMaxTitleBarHitExclusions = 8;

	struct TitleBarHitTest
	{
		WindowHitRect Caption{};
		WindowHitRect Exclusions[kMaxTitleBarHitExclusions]{};
		uint32_t ExclusionCount = 0;

		void Assign(const WindowHitRect& caption, const WindowHitRect* exclusions, uint32_t count)
		{
			Caption = caption;
			ExclusionCount = count > kMaxTitleBarHitExclusions ? kMaxTitleBarHitExclusions : count;
			for (uint32_t i = 0; i < ExclusionCount; i++)
				Exclusions[i] = exclusions[i];
		}

		static bool Contains(const WindowHitRect& r, float x, float y)
		{
			return x >= r.X && y >= r.Y && x < r.X + r.W && y < r.Y + r.H;
		}

		bool InInteractive(float x, float y) const
		{
			for (uint32_t i = 0; i < ExclusionCount; i++)
			{
				if (Contains(Exclusions[i], x, y))
					return true;
			}
			return false;
		}

		bool DraggableAt(float x, float y) const
		{
			return Contains(Caption, x, y) && !InInteractive(x, y);
		}
	};

	struct WindowProps {
		std::string Title;
		uint32_t Width;
		uint32_t Height;

		// Borderless fullscreen: an undecorated window sized to the primary monitor's
		// video mode, not an exclusive-mode swapchain. Width/Height are ignored when
		// set. Implemented on Windows; Linux/macOS honour it best-effort.
		bool Fullscreen;

		// Undecorated host + ImGui-drawn title bar. Default false so Sandbox and
		// the runtime keep a normal OS frame. Realized by Window::HasCustomTitleBar;
		// Wayland refuses it (cannot move an undecorated window) and stays decorated.
		bool CustomTitleBar;

		WindowProps(const std::string& title = "GanymedEngine",
			uint32_t width = DEFAULT_WINDOW_WIDTH,
			uint32_t height = DEFAULT_WINDOW_HEIGHT,
			bool fullscreen = false,
			bool customTitleBar = false)
			: Title(title), Width(width), Height(height), Fullscreen(fullscreen),
			  CustomTitleBar(customTitleBar) {}
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

		// Custom title bar. HasCustomTitleBar is the *realized* flag: the spec
		// may request it and the platform still refuse (Wayland).
		virtual bool HasCustomTitleBar() const = 0;
		virtual bool IsMaximized() const = 0;
		virtual void Minimize() = 0;
		virtual void ToggleMaximize() = 0;
		virtual void SetTitleBarHitTest(const WindowHitRect& caption,
			const WindowHitRect* exclusions, uint32_t exclusionCount) = 0;

		static Window* Create(const WindowProps& props = WindowProps());
	};
}
