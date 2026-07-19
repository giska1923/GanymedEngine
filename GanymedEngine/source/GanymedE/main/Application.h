#pragma once
#include "GanymedE/Core/Core.h"

#include "GanymedE/Core/Window.h"
#include "GanymedE/Core/LayerStack.h"
#include "GanymedE/events/Event.h"
#include "GanymedE/events/ApplicationEvent.h"
#include "GanymedE/events/KeyEvent.h"

#include "GanymedE/Core/Timestep.h"

#include "GanymedE/ImGui/ImGuiLayer.h"

namespace GanymedE {
	class WindowCloseEvent;

	struct ApplicationCommandLineArgs
	{
		int Count = 0;
		char** Args = nullptr;

		const char* operator[](int index) const
		{
			GE_CORE_ASSERT(index < Count, "Command line argument index out of range!");
			return Args[index];
		}
	};

	class GE_API Application {
	public:
		Application(const std::string& name = "GanymedEngine");
		virtual ~Application();

		void Run();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);

		void Close();

		ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }

		inline static Application& Get() { return *s_instance; }
		inline Window& GetWindow() { return *m_Window; }

		// Set by the entry point before CreateApplication runs
		static void SetCommandLineArgs(const ApplicationCommandLineArgs& args) { s_CommandLineArgs = args; }
		static const ApplicationCommandLineArgs& GetCommandLineArgs() { return s_CommandLineArgs; }
	private:
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
		bool OnKeyPressed(KeyPressedEvent& e);
	private:
		std::unique_ptr<GanymedE::Window> m_Window;
		ImGuiLayer* m_ImGuiLayer;
		bool m_Running = true;
		bool m_Minimized = false;
		LayerStack m_LayerStack;
		float m_LastFrameTime = 0.f;
	private:
		static Application* s_instance;
		static ApplicationCommandLineArgs s_CommandLineArgs;
	};

	// THIS FUNCTION NEEDS TO BE IMPLEMENTED ON CLIENT IN ORDER TO START THE ENGINE
	Application* CreateApplication();
}