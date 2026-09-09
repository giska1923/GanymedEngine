#include "gepch.h"
#include "Application.h"
#include "GanymedE/events/ApplicationEvent.h"
#include "GanymedE/events/KeyEvent.h"

#include "GanymedE/Audio/AudioEngine.h"
#include "GanymedE/Reflection/Reflection.h"
#include "GanymedE/Renderer/Renderer.h"
#include "GanymedE/Scripting/ScriptEngine.h"
#include "GanymedE/UI/UIEngine.h"

#include "GanymedE/Core/Input.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Core/JobSystem.h"
#include "GanymedE/Core/KeyCodes.h"

#include <GLFW/glfw3.h>

namespace GanymedE {

	Application* Application::s_instance = nullptr;
	ApplicationCommandLineArgs Application::s_CommandLineArgs;

	Application::Application(const std::string& name)
		: Application(ApplicationSpecification{ name })
	{
	}

	Application::Application(const ApplicationSpecification& specification)
		: m_Specification(specification)
	{
		GE_PROFILE_FUNCTION();

		GE_CORE_ASSERT(!s_instance, "Application cannot have two instances!");
		s_instance = this;

		// First, before the window and the renderer, for two reasons. It depends on
		// nothing, so anything initialised after it may submit work — and enkiTS numbers
		// the thread that initialises it as thread 0, so doing it here is what makes
		// "thread 0" and "the thread that owns bgfx submission" the same thread by
		// construction rather than by convention. JobSystem::IsMainThread is only
		// trustworthy because of that.
		JobSystem::Init();

		// Before the window, because it needs nothing at all - it only populates entt's meta
		// context. Anything constructed after this point may assume every component is reflected,
		// which matters most for a tool or a Sandbox app that builds a Scene before the first
		// frame. In Debug it self-validates and asserts, so a registration mistake surfaces at
		// boot rather than the first time a panel draws.
		Reflection::Init();

		m_Window = std::unique_ptr<GanymedE::Window>(Window::Create(
			WindowProps(m_Specification.Name, m_Specification.Width, m_Specification.Height,
				m_Specification.Fullscreen)));
		m_Window->SetEventCallback(BIND_CALLBACK_FN(Application::OnEvent, this));

		Renderer::Init();

		// No ordering dependency either way - audio touches neither the GPU nor the VM.
		// It sits here so the boot log reads renderer, audio, scripting, UI, and so that
		// a device failure is reported before anything slower has run. Failure is not
		// fatal: the app continues silent (see AudioEngine.h).
		AudioEngine::Init();

		// Application scope, not the editor's, because the VM belongs to the engine the way the
		// renderer does — a Scene constructed by any app registers a LuaScriptSystem. It needs no
		// AssetManager at this point; script assets are only resolved when one is instantiated.
		ScriptEngine::Init();

		// After both: it needs a live bgfx with compiled shaders, and it shares
		// ScriptEngine's lua_State. Sized to the window; the editor re-sizes it to
		// the viewport once that exists.
		UIEngine::Init(m_Window->GetWidth(), m_Window->GetHeight());

		// A shipped game has no editor chrome, and ImGui is not passive: it installs its
		// own GLFW callbacks and blocks events by default. Leaving it out is the whole
		// opt-out - layers' OnImGuiRender simply never runs.
		if (m_Specification.EnableImGui)
		{
			m_ImGuiLayer = new ImGuiLayer();
			PushOverlay(m_ImGuiLayer);
		}
	}

	Application::~Application()
	{
		GE_PROFILE_FUNCTION();

		// Strict order. UIEngine first: the RmlUi Lua plugin holds references into
		// ScriptEngine's VM, and Rml::Shutdown releases GPU textures, so it must run
		// while bgfx is still alive. Then the VM, then the GPU.
		UIEngine::Shutdown();
		ScriptEngine::Shutdown();

		// Before the LayerStack unwinds, which is why every AudioEngine call is guarded
		// by an alive flag: a layer stopping its sounds in OnDetach runs after this.
		AudioEngine::Shutdown();

		// Deliberately not the mirror of Init's position. Shutdown waits for outstanding
		// jobs and drains the main-thread queue one last time, and that queue is where the
		// bgfx half of a background load lives — so it has to run while the renderer is
		// still up. Joining after Renderer::Shutdown would hand a live bgfx call a dead
		// context.
		JobSystem::Shutdown();

		Renderer::Shutdown();
	}

