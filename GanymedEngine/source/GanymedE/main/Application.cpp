#include "gepch.h"
#include "Application.h"
#include "GanymedE/events/ApplicationEvent.h"

#include <glad/glad.h>

#include "GanymedE/Input.h"

namespace GanymedE {

	Application* Application::s_instance = nullptr;

	Application::Application() {
		GE_CORE_ASSERT(!s_instance, "Application cannot have two instances!");
		s_instance = this;

		m_Window = std::unique_ptr<GanymedE::Window>(Window::Create());
		m_Window->SetEventCallback(BIND_CALLBACK_FN(Application::OnEvent, this));
	}

	Application::~Application() {

	}

	void Application::PushLayer(Layer* layer) {
		m_LayerStack.PushLayer(layer);
		layer->OnAttach();
	}

	void Application::PushOverlay(Layer* overlay) {
		m_LayerStack.PushOverlay(overlay);
		overlay->OnAttach();
	}

	void Application::OnEvent(Event& e) {
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(BIND_CALLBACK_FN(Application::OnWindowClose, this));

		for (auto it = m_LayerStack.end(); it != m_LayerStack.begin();) {
			(*--it)->OnEvent(e);
			if (e.IsHandled())
				break;
		}
	}

	bool Application::OnWindowClose(WindowCloseEvent& e) {
		m_Running = false;
		return true;
	}

	void Application::Run() {
		while (m_Running) {
			glClearColor(1, 0, 1, 1);
			glClear(GL_COLOR_BUFFER_BIT);

			for (Layer* layer : m_LayerStack)
				layer->OnUpdate();
			
			m_Window->OnUpdate();
		}
	}
}