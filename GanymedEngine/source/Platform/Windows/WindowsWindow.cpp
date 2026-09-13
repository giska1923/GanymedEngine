#include "gepch.h"
#include "Platform/Windows/WindowsWindow.h"

#include "GanymedE/events/ApplicationEvent.h"
#include "GanymedE/events/MouseEvent.h"
#include "GanymedE/events/KeyEvent.h"

#include "Platform/Bgfx/BgfxContext.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <dwmapi.h>
#include <windowsx.h>

#ifdef IsMaximized
#undef IsMaximized
#endif
#ifdef IsMinimized
#undef IsMinimized
#endif

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif

namespace GanymedE {

	static constexpr wchar_t kWindowProp[] = L"Ganymed.Window";
	static constexpr int kResizeBorderDip = 6;

	static int ResizeBorderPx(HWND hwnd)
	{
		return MulDiv(kResizeBorderDip, (int)GetDpiForWindow(hwnd), 96);
	}

	static LRESULT CustomChromeHitTest(HWND hwnd, const TitleBarHitTest& hit, LPARAM lParam)
	{
		POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		POINT client = screen;
		ScreenToClient(hwnd, &client);

		const float x = (float)client.x;
		const float y = (float)client.y;

		// Caption buttons win over the resize border so the top 6 px of Close is
		// still a click, not HTTOP.
		if (hit.InInteractive(x, y))
			return HTCLIENT;

		if (!IsZoomed(hwnd))
		{
			RECT wr;
			GetWindowRect(hwnd, &wr);
			const int border = ResizeBorderPx(hwnd);
			const bool top = screen.y < wr.top + border;
			const bool bottom = screen.y >= wr.bottom - border;
			const bool left = screen.x < wr.left + border;
			const bool right = screen.x >= wr.right - border;
			if (top && left) return HTTOPLEFT;
			if (top && right) return HTTOPRIGHT;
			if (bottom && left) return HTBOTTOMLEFT;
			if (bottom && right) return HTBOTTOMRIGHT;
			if (top) return HTTOP;
			if (bottom) return HTBOTTOM;
			if (left) return HTLEFT;
			if (right) return HTRIGHT;
		}

		if (hit.DraggableAt(x, y))
			return HTCAPTION;

		return HTCLIENT;
	}

	static bool s_GLFWInitialized = false;

	static void glfwErrorCallback(int error, const char* desc){
		GE_CORE_ERROR("GLFW Error ({0}): {1}", error, desc);
	}

#ifdef GE_PLATFORM_WINDOWS
	Window* Window::Create(const WindowProps& props)
	{
		return new WindowsWindow(props);
	}
#endif

	WindowsWindow::WindowsWindow(const WindowProps& props)
	{
		GE_PROFILE_FUNCTION();

		Init(props);
	}

	WindowsWindow::~WindowsWindow()
	{
		GE_PROFILE_FUNCTION();

		Shutdown();
	}

	void WindowsWindow::Init(const WindowProps& props)
	{
		GE_PROFILE_FUNCTION();

		m_Data.Title = props.Title;
		m_Data.Width = props.Width;
		m_Data.Height = props.Height;

		GE_CORE_INFO("Creating window {0} ({1}, {2})", props.Title, props.Width, props.Height);

		if (!s_GLFWInitialized)
		{
			// TODO: glfwTerminate on system shutdown
			int success = glfwInit();
			GE_CORE_ASSERT(success, "Could not initialize GLFW!");
			glfwSetErrorCallback(glfwErrorCallback);
			s_GLFWInitialized = true;
		}

		{
			GE_PROFILE_SCOPE("glfwCreateWindow");

			// bgfx creates and owns the graphics device itself (including the GL
			// context when the GL backend is picked), so GLFW must not make one.
			glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

			// Borderless, not exclusive: an undecorated window covering the primary
			// monitor. Passing the monitor to glfwCreateWindow would request an
			// exclusive mode change, which bgfx does not drive and which costs
			// alt-tab friendliness for nothing at this scale.
			int monitorX = 0, monitorY = 0;
			if (props.Fullscreen)
			{
				GLFWmonitor* monitor = glfwGetPrimaryMonitor();
				const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
				if (mode)
				{
					glfwGetMonitorPos(monitor, &monitorX, &monitorY);
					m_Data.Width = (uint32_t)mode->width;
					m_Data.Height = (uint32_t)mode->height;
					glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
					GE_CORE_INFO("Borderless fullscreen: {0}x{1} at ({2}, {3})",
						m_Data.Width, m_Data.Height, monitorX, monitorY);
				}
				else
				{
					GE_CORE_WARN("Fullscreen requested but no video mode is available; staying windowed");
				}
			}
			else if (props.CustomTitleBar)
			{
				glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
				m_CustomTitleBar = true;
			}

			m_Window = glfwCreateWindow((int)m_Data.Width, (int)m_Data.Height, m_Data.Title.c_str(), nullptr, nullptr);

			if (props.Fullscreen && m_Window)
				glfwSetWindowPos(m_Window, monitorX, monitorY);
		}

		m_Context = CreateScope<BgfxContext>(m_Window);
		m_Context->Init(m_Data.Width, m_Data.Height);

		glfwSetWindowUserPointer(m_Window, &m_Data);

		if (m_CustomTitleBar)
			InstallCustomChrome();

		// Read back rather than forcing: Init() already applied the default.
		m_Data.VSync = m_Context->IsVSync();

		// setting callbacks
		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int width, int height) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			data.Width = width;
			data.Height = height;