	void Application::PushLayer(Layer* layer)
	{
		GE_PROFILE_FUNCTION();

		m_LayerStack.PushLayer(layer);
		layer->OnAttach();
		layer->SetAttached(true);
	}

	void Application::PushOverlay(Layer* overlay)
	{
		GE_PROFILE_FUNCTION();

		m_LayerStack.PushOverlay(overlay);
		overlay->OnAttach();
		overlay->SetAttached(true);
	}

	void Application::Close()
	{
		m_Running = false;
	}

	void Application::OnEvent(Event& e)
	{
		GE_PROFILE_FUNCTION();

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(BIND_CALLBACK_FN(Application::OnWindowClose, this));
		dispatcher.Dispatch<WindowResizeEvent>(BIND_CALLBACK_FN(Application::OnWindowResize, this));
		dispatcher.Dispatch<KeyPressedEvent>(BIND_CALLBACK_FN(Application::OnKeyPressed, this));

		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
		{
			if (e.IsHandled())
				break;

			// A layer that never attached has no state to handle events with -
			// ImGuiLayer, for one, dereferences a context OnAttach creates.
			if (!(*it)->IsAttached())
				continue;

			(*it)->OnEvent(e);
		}
	}

	bool Application::OnWindowClose(WindowCloseEvent& e)
	{
		m_Running = false;
		return true;
	}

	bool Application::OnKeyPressed(KeyPressedEvent& e)
	{
		if (e.GetKeyCode() == Key::F1)
		{
			Renderer::SetDebugStatsEnabled(!Renderer::IsDebugStatsEnabled());
			return true;
		}

		return false;
	}

	bool Application::OnWindowResize(WindowResizeEvent& e)
	{
		GE_PROFILE_FUNCTION();

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
		GE_PROFILE_FUNCTION();

		while (m_Running)
		{
			GE_PROFILE_SCOPE("RunLoop");

			float time = (float)glfwGetTime(); // Platform::GetTime()
			Timestep timestep = time - m_LastFrameTime;
			m_LastFrameTime = time;

			// Before the layers, and outside the minimised gate. Before, so a resource a
			// worker finished last frame becomes usable by the systems that draw with it
			// this frame instead of a frame later. Outside, because a minimised window has
			// not stopped background loading, and a queue nobody drains grows without bound.
			JobSystem::OnUpdate();

			// Immediately after, and for the same reason: a parse that finished on a worker
			// becomes a usable GPU resource here, in time for the systems that draw with it this
			// frame. This is the only place the asset layer creates bgfx resources off the
			// synchronous path.
			AssetManager::Update();

			// The dormancy gate is a leftover migration kill-switch and is
			// hard-false now that the scene path runs fully on bgfx; it goes
			// away with Renderer::IsSceneRenderPathDormant in Phase 7.
			if (!m_Minimized && !Renderer::IsSceneRenderPathDormant())
			{
				GE_PROFILE_SCOPE("LayerStack OnUpdate");

				for (Layer* layer : m_LayerStack)
					layer->OnUpdate(timestep);
			}

			// Outside the minimised gate on purpose: a window nobody is looking at has
			// not stopped making noise, and finished one-shots still need reaping.
			AudioEngine::OnUpdate();

			if (m_ImGuiLayer)
			{
				m_ImGuiLayer->Begin();
				{
					GE_PROFILE_SCOPE("LayerStack OnImGuiRender");

					for (Layer* layer : m_LayerStack)
						layer->OnImGuiRender();
				}
				m_ImGuiLayer->End();
			}

			m_Window->OnUpdate();
		}
	}
}