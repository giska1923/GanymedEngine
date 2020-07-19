#pragma once
#include "GanymedE/Core.h"
#include "GanymedE/events/Event.h"
#include "GanymedE/Window.h"

namespace GanymedE {
	class WindowCloseEvent;

	class GE_API Application {
	public:
		Application();
		virtual ~Application();

		void Run();

		void OnEvent(Event& e);
	private:
		bool OnWindowClose(WindowCloseEvent& e);
		std::unique_ptr<GanymedE::Window> m_Window;
		bool m_Running = true;
	};

	// THIS FUNCTION NEEDS TO BE IMPLEMENTED ON CLIENT IN ORDER TO START THE ENGINE
	Application* CreateApplication();
}