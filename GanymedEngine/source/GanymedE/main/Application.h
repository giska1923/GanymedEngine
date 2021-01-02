#pragma once
#include "GanymedE/Core.h"

#include "GanymedE/Window.h"
#include "GanymedE/LayerStack.h"
#include "GanymedE/events/Event.h"
#include "GanymedE/events/ApplicationEvent.h"

#include "GanymedE/ImGui/ImGuiLayer.h"

namespace GanymedE {
	class WindowCloseEvent;

	class GE_API Application {
	public:
		Application();
		virtual ~Application();

		void Run();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);

		inline static Application& Get() { return *s_instance; }
		inline Window& GetWindow() { return *m_Window; }
	private:
		bool OnWindowClose(WindowCloseEvent& e);
		std::unique_ptr<GanymedE::Window> m_Window;
		ImGuiLayer* m_ImGuiLayer;
		bool m_Running = true;

		LayerStack m_LayerStack;
	private:
		static Application* s_instance;
	};

	// THIS FUNCTION NEEDS TO BE IMPLEMENTED ON CLIENT IN ORDER TO START THE ENGINE
	Application* CreateApplication();
}