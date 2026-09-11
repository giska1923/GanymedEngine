#include "RuntimeLayer.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Scene/SceneSerializer.h"
#include "GanymedE/UI/UIEngine.h"

namespace GanymedE {

	RuntimeLayer::RuntimeLayer(RuntimeConfig config)
		: Layer("RuntimeLayer"), m_Config(std::move(config))
	{
	}

	void RuntimeLayer::OnAttach()
	{
		GE_PROFILE_FUNCTION();

		Window& window = Application::Get().GetWindow();
		const uint32_t width = window.GetWidth();
		const uint32_t height = window.GetHeight();

		GE_INFO("--- Runtime boot ---");
		GE_INFO("Window: {0}x{1}{2}", width, height,
			Application::Get().GetSpecification().Fullscreen ? " (borderless fullscreen)" : "");
		m_Config.Log();

		// Before any deserialize: the scene format stores bare asset handles, and every
		// handle -> path lookup goes through the index this builds. Writing is off because a
		// shipped game must not touch its own install directory, which is why every shipped
		// asset needs its `.meta` sidecar committed - a read-only scan can only adopt the
		// identity it finds, never persist one. See assets.md.
		AssetManager::Init(/*writableAssets=*/false);

		m_SceneRenderer = CreateRef<SceneRenderer>(width, height);

		// The two calls that make this a game rather than an editor viewport: the post
		// stack's final pass goes to the backbuffer, and the UI composites there too
		// rather than into the composite framebuffer nobody is going to display.
		m_SceneRenderer->SetOutputToBackbuffer(true);
		UIEngine::SetTarget(nullptr);
		UIEngine::SetViewport(width, height);
		// SetViewportOrigin stays at its (0,0) default: the game owns the whole window,
		// so window-relative mouse positions are already UI-relative.

		// Straight into the play scene - no Scene::Copy. The copy exists to preserve the
		// editor's edit-time scene across play/stop; there is no edit-time scene here and
		// no stop, so a copy would only cost a full registry duplication at boot.
		m_Scene = CreateRef<Scene>();

		SceneSerializer serializer(m_Scene);
		if (serializer.Deserialize(m_Config.StartScene))
		{
			// Counted through TagComponent rather than the entity storage: every entity
			// the serializer creates gets one, and a single-component view's size() is
			// stable across entt versions in a way the storage API is not.
			const size_t entityCount = m_Scene->Reg().view<TagComponent>().size();
			GE_INFO("Scene '{0}' loaded ({1} entities)", m_Config.StartScene, entityCount);

			Entity camera = m_Scene->GetPrimaryCameraEntity();
			if (camera)
				GE_INFO("Primary camera: '{0}'", camera.GetName());
			else
				GE_ERROR("Scene has no primary camera - the runtime passes no fallback, so "
					"this renders the clear colour only");

			m_Scene->OnViewportResize(width, height);
			m_Scene->OnRuntimeStart();
			m_RuntimeStarted = true;
			GE_INFO("Runtime started (physics scene up, scripts instantiated)");
		}
		else
		{
			GE_ERROR("Failed to load scene '{0}' - nothing will be simulated",
				m_Config.StartScene);
		}

		if (!m_Config.UIDocument.empty())
		{
			if (UIEngine::LoadDocument(m_Config.UIDocument))
				GE_INFO("UI document '{0}' loaded", m_Config.UIDocument);
			else
				GE_ERROR("UI document '{0}' failed to load", m_Config.UIDocument);
		}

		GE_INFO("--- Boot complete ---");
	}

	void RuntimeLayer::OnDetach()
	{
		GE_PROFILE_FUNCTION();

		UIEngine::CloseAllDocuments();

		if (m_RuntimeStarted)
			m_Scene->OnRuntimeStop();

		m_Scene.reset();
		m_SceneRenderer.reset();

		// Runs while bgfx is alive: LayerStack is destroyed before the Window, so the
		// GPU caches this drops release real handles. The same guarantee EditorLayer
		// relies on - see the shutdown-order note in docs/engine/core.md.
		AssetManager::Shutdown();
	}

	void RuntimeLayer::OnUpdate(Timestep ts)
	{
		GE_PROFILE_FUNCTION();

		Renderer2D::ResetStats();
		Renderer3D::ResetStats();

		m_SceneRenderer->BeginFrame();

		// nullptr fallback camera: an editor camera is the editor's business, and a game
		// with no primary camera is a content bug the engine reports rather than hides
		// (RenderSystem logs it, throttled).
		if (m_RuntimeStarted)
			m_Scene->OnUpdateRuntime(ts, nullptr);

		// Update before render for the same reason the editor does it: gameplay scripts
		// have just written this frame's HUD values, and Context::Update lays out against
		// them. Where the submit lands is RenderPass::UI's business.
		UIEngine::OnUpdate(ts);
		UIEngine::OnRender();

		m_SceneRenderer->EndFrame();
	}

	void RuntimeLayer::OnEvent(Event& e)
	{
		// The HUD gets first refusal unconditionally. The editor gates this on the
		// viewport owning the pointer because a click could land on a panel instead;
		// here there is nothing else on screen to own it.
		UIEngine::OnEvent(e);

		if (e.IsHandled())
			return;

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowResizeEvent>(GE_BIND_EVENT_FN(RuntimeLayer::OnWindowResize));
		dispatcher.Dispatch<KeyPressedEvent>(GE_BIND_EVENT_FN(RuntimeLayer::OnKeyPressed));
	}

	bool RuntimeLayer::OnWindowResize(WindowResizeEvent& e)
	{
		const uint32_t width = e.GetWidth();
		const uint32_t height = e.GetHeight();

		// Application::OnWindowResize already swallowed minimize-to-zero, so anything
		// arriving here is a real size.
		m_SceneRenderer->SetViewportSize(width, height);
		if (m_RuntimeStarted)
			m_Scene->OnViewportResize(width, height);
		UIEngine::SetViewport(width, height);

		// No UIEngine::SetTarget re-set, unlike the editor: SetViewportSize rebuilt the
		// composite framebuffer, but the UI target is null here and stays null.

		GE_INFO("Resized to {0}x{1}", width, height);
		return false;
	}

	bool RuntimeLayer::OnKeyPressed(KeyPressedEvent& e)
	{
		// Placeholder for a pause menu, which is out of scope for this milestone. A
		// chromeless fullscreen window with no exit but Alt+F4 is a worse default than
		// a quit key that a real menu will later take over.
		if (e.GetKeyCode() == Key::Escape)
		{
			GE_INFO("Escape pressed - closing");
			Application::Get().Close();
			return true;
		}

		return false;
	}

}
