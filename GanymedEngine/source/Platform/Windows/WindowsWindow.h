#pragma once

#include "GanymedE/Core/Window.h"
#include "Platform/Bgfx/BgfxContext.h"

#include <GLFW/glfw3.h>
#include <Windows.h>

#ifdef IsMaximized
#undef IsMaximized
#endif
#ifdef IsMinimized
#undef IsMinimized
#endif

namespace GanymedE {

	class WindowsWindow : public Window
	{
	public:
		WindowsWindow(const WindowProps& props);
		virtual ~WindowsWindow();

		void OnUpdate() override;

		uint32_t GetWidth() const override { return m_Data.Width; }
		uint32_t GetHeight() const override { return m_Data.Height; }

		// Window attributes
		void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		void SetVSync(bool enabled) override;
		bool IsVSync() const override;

		inline void* GetNativeWindow() const { return m_Window; }

		bool HasCustomTitleBar() const override { return m_CustomTitleBar; }
		bool IsMaximized() const override;
		void Minimize() override;
		void ToggleMaximize() override;
		void SetTitleBarHitTest(const WindowHitRect& caption,
			const WindowHitRect* exclusions, uint32_t exclusionCount) override;
	private:
		virtual void Init(const WindowProps& props);
		virtual void Shutdown();
		void InstallCustomChrome();
		void RemoveCustomChrome();
		static LRESULT CALLBACK CustomChromeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	private:
		GLFWwindow* m_Window;
		Scope<BgfxContext> m_Context;

		struct WindowData
		{
			std::string Title;
			uint32_t Width, Height;
			bool VSync;

			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
		bool m_CustomTitleBar = false;
		void* m_PrevWndProc = nullptr;
		TitleBarHitTest m_HitTest;
	};

}