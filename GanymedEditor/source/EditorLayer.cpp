#include "EditorLayer.h"
#include "AssetDragDrop.h"
#include "EditorFonts.h"
#include "EditorIcons.h"
#include "EditorInspector.h"
#include "EditorTheme.h"
#include "EditorTitleBar.h"
#include "EditorWidgets.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "GanymedE/Scene/SceneSerializer.h"
#include "GanymedE/Scene/PrefabSerializer.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/AssetWatcher.h"
#include "GanymedE/Assets/CompiledCache.h"
#include "GanymedE/Assets/MaterialSerializer.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/MeshShader.h"

#include "GanymedE/Utils/PlatformUtils.h"
#include "GanymedE/Math/Math.h"
#include "GanymedE/Renderer/MeshImporter.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/Renderer3D.h"
#include "GanymedE/UI/UIEngine.h"
#include "GanymedE/Scene/SceneSingletons.h"
#include "GanymedE/Scene/SceneCamera.h"

#include <ImGuizmo.h>
#include <bgfx/bgfx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace GanymedE {

	extern const std::filesystem::path g_AssetPath;

	namespace {

		// Bump when the DockBuilder default tree changes. Existing imgui.ini otherwise keeps
		// the old splits — including the phase-1 6% toolbar node — and View → Reset Layout
		// is easy to miss on the first launch after a chrome change.
		constexpr int kDockLayoutVersion = 2;
		int s_IniDockLayoutVersion = 0;

		void* DockLayoutReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
		{
			return std::strcmp(name, "Dock") == 0 ? (void*)1 : nullptr;
		}

		void DockLayoutReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
		{
			if (std::strncmp(line, "Version=", 8) == 0)
				s_IniDockLayoutVersion = std::atoi(line + 8);
		}

		void DockLayoutWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
		{
			buf->appendf("[%s][Dock]\nVersion=%d\n\n", handler->TypeName, kDockLayoutVersion);
		}

		void RegisterDockLayoutSettingsHandler()
		{
			ImGuiSettingsHandler handler;
			handler.TypeName = "GanymedEditor";
			handler.TypeHash = ImHashStr("GanymedEditor");
			handler.ReadOpenFn = DockLayoutReadOpen;
			handler.ReadLineFn = DockLayoutReadLine;
			handler.WriteAllFn = DockLayoutWriteAll;
			ImGui::AddSettingsHandler(&handler);
		}

		void BuildDefaultDockLayout(ImGuiID dockspaceId)
		{
			ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

			ImGuiID dockMain = dockspaceId;
			ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.28f, nullptr, &dockMain);
			ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.24f, nullptr, &dockMain);
			ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.22f, nullptr, &dockMain);
			ImGuiID dockLeftBottom = ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Down, 0.5f, nullptr, &dockLeft);

			ImGui::DockBuilderDockWindow("Scene Hierarchy", dockLeft);
			ImGui::DockBuilderDockWindow("Properties", dockLeftBottom);
			ImGui::DockBuilderDockWindow("Viewport", dockMain);
			ImGui::DockBuilderDockWindow("Stats", dockRight);
			ImGui::DockBuilderDockWindow("Content Browser", dockBottom);
			ImGui::DockBuilderFinish(dockspaceId);
		}

	}

	EditorLayer::EditorLayer()
		: Layer("EditorLayer"), m_GizmoType(ImGuizmo::OPERATION::TRANSLATE)
	{
	}

	void EditorLayer::OnAttach()
	{
		GE_PROFILE_FUNCTION();

		AssetManager::Init();

		// After ImGuiLayer::OnAttach (Application's constructor pushed that overlay
		// first): the context exists, the default atlas is already uploaded, and
		// Clear() here makes NewFrame rebuild it with Inter + Lucide.
		EditorUI::EditorFonts::Load();
		EditorUI::ApplyTheme(EditorUI::MakeDarkTheme());
		EditorUI::InitTitleBar();
		RegisterDockLayoutSettingsHandler();

		// After Reflection::Init (Application's constructor), because a drawer is keyed on a
		// meta_type that has to exist first.
		EditorUI::InitPropertyDrawers();

		m_CheckerboardTexture = Texture2D::Create("assets/textures/Checkerboard.png");

		m_SceneRenderer = CreateRef<SceneRenderer>(1280, 720);

		// Game UI composites into the same LDR target the viewport image shows, so
		// the HUD appears inside the viewport rather than over the whole editor.
		UIEngine::SetTarget(m_SceneRenderer->GetCompositeFramebuffer());

		m_EditorScene = CreateRef<Scene>();
		SetupDefaultEnvironment(m_EditorScene);
		m_ActiveScene = m_EditorScene;

		m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 1000.0f);

		RetargetPanels();

		// Optional scene on the command line: GanymedEditor [--renderer=<backend>] [path/to/scene.ganymede]
		// FirstPositional, not Args[1]: an option may come first.
		if (const char* positional = Application::GetCommandLineArgs().FirstPositional())
		{
			std::filesystem::path scenePath = positional;
			if (std::filesystem::exists(scenePath))
				OpenScene(scenePath);
			else
				GE_WARN("Scene passed on the command line not found: {0}", scenePath.string());
		}
	}

	void EditorLayer::OnDetach()
	{
		GE_PROFILE_FUNCTION();
		EditorUI::ShutdownTitleBar();
		AssetManager::Shutdown();
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		GE_PROFILE_FUNCTION();

		// Status-bar FPS. Skip dt >= 1 s so a debugger pause does not pull the
		// average to 1; skip tiny dt so a hitch-recovery spike cannot mint 10k FPS.
		{
			const float dt = ts.GetSeconds();
			if (dt > 0.0001f && dt < 1.0f)
			{
				const float instant = 1.0f / dt;
				m_SmoothedFps = (m_SmoothedFps <= 0.0f)
					? instant
					: m_SmoothedFps + (instant - m_SmoothedFps) * 0.1f;
			}
		}

		// Resize
		if (m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f && // zero sized framebuffer is invalid
			(m_SceneRenderer->GetWidth() != (uint32_t)m_ViewportSize.x || m_SceneRenderer->GetHeight() != (uint32_t)m_ViewportSize.y))
		{
			m_SceneRenderer->SetViewportSize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
			m_EditorCamera.SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);

			m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);

			// SetViewportSize rebuilds the post-stack targets, so the composite
			// framebuffer is a different object afterwards - re-point the UI at it
			// or it keeps compositing into the destroyed one.
			UIEngine::SetViewport((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
			UIEngine::SetTarget(m_SceneRenderer->GetCompositeFramebuffer());
		}

		// Render
		Renderer2D::ResetStats();
		Renderer3D::ResetStats();
		m_SceneRenderer->BeginFrame();

		// Update scene
		switch (m_SceneState)
		{
			case SceneState::Edit:
			{
				if (m_ViewportCamera == UUID{ 0 })
					m_EditorCamera.OnUpdate(ts);

				m_ActiveScene->GetSingleton<EditorViewFilter>().HiddenEntities =
					&m_SceneHierarchyPanel.HiddenEntities();
				m_ActiveScene->GetSingleton<RenderContext>().PreviewCamera = m_ViewportCamera;
				m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
				break;
			}
			case SceneState::Play:
			{
				// Fall back to the editor camera when the scene has no primary Camera
				m_EditorCamera.OnUpdate(ts);

				PhysicsSettings& physicsSettings = m_ActiveScene->GetSingleton<PhysicsSettings>();
				physicsSettings.DebugDraw = m_PhysicsDebugDraw;
				// Editor-only opt-in: the engine defaults this off so a shipped game never
				// draws authored collider wireframes. Pushed every frame for the same reason
				// DebugDraw is - Scene::Copy does not carry singletons, so the play scene
				// starts each run with engine defaults.
				physicsSettings.ShowColliderGizmos = true;

				m_ActiveScene->OnUpdateRuntime(ts, &m_EditorCamera);

				// Update order matters even though the render order does not:
				// gameplay scripts have just set this frame's values, and the
				// context lays out and animates against them. The actual submit
				// lands wherever - RenderPass::UI decides when it draws.
				UIEngine::OnUpdate(ts);
				UIEngine::OnRender();
				break;
			}
		}

		// Mouse picking: read back the entity ID under the cursor
		auto [mx, my] = ImGui::GetMousePos();
		mx -= m_ViewportBounds[0].x;
		my -= m_ViewportBounds[0].y;
		glm::vec2 viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];

		// Render-target origin differs per backend: GL addresses from the bottom
		// left, D3D/Vulkan/Metal from the top left. Ask bgfx rather than assume,
		// or picking is vertically mirrored on half the backends.
		if (bgfx::getCaps()->originBottomLeft)
			my = viewportSize.y - my;

		int mouseX = (int)mx;
		int mouseY = (int)my;

		// Picking is asynchronous now: queue this frame's pick and take whatever
		// has landed. The result trails the cursor by a frame or two, which is
		// invisible for hover highlighting.
		if (mouseX >= 0 && mouseY >= 0 && mouseX < (int)viewportSize.x && mouseY < (int)viewportSize.y)
		{
			m_SceneRenderer->RequestEntityID(mouseX, mouseY);

			int pixelData = -1;
			if (m_SceneRenderer->PollEntityID(pixelData))
				m_HoveredEntity = pixelData == -1 ? Entity() : Entity((entt::entity)pixelData, m_ActiveScene.get());
		}
		else
		{
			m_HoveredEntity = {};
		}

		// Post stack: bloom -> tonemap -> FXAA into the composite shown in the viewport
		m_SceneRenderer->EndFrame();
	}

	void EditorLayer::OnImGuiRender()
	{
		GE_PROFILE_FUNCTION();

		// ImGuizmo needs its per-frame setup before any Manipulate call
		ImGuizmo::BeginFrame();

		// Note: Switch this to true to enable dockspace
		static bool dockspaceOpen = true;
		static bool opt_fullscreen_persistant = true;
		bool opt_fullscreen = opt_fullscreen_persistant;
		static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

		// We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
		// because it would be confusing to have two docking targets within each others.
		const bool customChrome = Application::Get().GetWindow().HasCustomTitleBar();
		ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking;
		if (!customChrome)
			window_flags |= ImGuiWindowFlags_MenuBar;
		window_flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		if (opt_fullscreen)
		{
			ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->Pos);
			ImGui::SetNextWindowSize(viewport->Size);
			ImGui::SetNextWindowViewport(viewport->ID);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
			window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
		}

		// When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background and handle the pass-thru hole, so we ask Begin() to not render a background.
		if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
			window_flags |= ImGuiWindowFlags_NoBackground;

		// Important: note that we proceed even if Begin() returns false (aka window is collapsed).
		// This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
		// all active windows docked into it will lose their parent and become undocked.
		// We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
		// any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("DockSpace Demo", &dockspaceOpen, window_flags);
		ImGui::PopStyleVar();

		if (opt_fullscreen)
			ImGui::PopStyleVar(2);

		// Theme ItemSpacing.y is 4. Between title, toolbar, DockSpace and status that
		// accumulates into a few pixels of overflow and a host scrollbar at every size.
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

		if (customChrome)
			UI_TitleBar();
		else if (ImGui::BeginMenuBar())
		{
			UI_Menus();
			ImGui::EndMenuBar();
		}

		UI_Toolbar();

		ImGuiIO& io = ImGui::GetIO();
		if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
		{
			ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");

			if (s_IniDockLayoutVersion != kDockLayoutVersion)
			{
				m_ResetDockLayout = true;
				s_IniDockLayoutVersion = kDockLayoutVersion;
				ImGui::MarkIniSettingsDirty();
			}
			static bool s_ClearedGhostToolbar = false;
			if (!s_ClearedGhostToolbar)
			{
				ImGui::ClearWindowSettings("##toolbar");
				s_ClearedGhostToolbar = true;
			}
			if (m_ResetDockLayout)
			{
				ImGui::DockBuilderRemoveNode(dockspace_id);
				m_ResetDockLayout = false;
			}
			if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr)
				BuildDefaultDockLayout(dockspace_id);

			ImGui::DockSpace(dockspace_id, ImVec2(0.0f, -EditorUI::Theme().StatusBarHeight), dockspace_flags);
		}

		UI_StatusBar();
		ImGui::PopStyleVar();

		m_SceneHierarchyPanel.OnImGuiRender();
		m_ContentBrowserPanel.OnImGuiRender();

		ImGui::Begin("Stats");

		std::string hoveredEntityName = "None";
		if (m_HoveredEntity)
			hoveredEntityName = m_HoveredEntity.GetComponent<TagComponent>().Tag;
		ImGui::Text("Hovered Entity: %s", hoveredEntityName.c_str());

		auto stats = Renderer2D::GetStats();
		ImGui::Text("Renderer2D Stats:");
		ImGui::Text("Draw Calls: %d", stats.DrawCalls);
		ImGui::Text("Quads: %d", stats.QuadCount);
		ImGui::Text("Vertices: %d", stats.GetTotalVertexCount());
		ImGui::Text("Indices: %d", stats.GetTotalIndexCount());

		auto stats3D = Renderer3D::GetStats();
		ImGui::Text("Renderer3D Stats:");
		ImGui::Text("Draw Calls: %d", stats3D.DrawCalls);
		ImGui::Text("Meshes: %d", stats3D.MeshCount);
		ImGui::Text("Culled (frustum): %d", stats3D.CulledMeshes);
		ImGui::Text("Instanced Draws: %d", stats3D.InstancedDraws);
		ImGui::Text("Transparent: %d", stats3D.TransparentMeshes);
		ImGui::Text("Particles: %d emitters, %d billboards, %d draws, %d culled",
			stats3D.ParticleEmitters, stats3D.ParticleBillboards,
			stats3D.ParticleDrawCalls, stats3D.ParticleCulledEmitters);

		ImGui::Separator();
		// Resident is live objects; tracked is cache entries, which includes ones whose object
		// has been collected. The gap between the two is eviction actually happening - close a
		// scene and resident falls while tracked does not until those handles are loaded again.
		// Loading is parses in flight, which is what a cold open looks like from here.
		ImGui::Text("Asset Cache (resident / tracked / loading):");
		for (const AssetCacheStats& cache : AssetManager::GetCacheStats())
		{
			ImGui::Text("%s: %zu / %zu / %zu", cache.TypeName,
				cache.Resident, cache.Tracked, cache.Pending);
		}

		// Apply is the one half of an asynchronous load that cannot leave the main thread, so it
		// is the one that needs a ceiling. Deferred is finished parses waiting for a later frame:
		// a non-zero value during a burst is the budget working, not a backlog.
		const AssetApplyStats apply = AssetManager::GetApplyStats();
		ImGui::Text("Apply: %u done, %u deferred, %.2f / %.1f ms",
			apply.Applied, apply.Deferred, apply.Milliseconds, apply.BudgetMs);

		// Compiles vs cache hits is the number that says whether the compiled tree is doing its
		// job: a second run over an unchanged project must read 0 compiled. The in-flight count
		// is what a cold open looks like while it is happening.
		const CompiledCache::Stats compiled = CompiledCache::GetStats();
		ImGui::Text("Compiled: %u built (%.0f ms), %u from cache, %u running",
			compiled.Compiles, compiled.TotalCompileMs, compiled.CacheHits,
			CompiledCache::CompilesInFlight());

		// The switch exists for one situation and it is worth naming: a `git checkout` across a
		// branch that touches many assets generates a change event for every one of them.
		// Turning watching back on adopts the new state without reloading it.
		bool watching = AssetWatcher::IsEnabled();
		if (ImGui::Checkbox("Hot reload assets/", &watching))
			AssetWatcher::SetEnabled(watching);

		// ms/poll is the number that decides whether this stays a poll. The roadmap's
		// alternative is a Win32 ReadDirectoryChangesW watcher, worth writing the day this
		// column shows up in a frame - and not before.
		const AssetWatcher::Stats watch = AssetWatcher::GetStats();
		ImGui::TextDisabled("%zu watched, %.2f ms/poll, %u reloaded, %u settling",
			watch.Watched, watch.LastPollMs, watch.Reloads, watch.Settling);

		if (!watch.LastReloaded.empty())
			ImGui::TextDisabled("Last: %s", watch.LastReloaded.c_str());

		ImGui::Separator();
		ImGui::Text("Post Processing:");
		auto& rendererSettings = m_SceneRenderer->GetSettings();
		ImGui::DragFloat("Exposure", &rendererSettings.Exposure, 0.01f, 0.0f, 16.0f);
		ImGui::Checkbox("Bloom", &rendererSettings.BloomEnabled);
		ImGui::BeginDisabled(!rendererSettings.BloomEnabled);
		ImGui::DragFloat("Threshold", &rendererSettings.BloomThreshold, 0.01f, 0.0f, 16.0f);
		ImGui::DragFloat("Knee", &rendererSettings.BloomKnee, 0.01f, 0.01f, 1.0f);
		ImGui::DragFloat("Intensity", &rendererSettings.BloomIntensity, 0.01f, 0.0f, 4.0f);
		ImGui::DragFloat("Radius", &rendererSettings.BloomFilterRadius, 0.01f, 0.1f, 4.0f);
		ImGui::EndDisabled();
		ImGui::Checkbox("FXAA", &rendererSettings.FXAAEnabled);

		ImGui::End();

		UI_Viewport();

		HandleShortcuts();

		ImGui::End();
	}

	// Editor-global shortcuts, polled here rather than routed through OnKeyPressed.
	//
	// The engine event path cannot serve them: ImGuiLayer::BlockEvents is driven by viewport
	// focus/hover, so OnKeyPressed never fires while the Properties or Content Browser panel
	// has the mouse - Ctrl+Z over the inspector simply did nothing. Relaxing that policy was
	// the other option and is worse: it would leak *every* key into the engine path while
	// typing in a panel, so camera keys and the Q/W/E/R gizmo switches would fire mid-rename.
	// A command layer sitting above widget focus is the production norm; at Ganymed's scale,
	// polling ImGui inside the ImGui frame is that layer.
	//
	// WantTextInput is the one gate: while a text field is focused, Ctrl+Z belongs to ImGui's
	// own text undo, which is what every editor does.
	void EditorLayer::UI_TitleBar()
	{
		EditorUI::TitleBarState state;
		state.DocumentName = m_EditorScenePath.empty()
			? std::string("Untitled") : m_EditorScenePath.filename().string();
		state.Dirty = m_UndoStack.IsDirtySinceSave();
		EditorUI::DrawTitleBar(state, [this] { UI_Menus(); });
	}

	void EditorLayer::UI_Menus()
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New", "Ctrl+N"))
				NewScene();

			if (ImGui::MenuItem("Open...", "Ctrl+O"))
				OpenScene();

			if (ImGui::MenuItem("Save", "Ctrl+S"))
				SaveScene();

			if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
				SaveSceneAs();

			if (ImGui::MenuItem("Exit"))
				Application::Get().Close();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit"))
		{
			const bool editing = m_SceneState == SceneState::Edit;
			if (ImGui::MenuItem("Undo", "Ctrl+Z", false, editing && m_UndoStack.CanUndo()))
				m_UndoStack.Undo(*m_EditorScene);

			if (ImGui::MenuItem("Redo", "Ctrl+Y", false, editing && m_UndoStack.CanRedo()))
				m_UndoStack.Redo(*m_EditorScene);

			ImGui::Separator();

			const bool hasSelection = editing && m_SceneHierarchyPanel.GetSelectedEntity();
			if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection))
				m_SceneHierarchyPanel.DuplicateSelectedEntity();

			if (ImGui::MenuItem("Delete", "Del", false, hasSelection))
				m_SceneHierarchyPanel.DeleteSelectedEntity();

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View"))
		{
			// RmlUi's own inspector: element tree, computed RCSS, event log.
			// Debug builds only - the Debugger sources are not compiled otherwise.
			bool debuggerVisible = UIEngine::IsDebuggerVisible();
			if (ImGui::MenuItem("Game UI Debugger", "Ctrl+U", &debuggerVisible))
				UIEngine::SetDebuggerVisible(debuggerVisible);

			if (ImGui::MenuItem("Reset Layout"))
				m_ResetDockLayout = true;

			ImGui::Separator();

			// 0 = Dark (OnAttach default), 1 = Light. Same violet accent; the
			// ramps swap. Not persisted — imgui.ini has no theme key.
			static int s_ThemePreset = 0;
			if (ImGui::BeginMenu("Theme"))
			{
				if (ImGui::MenuItem("Dark", nullptr, s_ThemePreset == 0))
				{
					s_ThemePreset = 0;
					EditorUI::ApplyTheme(EditorUI::MakeDarkTheme());
				}
				if (ImGui::MenuItem("Light", nullptr, s_ThemePreset == 1))
				{
					s_ThemePreset = 1;
					EditorUI::ApplyTheme(EditorUI::MakeLightTheme());
				}
				ImGui::EndMenu();
			}

			ImGui::EndMenu();
		}
	}

	void EditorLayer::HandleShortcuts()
	{
		if (ImGui::GetIO().WantTextInput)
			return;

		// File shortcuts live here too. They used to be in OnKeyPressed and dead-zoned over
		// every panel for exactly the same reason.
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N))
			NewScene();

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
			OpenScene();

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S))
			SaveSceneAs();
		else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
			SaveScene();

		if (m_SceneState != SceneState::Edit || !m_EditorScene)
			return;

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
			m_UndoStack.Undo(*m_EditorScene);

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)
			|| ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
		{
			m_UndoStack.Redo(*m_EditorScene);
		}

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D))
			m_SceneHierarchyPanel.DuplicateSelectedEntity();

		if (ImGui::IsKeyPressed(ImGuiKey_Delete))
			m_SceneHierarchyPanel.DeleteSelectedEntity();
	}

	void EditorLayer::UI_Toolbar()
	{
		using EditorUI::Color;
		using EditorUI::IconButton;
		using EditorUI::Theme;
		using EditorUI::WithAlpha;

		const EditorUI::EditorTheme& theme = Theme();
		const float height = theme.ToolbarHeight;

		ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(theme.SurfaceBg));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
		ImGui::BeginChild("##MainToolbar", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

		constexpr float kBtn = 24.0f;
		const float rowY = (ImGui::GetContentRegionAvail().y - kBtn) * 0.5f;
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowY);

		const bool canSwitch = !ImGuizmo::IsUsing() && !Input::IsMouseButtonPressed(Mouse::ButtonRight);
		auto setGizmo = [&](int op)
		{
			if (canSwitch)
				m_GizmoType = op;
		};

		if (IconButton(ICON_LC_MOUSE_POINTER, "Select (Q)", m_GizmoType == -1))
			setGizmo(-1);
		ImGui::SameLine();
		if (IconButton(ICON_LC_MOVE, "Translate (W)", m_GizmoType == ImGuizmo::OPERATION::TRANSLATE))
			setGizmo(ImGuizmo::OPERATION::TRANSLATE);
		ImGui::SameLine();
		if (IconButton(ICON_LC_ROTATE_3D, "Rotate (E)", m_GizmoType == ImGuizmo::OPERATION::ROTATE))
			setGizmo(ImGuizmo::OPERATION::ROTATE);
		ImGui::SameLine();
		if (IconButton(ICON_LC_SCALING, "Scale (R)", m_GizmoType == ImGuizmo::OPERATION::SCALE))
			setGizmo(ImGuizmo::OPERATION::SCALE);

		const bool playing = m_SceneState == SceneState::Play;
		const char* playLabel = playing ? ICON_LC_SQUARE_STOP "  Stop" : ICON_LC_PLAY "  Play";
		const ImVec2 playSize(
			ImGui::CalcTextSize(playLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f,
			kBtn);
		ImGui::SetCursorPosX((ImGui::GetWindowSize().x - playSize.x) * 0.5f);
		ImGui::SetCursorPosY(rowY);

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(WithAlpha(theme.AccentHover, 0.35f)));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(WithAlpha(theme.Accent, 0.55f)));
		ImGui::PushStyleColor(ImGuiCol_Text, Color(playing ? theme.TextPrimary : theme.Success));
		ImGui::PushID("playstop");
		if (ImGui::Button(playLabel, playSize))
		{
			if (playing)
				OnSceneStop();
			else
				OnScenePlay();
		}
		ImGui::PopID();
		ImGui::PopStyleColor(4);

		const ImVec2 wp = ImGui::GetWindowPos();
		const ImVec2 ws = ImGui::GetWindowSize();
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2(wp.x, wp.y + ws.y - 1.0f),
			ImVec2(wp.x + ws.x, wp.y + ws.y - 1.0f),
			theme.Border);

		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	void EditorLayer::UI_StatusBar()
	{
		using EditorUI::Color;
		using EditorUI::StatusBarItem;
		using EditorUI::Theme;

		const EditorUI::EditorTheme& theme = Theme();
		const float height = theme.StatusBarHeight;

		ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(theme.ChromeBg));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 0.0f));
		ImGui::BeginChild("##StatusBar", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

		const float lineH = ImGui::GetTextLineHeight();
		const float rowY = (ImGui::GetContentRegionAvail().y - lineH) * 0.5f;
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowY);
		const ImVec2 lineStart = ImGui::GetCursorPos();
		const float availX = ImGui::GetContentRegionAvail().x;

		auto itemWidth = [](const char* icon, const char* text) -> float
		{
			float w = 0.0f;
			if (icon && icon[0])
				w += ImGui::CalcTextSize(icon).x;
			if (icon && icon[0] && text && text[0])
				w += 6.0f;
			if (text && text[0])
				w += ImGui::CalcTextSize(text).x;
			return w;
		};

		auto sep = [&]()
		{
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, Color(theme.TextDim));
			ImGui::TextUnformatted("·");
			ImGui::PopStyleColor();
			ImGui::SameLine();
		};

		const std::string sceneName = m_EditorScenePath.empty()
			? std::string("Untitled") : m_EditorScenePath.filename().string();
		const std::string sceneChip = sceneName + (m_UndoStack.IsDirtySinceSave() ? "*" : "");
		StatusBarItem(ICON_LC_FILE_TEXT, sceneChip.c_str());
		if (!m_EditorScenePath.empty())
			ImGui::SetItemTooltip("%s", m_EditorScenePath.string().c_str());

