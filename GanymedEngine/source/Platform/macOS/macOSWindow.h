#pragma once

#include "GanymedE/Core/Window.h"
#include "Platform/Bgfx/BgfxContext.h"

#include <GLFW/glfw3.h>

namespace GanymedE {

	class macOSWindow : public Window
	{
	public:
		macOSWindow(const WindowProps& props);
		virtual ~macOSWindow();

		void OnUpdate() override;

		uint32_t GetWidth() const override { return m_Data.Width; }
		uint32_t GetHeight() const override { return m_Data.Height; }

		// Window attributes
		void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		void SetVSync(bool enabled) override;
		bool IsVSync() const override;

		inline void* GetNativeWindow() const { return m_Window; }
	private:
		virtual void Init(const WindowProps& props);
		virtual void Shutdown();
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
	};

}