			WindowResizeEvent event(width, height);
			data.EventCallback(event);
		});

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			WindowCloseEvent event;
			data.EventCallback(event);
		});

		glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int scancode, int action, int mods){
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			switch (action)
			{
				case GLFW_PRESS:
				{
					KeyPressedEvent event(key, 0);
					data.EventCallback(event);
					break;
				}
				case GLFW_RELEASE:
				{
					KeyReleasedEvent event(key);
					data.EventCallback(event);
					break;
				}
				case GLFW_REPEAT:
				{
					KeyPressedEvent event(key, 1);
					data.EventCallback(event);
					break;
				}
			}
		});

		glfwSetCharCallback(m_Window, [](GLFWwindow* window, unsigned int keycode) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			KeyTypedEvent event(keycode);
			data.EventCallback(event);
		});
		
		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int mods){
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			switch (action)
			{
			case GLFW_PRESS:
			{
				MouseButtonPressedEvent event(button);
				data.EventCallback(event);
				break;
			}
			case GLFW_RELEASE:
			{
				MouseButtonReleasedEvent event(button);
				data.EventCallback(event);
				break;
			}
			}
		});

		glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset){
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			MouseScrolledEvent event((float)xOffset, (float)yOffset);
			data.EventCallback(event);
		});

		glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double xPos, double yPos){
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			MouseMovedEvent event((float)xPos, (float)yPos);
			data.EventCallback(event);
		});
	}

	void WindowsWindow::Shutdown()
	{
		GE_PROFILE_FUNCTION();

		// bgfx holds this window's native handle, so it has to let go first.
		m_Context.reset();

		RemoveCustomChrome();

		glfwDestroyWindow(m_Window);
	}

	void WindowsWindow::OnUpdate()
	{
		GE_PROFILE_FUNCTION();

		glfwPollEvents();

		// The GLFW callback only records the new size; the swapchain is reset
		// here so it happens on a frame boundary. No-ops when nothing changed.
		m_Context->Resize(m_Data.Width, m_Data.Height);

		m_Context->Frame();
	}

	void WindowsWindow::SetVSync(bool enabled)
	{
		GE_PROFILE_FUNCTION();

		// Under bgfx vsync is a swapchain reset flag, not a GLFW swap interval.
		m_Context->SetVSync(enabled);
		m_Data.VSync = enabled;
	}

	bool WindowsWindow::IsVSync() const
	{
		return m_Data.VSync;
	}

	bool WindowsWindow::IsMaximized() const
	{
		return glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED) == GLFW_TRUE;
	}

	void WindowsWindow::Minimize()
	{
		glfwIconifyWindow(m_Window);
	}

	void WindowsWindow::ToggleMaximize()
	{
		if (IsMaximized())
			glfwRestoreWindow(m_Window);
		else
			glfwMaximizeWindow(m_Window);
	}

	void WindowsWindow::SetTitleBarHitTest(const WindowHitRect& caption,
		const WindowHitRect* exclusions, uint32_t exclusionCount)
	{
		m_HitTest.Assign(caption, exclusions, exclusionCount);
	}

	void WindowsWindow::InstallCustomChrome()
	{
		HWND hwnd = glfwGetWin32Window(m_Window);
		if (!hwnd)
			return;

		SetPropW(hwnd, kWindowProp, this);

		LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
		style |= WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
		SetWindowLongPtrW(hwnd, GWL_STYLE, style);

		m_PrevWndProc = (void*)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&WindowsWindow::CustomChromeWndProc);

		const DWORD corner = DWMWCP_DONOTROUND;
		DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

		MARGINS shadow{ 1, 1, 1, 1 };
		DwmExtendFrameIntoClientArea(hwnd, &shadow);

		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	void WindowsWindow::RemoveCustomChrome()
	{
		if (!m_Window || !m_PrevWndProc)
			return;

		HWND hwnd = glfwGetWin32Window(m_Window);
		if (hwnd)
		{
			SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)m_PrevWndProc);
			RemovePropW(hwnd, kWindowProp);
		}
		m_PrevWndProc = nullptr;
	}

	LRESULT CALLBACK WindowsWindow::CustomChromeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		auto* self = static_cast<WindowsWindow*>(GetPropW(hwnd, kWindowProp));
		const WNDPROC prev = self ? (WNDPROC)self->m_PrevWndProc : nullptr;

		if (!self || !self->m_CustomTitleBar || !prev)
		{
			if (prev)
				return CallWindowProcW(prev, hwnd, msg, wParam, lParam);
			return DefWindowProcW(hwnd, msg, wParam, lParam);
		}

		switch (msg)
		{
		case WM_NCCALCSIZE:
			if (wParam == TRUE)
			{
				auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
				if (IsZoomed(hwnd))
				{
					MONITORINFO mi{ sizeof(mi) };
					if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
					{
						params->rgrc[0] = mi.rcWork;
						// Auto-hide taskbar: rcWork == rcMonitor and a covering window is
						// treated as fullscreen, so the bar never comes back. 1 px inset
						// is the documented workaround (and the spec's maximized case).
						if (mi.rcWork.left == mi.rcMonitor.left &&
							mi.rcWork.top == mi.rcMonitor.top &&
							mi.rcWork.right == mi.rcMonitor.right &&
							mi.rcWork.bottom == mi.rcMonitor.bottom)
						{
							params->rgrc[0].top += 1;
						}
					}
				}
			}
			return 0;

		case WM_NCHITTEST:
			return CustomChromeHitTest(hwnd, self->m_HitTest, lParam);
		}

		return CallWindowProcW(prev, hwnd, msg, wParam, lParam);
	}
}
