#include "gepch.h"
#include "Application.h"
#include "GanymedE/events/ApplicationEvent.h"
#include "GanymedE/events/KeyEvent.h"

#include "GanymedE/Renderer/Renderer.h"
#include "GanymedE/Scripting/ScriptEngine.h"
#include "GanymedE/UI/UIEngine.h"

#include "GanymedE/Core/Input.h"
#include "GanymedE/Core/KeyCodes.h"

#include <GLFW/glfw3.h>

namespace GanymedE {

	Application* Application::s_instance = nullptr;
	ApplicationCommandLineArgs Application::s_CommandLineArgs;

	Application::Application(const std::string& name)
	{
		GE_PROFILE_FUNCTION();

		GE_CORE_ASSERT(!s_instance, "Application cannot have two instances!");
		s_instance = this;

		m_Window = std::unique_ptr<GanymedE::Window>(Window::Create(WindowProps(name)));
		m_Window->SetEventCallback(BIND_CALLBACK_FN(Application::OnEvent, this));

		Renderer::Init();

		// Application scope, not the editor's, because the VM belongs to the engine the way the
		// renderer does — a Scene constructed by any app registers a LuaScriptSystem. It needs no
		// AssetManager at this point; script assets are only resolved when one is instantiated.
		ScriptEngine::Init();

		// After both: it needs a live bgfx with compiled shaders, and it shares
		// ScriptEngine's lua_State. Sized to the window; the editor re-sizes it to
		// the viewport once that exists.
		UIEngine::Init(m_Window->GetWidth(), m_Window->GetHeight());

		m_ImGuiLayer = new ImGuiLayer();
		PushOverlay(m_ImGuiLayer);
	}

	Application::~Application()
	{
		GE_PROFILE_FUNCTION();

		// Strict order. UIEngine first: the RmlUi Lua plugin holds references into
		// ScriptEngine's VM, and Rml::Shutdown releases GPU textures, so it must run
		// while bgfx is still alive. Then the VM, then the GPU.
		UIEngine::Shutdown();
		ScriptEngine::Shutdown();

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

			// The dormancy gate is a leftover migration kill-switch and is
			// hard-false now that the scene path runs fully on bgfx; it goes
			// away with Renderer::IsSceneRenderPathDormant in Phase 7.
			if (!m_Minimized && !Renderer::IsSceneRenderPathDormant())
			{
				GE_PROFILE_SCOPE("LayerStack OnUpdate");

				for (Layer* layer : m_LayerStack)
					layer->OnUpdate(timestep);
			}

			m_ImGuiLayer->Begin();
			{
				GE_PROFILE_SCOPE("LayerStack OnImGuiRender");

				for (Layer* layer : m_LayerStack)
					layer->OnImGuiRender();
			}
			m_ImGuiLayer->End();

			m_Window->OnUpdate();
		}
	}
}