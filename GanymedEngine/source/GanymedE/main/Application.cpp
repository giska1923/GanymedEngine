#include "gepch.h"
#include "Application.h"
#include "GanymedE/events/ApplicationEvent.h"

#include "GanymedE/Renderer/Renderer.h"

#include "GanymedE/Core/Input.h"

#include <glfw/glfw3.h>

namespace GanymedE {

	Application* Application::s_instance = nullptr;
	
	Application::Application()
	{
		GE_CORE_ASSERT(!s_instance, "Application cannot have two instances!");
		s_instance = this;

		m_Window = std::unique_ptr<GanymedE::Window>(Window::Create());
		m_Window->SetEventCallback(BIND_CALLBACK_FN(Application::OnEvent, this));

		Renderer::Init();

		m_ImGuiLayer = new ImGuiLayer();
		PushOverlay(m_ImGuiLayer);
	}

	Application::~Application()
	{

	}

	void Application::PushLayer(Layer* layer)
	{
		m_LayerStack.PushLayer(layer);
		layer->OnAttach();
	}

	void Application::PushOverlay(Layer* overlay)
	{
		m_LayerStack.PushOverlay(overlay);
		overlay->OnAttach();
	}

	void Application::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(BIND_CALLBACK_FN(Application::OnWindowClose, this));
		dispatcher.Dispatch<WindowResizeEvent>(BIND_CALLBACK_FN(Application::OnWindowResize, this));

		for (auto it = m_LayerStack.end(); it != m_LayerStack.begin();) {
			(*--it)->OnEvent(e);
			if (e.IsHandled())
				break;
		}
	}

	bool Application::OnWindowClose(WindowCloseEvent& e)
	{
		m_Running = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& e)
	{
		if (e.GetWidth() == 0 || e.GetHeight() == 0)
		{
			m_Minimized = true;
			return false;
		}

		m_Minimized = false;
		Renderer::OnWindowResize(e.GetWidth(), e.GetHeight());

		return false;
	}

	void Application::Run()
	{
		while (m_Running)
		{
			float time = (float)glfwGetTime(); // Platform::GetTime()
			Timestep timestep = time - m_LastFrameTime;
			m_LastFrameTime = time;

			if (!m_Minimized)
			{
				for (Layer* layer : m_LayerStack)
					layer->OnUpdate(timestep);
			}

			m_ImGuiLayer->Begin();
			for (Layer* layer : m_LayerStack)
				layer->OnImGuiRender();
			m_ImGuiLayer->End();

			m_Window->OnUpdate();
		}
	}
}