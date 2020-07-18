#include "gepch.h"
#include "Application.h"

#include <GLFW/glfw3.h>

namespace GanymedE {
	Application::Application() {
		m_Window = std::unique_ptr<GanymedE::Window>(Window::Create());
	}

	Application::~Application() {

	}

	void Application::Run() {
		while (m_Running) {
			glClearColor(1, 0, 1, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			m_Window->OnUpdate();
		}
	}
}