#if defined(GE_DEBUG)
		constexpr const char* kConfig = "Debug";
#elif defined(GE_RELEASE)
		constexpr const char* kConfig = "Release";
#elif defined(GE_DIST)
		constexpr const char* kConfig = "Dist";
#else
		constexpr const char* kConfig = nullptr;
#endif
		if (kConfig)
		{
			sep();
			StatusBarItem(ICON_LC_HAMMER, kConfig);
		}

		sep();
		StatusBarItem(ICON_LC_MONITOR, bgfx::getRendererName(bgfx::getRendererType()));

		const std::size_t entityCount = m_ActiveScene
			? m_ActiveScene->Reg().storage<IDComponent>().size()
			: 0;
		char entities[32];
		std::snprintf(entities, sizeof(entities), "%zu", entityCount);

		char draws[32];
		std::snprintf(draws, sizeof(draws), "%u", Renderer3D::GetStats().DrawCalls);

		char fps[32];
		std::snprintf(fps, sizeof(fps), "FPS %d", (int)(m_SmoothedFps + 0.5f));

		const bool playing = m_SceneState == SceneState::Play;
		const char* playIcon = playing ? ICON_LC_PLAY : ICON_LC_PENCIL;
		const char* playText = playing ? "Play" : "Edit";
		const ImU32 playColour = playing ? theme.Success : theme.TextDim;

		const float gap = ImGui::GetStyle().ItemSpacing.x;
		const float rightWidth =
			itemWidth(ICON_LC_BOXES, entities) + gap +
			itemWidth(ICON_LC_LAYERS, draws) + gap +
			itemWidth(ICON_LC_GAUGE, fps) + gap +
			itemWidth(playIcon, playText);

		const float rightX = lineStart.x + availX - rightWidth;
		if (rightX > ImGui::GetCursorPosX() + gap)
			ImGui::SetCursorPos(ImVec2(rightX, lineStart.y));
		else
			ImGui::SameLine();

		StatusBarItem(ICON_LC_GAUGE, fps);
		ImGui::SameLine();
		StatusBarItem(ICON_LC_BOXES, entities);
		ImGui::SetItemTooltip("Entities");
		ImGui::SameLine();
		StatusBarItem(ICON_LC_LAYERS, draws);
		ImGui::SetItemTooltip("Draw calls");
		ImGui::SameLine();
		StatusBarItem(playIcon, playText, playColour);

		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	void EditorLayer::UI_Viewport()
	{
		using EditorUI::BeginPanel;
		using EditorUI::Color;
		using EditorUI::EndPanel;
		using EditorUI::EndPanelToolbarRow;
		using EditorUI::IconButton;
		using EditorUI::PanelToolbarRow;
		using EditorUI::Theme;
		using EditorUI::ToolbarSeparator;

		if (!BeginPanel("Viewport"))
		{
			EndPanel();
			return;
		}

		const EditorUI::EditorTheme& theme = Theme();
		const bool editing = m_SceneState == SceneState::Edit;

		if (PanelToolbarRow("##ViewportTB"))
		{
			const ImVec2 row = ImGui::GetCursorPos();
			const float availX = ImGui::GetContentRegionAvail().x;

			std::string cameraLabel = "Editor Camera";
			if (editing && m_ViewportCamera != UUID{ 0 })
			{
				Entity preview = m_ActiveScene ? m_ActiveScene->FindEntityByUUID(m_ViewportCamera) : Entity{};
				if (preview && preview.HasComponent<CameraComponent>())
					cameraLabel = preview.GetComponent<TagComponent>().Tag;
				else
					m_ViewportCamera = UUID{ 0 };
			}
			else if (!editing && m_ActiveScene)
			{
				Entity primary = m_ActiveScene->GetPrimaryCameraEntity();
				if (primary)
					cameraLabel = primary.GetComponent<TagComponent>().Tag;
			}

			ImGui::BeginDisabled(!editing);
			ImGui::SetNextItemWidth(200.0f);
			if (ImGui::BeginCombo("##ViewportCam", cameraLabel.c_str()))
			{
				if (ImGui::Selectable("Editor Camera", m_ViewportCamera == UUID{ 0 }))
					m_ViewportCamera = UUID{ 0 };

				if (m_ActiveScene)
				{
					auto view = m_ActiveScene->Reg().view<CameraComponent, TagComponent, IDComponent>();
					for (auto entityID : view)
					{
						Entity entity{ entityID, m_ActiveScene.get() };
						const UUID id = entity.GetUUID();
						const std::string item = entity.GetComponent<TagComponent>().Tag
							+ "##" + std::to_string(static_cast<uint64_t>(id));
						if (ImGui::Selectable(item.c_str(), m_ViewportCamera == id))
							m_ViewportCamera = id;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::EndDisabled();
			if (!editing)
				ImGui::SetItemTooltip("Play uses the scene's primary camera");

			ImGui::SameLine();
			ToolbarSeparator();
			ImGui::SameLine();

			char aspect[48];
			std::snprintf(aspect, sizeof(aspect), "Free Aspect: %.0fx%.0f",
				m_ViewportSize.x, m_ViewportSize.y);
			ImGui::PushStyleColor(ImGuiCol_Text, Color(theme.TextDim));
			ImGui::TextUnformatted(aspect);
			ImGui::PopStyleColor();

			const float gap = ImGui::GetStyle().ItemSpacing.x;
			const char* spaceLabel = m_GizmoWorldSpace ? "World" : "Local";
			const float rightWidth = 24.0f + gap + 9.0f + gap + 72.0f;
			const float rightX = row.x + availX - rightWidth;
			if (rightX > ImGui::GetCursorPosX() + gap)
				ImGui::SetCursorPos(ImVec2(rightX, row.y));
			else
				ImGui::SameLine();

			if (IconButton(ICON_LC_BOXES, "Visualizers", m_PhysicsDebugDraw.Enabled))
				ImGui::OpenPopup("##Visualizers");
			if (ImGui::BeginPopup("##Visualizers"))
			{
				ImGui::Checkbox("Jolt Debug Draw", &m_PhysicsDebugDraw.Enabled);
				ImGui::BeginDisabled(!m_PhysicsDebugDraw.Enabled);
				ImGui::Checkbox("Wireframe Shapes", &m_PhysicsDebugDraw.Wireframe);
				ImGui::Checkbox("Bounding Boxes", &m_PhysicsDebugDraw.BoundingBoxes);
				ImGui::Checkbox("Velocities", &m_PhysicsDebugDraw.Velocities);
				ImGui::Checkbox("Center of Mass", &m_PhysicsDebugDraw.CenterOfMass);
				ImGui::Checkbox("Constraints", &m_PhysicsDebugDraw.Constraints);
				ImGui::EndDisabled();
				ImGui::TextDisabled("Visible during Play (Jolt body state)");
				ImGui::EndPopup();
			}

			ImGui::SameLine();
			ToolbarSeparator();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(72.0f);
			if (ImGui::BeginCombo("##GizmoSpace", spaceLabel))
			{
				if (ImGui::Selectable("Local", !m_GizmoWorldSpace))
					m_GizmoWorldSpace = false;
				if (ImGui::Selectable("World", m_GizmoWorldSpace))
					m_GizmoWorldSpace = true;
				ImGui::EndCombo();
			}
			ImGui::SetItemTooltip("Gizmo space");
		}
		EndPanelToolbarRow();

		m_ViewportFocused = ImGui::IsWindowFocused();

		ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
		m_ViewportSize = { viewportPanelSize.x, viewportPanelSize.y };

		// Image origin is below the header. Picking, ImGuizmo and RmlUi all
		// read m_ViewportBounds[0] from this cursor, so they stay correct.
		ImVec2 viewportScreenPos = ImGui::GetCursorScreenPos();
		m_ViewportBounds[0] = { viewportScreenPos.x, viewportScreenPos.y };
		m_ViewportBounds[1] = { viewportScreenPos.x + viewportPanelSize.x, viewportScreenPos.y + viewportPanelSize.y };

		UIEngine::SetViewportOrigin(m_ViewportBounds[0].x, m_ViewportBounds[0].y);

		uint32_t textureID = m_SceneRenderer->GetFinalImageRendererID();
		const bool flipV = bgfx::getCaps()->originBottomLeft;
		const ImVec2 uv0 = flipV ? ImVec2{ 0, 1 } : ImVec2{ 0, 0 };
		const ImVec2 uv1 = flipV ? ImVec2{ 1, 0 } : ImVec2{ 1, 1 };

		ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(textureID)),
			ImVec2{ m_ViewportSize.x, m_ViewportSize.y }, uv0, uv1);

		// Hover is the *image*, not the window: a click on the camera combo must
		// not also click-select whatever the pick buffer last saw.
		m_ViewportHovered = ImGui::IsItemHovered();
		Application::Get().GetImGuiLayer()->BlockEvents(!m_ViewportFocused && !m_ViewportHovered);

		if (auto drop = EditorUI::AcceptAssetDrop({ AssetType::Scene, AssetType::StaticMesh, AssetType::Prefab }))
		{
			std::filesystem::path fullPath = g_AssetPath / drop.Path;

			if (drop.Type == AssetType::Scene)
			{
				OpenScene(fullPath);
			}
			else if (drop.Type == AssetType::Prefab && editing)
			{
				m_SceneHierarchyPanel.InstantiatePrefab(drop.Path);
			}
			else if (drop.Type == AssetType::StaticMesh && editing)
			{
				Entity entity = MeshImporter::Instantiate(m_ActiveScene.get(), fullPath);
				if (entity)
					m_SceneHierarchyPanel.SetSelectedEntity(entity);
			}
		}

		Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
		Entity gizmoEntity = selectedEntity;
		if (gizmoEntity && m_SceneHierarchyPanel.IsLocked(gizmoEntity))
			gizmoEntity = {};
		if (gizmoEntity && m_GizmoType != -1 && editing)
		{
			glm::mat4 cameraProjection = m_EditorCamera.GetProjection();
			glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();
			bool ortho = false;
			if (m_ViewportCamera != UUID{ 0 })
			{
				Entity preview = m_ActiveScene->FindEntityByUUID(m_ViewportCamera);
				if (preview && preview.HasComponent<CameraComponent>())
				{
					const auto& cc = preview.GetComponent<CameraComponent>();
					cameraProjection = cc.Camera.GetProjection();
					cameraView = glm::inverse(m_ActiveScene->GetWorldSpaceTransform(preview));
					ortho = cc.Camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic;
				}
			}

			ImGuizmo::SetOrthographic(ortho);
			ImGuizmo::SetDrawlist();
			ImGuizmo::SetRect(m_ViewportBounds[0].x, m_ViewportBounds[0].y,
				m_ViewportBounds[1].x - m_ViewportBounds[0].x, m_ViewportBounds[1].y - m_ViewportBounds[0].y);

			auto& tc = gizmoEntity.GetComponent<TransformComponent>();
			glm::mat4 transform = m_ActiveScene->GetWorldSpaceTransform(gizmoEntity);

			bool snap = Input::IsKeyPressed(Key::LeftControl);
			float snapValue = 0.5f;
			if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
				snapValue = 45.0f;
			float snapValues[3] = { snapValue, snapValue, snapValue };

			ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection),
				(ImGuizmo::OPERATION)m_GizmoType,
				m_GizmoWorldSpace ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
				glm::value_ptr(transform),
				nullptr, snap ? snapValues : nullptr);

			if (ImGuizmo::IsUsing())
			{
				if (!m_GizmoUsing)
				{
					m_GizmoUsing = true;
					m_GizmoEntity = gizmoEntity.GetUUID();
					m_GizmoBefore = tc;
				}

				UUID parentID = gizmoEntity.GetComponent<RelationshipComponent>().Parent;
				if (parentID != UUID{ 0 })
				{
					Entity parent = m_ActiveScene->FindEntityByUUID(parentID);
					if (parent)
						transform = glm::inverse(m_ActiveScene->GetWorldSpaceTransform(parent)) * transform;
				}

				glm::vec3 translation, rotation, scale;
				Math::DecomposeTransform(transform, translation, rotation, scale);

				glm::vec3 deltaRotation = rotation - tc.Rotation;
				tc.Translation = translation;
				tc.Rotation += deltaRotation;
				tc.Scale = scale;

				m_ActiveScene->MarkChanged<TransformComponent>(gizmoEntity);
			}
		}

		if (m_GizmoUsing && !ImGuizmo::IsUsing())
		{
			m_GizmoUsing = false;

			Entity dragged = m_EditorScene ? m_EditorScene->FindEntityByUUID(m_GizmoEntity) : Entity{};
			if (dragged && editing && dragged.HasComponent<TransformComponent>())
			{
				m_UndoStack.Push(CreateScope<ComponentEditCommand<TransformComponent>>(
					"Gizmo Transform", m_GizmoEntity, m_GizmoBefore,
					dragged.GetComponent<TransformComponent>()));
			}
		}

		if (selectedEntity && selectedEntity.HasComponent<TransformComponent>())
		{
			glm::vec3 t = selectedEntity.GetComponent<TransformComponent>().Translation;
			if (m_GizmoWorldSpace)
				t = glm::vec3(m_ActiveScene->GetWorldSpaceTransform(selectedEntity)[3]);

			ImDrawList* draw = ImGui::GetWindowDrawList();
			ImVec2 p{ m_ViewportBounds[0].x + 12.0f, m_ViewportBounds[1].y - 28.0f };
			auto axis = [&](const char* label, ImU32 colour, float value)
			{
				draw->AddText(p, colour, label);
				p.x += ImGui::CalcTextSize(label).x;
				char buf[24];
				std::snprintf(buf, sizeof(buf), " %.3f", value);
				draw->AddText(p, theme.TextPrimary, buf);
				p.x += ImGui::CalcTextSize(buf).x + 16.0f;
			};
			axis("X", theme.AxisX, t.x);
			axis("Y", theme.AxisY, t.y);
			axis("Z", theme.AxisZ, t.z);
		}

		EndPanel();
	}

	void EditorLayer::OnEvent(Event& e)
	{
		if (m_SceneState == SceneState::Edit && m_ViewportCamera == UUID{ 0 })
			m_EditorCamera.OnEvent(e);

		// Game UI gets first refusal, but only while playing and only when the
		// viewport actually owns the pointer - otherwise clicking a panel would be
		// routed at a HUD sitting underneath it. UIEngine marks the event Handled
		// when RmlUi consumed it, so the editor shortcuts below then skip it.
		if (m_SceneState == SceneState::Play && (m_ViewportHovered || m_ViewportFocused))
			UIEngine::OnEvent(e);

		if (e.IsHandled())
			return;

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<KeyPressedEvent>(GE_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseButtonPressedEvent>(GE_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
	}

	bool EditorLayer::OnKeyPressed(KeyPressedEvent& e)
	{
		// Shortcuts
		if (e.GetRepeatCount() > 0)
			return false;

		// Ctrl+N/O/S and the undo shortcuts live in HandleShortcuts, above the widget-focus
		// layer. What stays here is deliberately viewport-gated: switching gizmo mode because
		// a name contains a W would be a regression, not a fix.
		bool control = Input::IsKeyPressed(Key::LeftControl) || Input::IsKeyPressed(Key::RightControl);
		switch (e.GetKeyCode())
		{
		// Gizmos
		case Key::U:
		{
			if (control)
				UIEngine::SetDebuggerVisible(!UIEngine::IsDebuggerVisible());
			break;
		}
		case Key::Q:
		{
			if (!ImGuizmo::IsUsing() && !Input::IsMouseButtonPressed(Mouse::ButtonRight))
				m_GizmoType = -1;
			break;
		}
		case Key::W:
		{
			if (!ImGuizmo::IsUsing() && !Input::IsMouseButtonPressed(Mouse::ButtonRight))
				m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
			break;
		}
		case Key::E:
		{
			if (!ImGuizmo::IsUsing() && !Input::IsMouseButtonPressed(Mouse::ButtonRight))
				m_GizmoType = ImGuizmo::OPERATION::ROTATE;
			break;
		}
		case Key::R:
		{
			if (!ImGuizmo::IsUsing() && !Input::IsMouseButtonPressed(Mouse::ButtonRight))
				m_GizmoType = ImGuizmo::OPERATION::SCALE;
			break;
		}
		}

		return false;
	}

	bool EditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent& e)
	{
		if (e.GetMouseButton() == Mouse::ButtonLeft)
		{
			if (m_ViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(Key::LeftAlt))
			{
				Entity hover = m_HoveredEntity;
				if (!(hover && m_SceneHierarchyPanel.IsLocked(hover)))
					m_SceneHierarchyPanel.SetSelectedEntity(hover);
			}
		}
		return false;
	}

	void EditorLayer::RetargetPanels()
	{
		m_SceneHierarchyPanel.SetContext(m_ActiveScene);

		// Nothing to record into during play: the active scene is a throwaway copy, and a
		// command naming its entities would be replayed against the editor scene on stop.
		m_SceneHierarchyPanel.SetUndoStack(m_SceneState == SceneState::Edit ? &m_UndoStack : nullptr);
	}

	void EditorLayer::NewScene()
	{
		m_EditorScene = CreateRef<Scene>();
		SetupDefaultEnvironment(m_EditorScene);
		m_ActiveScene = m_EditorScene;
		m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		m_EditorScenePath.clear();
		m_SceneState = SceneState::Edit;

		// Every UUID on the stack names an entity in a Scene object that no longer exists.
		m_UndoStack.Clear();
		RetargetPanels();
		m_SceneHierarchyPanel.ClearEditorViewState();
		m_ViewportCamera = UUID{ 0 };
	}

	void EditorLayer::SetupDefaultEnvironment(const Ref<Scene>& scene)
	{
		// Directional "sun": the light travels along the entity's -Z, so tilt it to come from above
		Entity sun = scene->CreateEntity("Sun");
		auto& sunTransform = sun.GetComponent<TransformComponent>();
		sunTransform.Rotation = { glm::radians(-50.0f), glm::radians(30.0f), 0.0f };
		scene->MarkChanged<TransformComponent>(sun);
		auto& dl = sun.AddComponent<DirectionalLightComponent>();
		dl.Color = { 1.0f, 0.98f, 0.92f };
		dl.Intensity = 3.0f;
		dl.CastShadows = true;

		// Environment / ambient (HDR IBL when the asset is present, procedural fallback otherwise)
		Entity sky = scene->CreateEntity("Sky Light");
		auto& skyLight = sky.AddComponent<SkyLightComponent>();
		skyLight.Environment = AssetRef<Environment>(
			AssetManager::ImportAsset("environments/studio_small_08_1k.hdr"));
		skyLight.Intensity = 1.0f;
	}

	void EditorLayer::OpenScene()
	{
		std::string filepath = FileDialogs::OpenFile("GanymedE Scene (*.ganymede)\0*.ganymede\0");
		if (!filepath.empty())
			OpenScene(filepath);
	}

	void EditorLayer::OpenScene(const std::filesystem::path& path)
	{
		if (m_SceneState != SceneState::Edit)
			OnSceneStop();

		if (path.extension().string() != ".ganymede")
		{
			GE_WARN("Could not load {0} - not a scene file", path.filename().string());
			return;
		}

		m_EditorScene = CreateRef<Scene>();
		m_ActiveScene = m_EditorScene;
		m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		m_SceneState = SceneState::Edit;

		m_UndoStack.Clear();
		RetargetPanels();
		m_SceneHierarchyPanel.ClearEditorViewState();
		m_ViewportCamera = UUID{ 0 };

		SceneSerializer serializer(m_ActiveScene);
		serializer.Deserialize(path.string());
		m_EditorScenePath = path;
	}

	void EditorLayer::SaveScene()
	{
		if (m_EditorScenePath.empty())
		{
			SaveSceneAs();
			return;
		}

		SaveSceneTo(m_EditorScenePath);
	}

	void EditorLayer::SaveSceneAs()
	{
		std::string filepath = FileDialogs::SaveFile("GanymedE Scene (*.ganymede)\0*.ganymede\0");
		if (!filepath.empty())
			SaveSceneTo(filepath);
	}

	void EditorLayer::SaveSceneTo(const std::filesystem::path& path)
	{
		// Always the editor scene: saving the play-mode copy would persist simulation state.
		SceneSerializer serializer(m_SceneState == SceneState::Edit ? m_ActiveScene : m_EditorScene);
		serializer.Serialize(path.string());

		m_EditorScenePath = path;
		m_UndoStack.MarkSaved();
	}

	void EditorLayer::OnScenePlay()
	{
		m_EditorScene = m_ActiveScene;
		m_ActiveScene = Scene::Copy(m_EditorScene);
		m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		m_ActiveScene->OnRuntimeStart();
		m_SceneState = SceneState::Play;
		RetargetPanels();
		m_SceneHierarchyPanel.SetSelectedEntity({});

		// Hard-coded for now. Making this a scene property is the obvious next
		// step, but it needs a UI-document asset type to hang off.
		UIEngine::LoadDocument("assets/ui/hud.rml");
	}

	void EditorLayer::OnSceneStop()
	{
		UIEngine::CloseAllDocuments();

		m_ActiveScene->OnRuntimeStop();
		m_ActiveScene = m_EditorScene;
		m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		m_SceneState = SceneState::Edit;

		// The stack survives the round trip: m_EditorScene is the same object it was before
		// play, so every UUID it records still resolves.
		RetargetPanels();
		m_SceneHierarchyPanel.SetSelectedEntity({});
	}
}
