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
	class BgfxContext;

	struct ApplicationCommandLineArgs
	{
		int Count = 0;
		char** Args = nullptr;

		const char* operator[](int index) const
		{
			GE_CORE_ASSERT(index < Count, "Command line argument index out of range!");
			return Args[index];
		}

		// The first argument that is not an option, or nullptr.
		//
		// Both apps take a path positionally - a scene for the editor, a scene override for the
		// runtime - and both used to read `Args[1]` directly. That broke the moment the engine
		// grew its first flag (`--renderer=`, see BgfxContext): the flag landed in slot 1 and was
		// reported as a missing scene file. Options are read by whoever owns them, wherever they
		// appear, so positional handling has to skip them rather than assume slot 1.
		// `--root <dir>` is two arguments (see Log::Prepare). The directory must not come back
		// as the scene path.
		const char* FirstPositional() const
		{
			for (int i = 1; i < Count; i++)
			{
				if (!Args[i])
					continue;
				if (Args[i][0] == '-')
				{
					const std::string flag(Args[i]);
					if ((flag == "--root" || flag == "-root") && i + 1 < Count)
						i++;
					continue;
				}
				return Args[i];
			}

			return nullptr;
		}
	};

	// Host configuration, chosen by the app before any layer exists.
	//
	// A spec rather than a virtual hook on purpose: the ctor decides whether to push
	// ImGuiLayer, and a virtual dispatched from a base ctor still sees the base vtable,
	// so a UsesImGui() override could never fire. Configuration is also how production
	// engines separate host services from front-end chrome.
	struct ApplicationSpecification
	{
		std::string Name = "GanymedEngine";
		uint32_t Width = DEFAULT_WINDOW_WIDTH;
		uint32_t Height = DEFAULT_WINDOW_HEIGHT;
		bool Fullscreen = false;

		// Undecorated GLFW window + ImGui-drawn title bar. Default false so Sandbox
		// and the runtime keep a normal OS frame. The editor opts in. Wayland cannot
		// move an undecorated window and falls back to OS decorations at Window::Init.
		bool CustomTitleBar = false;

		// False builds no ImGuiLayer at all. Consequence worth knowing: nothing installs
		// the ImGui GLFW callbacks or blocks events, so every event reaches the game
		// layers raw - which is what a shipped runtime wants.
		bool EnableImGui = true;

		// No window, no audio, no UI. The renderer is bgfx's Noop, with a null native window.
		// The loop sleeps toward 60 Hz instead of waiting on a swap. GanymedDedicated sets this.
		bool Headless = false;

		// False skips the player backend client (sign-in, push socket). A dedicated server talks
		// only to its fleet agent, through ServerLifecycle, and must not open a session.
		bool PlayerBackend = true;
	};

	class GE_API Application {
	public:
		explicit Application(const ApplicationSpecification& specification);
		Application(const std::string& name = "GanymedEngine");
		virtual ~Application();

		void Run();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);

		void Close();

		// Null when the spec disabled ImGui - always check before dereferencing.
		ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }

		const ApplicationSpecification& GetSpecification() const { return m_Specification; }

		inline static Application& Get() { return *s_instance; }

		// False on a headless app. GetWindow() asserts when there is none.
		bool HasWindow() const { return static_cast<bool>(m_Window); }
		inline Window& GetWindow()
		{
			GE_CORE_ASSERT(m_Window, "Application::GetWindow called with no window");
			return *m_Window;
		}

		// Set by the entry point before CreateApplication runs
		static void SetCommandLineArgs(const ApplicationCommandLineArgs& args) { s_CommandLineArgs = args; }
		static const ApplicationCommandLineArgs& GetCommandLineArgs() { return s_CommandLineArgs; }
	private:
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
		bool OnKeyPressed(KeyPressedEvent& e);
		void OnFrame(Timestep timestep);
	private:
		ApplicationSpecification m_Specification;
		std::unique_ptr<GanymedE::Window> m_Window;
		std::unique_ptr<BgfxContext> m_HeadlessContext;
		ImGuiLayer* m_ImGuiLayer = nullptr;
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