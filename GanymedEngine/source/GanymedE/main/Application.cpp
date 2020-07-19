#include "gepch.h"
#include "Application.h"
#include "GanymedE/events/ApplicationEvent.h"

#include <GLFW/glfw3.h>

namespace GanymedE {
	Application::Application() {
		m_Window = std::unique_ptr<GanymedE::Window>(Window::Create());
		m_Window->SetEventCallback(BIND_CALLBACK_FN(Application::OnEvent, this));
	}

	Application::~Application() {

	}

	void Application::OnEvent(Event& e) {
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(BIND_CALLBACK_FN(Application::OnWindowClose, this));

		GE_CORE_TRACE("{0}", e);
	}

	bool Application::OnWindowClose(WindowCloseEvent& e) {
		m_Running = false;
		return true;
	}

	void Application::Run() {
		while (m_Running) {
			glClearColor(1, 0, 1, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			m_Window->OnUpdate();
		}
	}
}