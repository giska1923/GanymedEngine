#include "EditorLayer.h"
#include "AssetDragDrop.h"
#include "EditorFonts.h"
#include "EditorIcons.h"
#include "EditorInspector.h"

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

#include <ImGuizmo.h>
#include <bgfx/bgfx.h>

namespace GanymedE {

	extern const std::filesystem::path g_AssetPath;

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
		AssetManager::Shutdown();
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		GE_PROFILE_FUNCTION();

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
				m_EditorCamera.OnUpdate(ts);

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
		ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
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

		// DockSpace
		ImGuiIO& io = ImGui::GetIO();
		ImGuiStyle& style = ImGui::GetStyle();
		float minWinSizeX = style.WindowMinSize.x;
		style.WindowMinSize.x = 430.0f;
		if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
		{
			ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");

			// Build a default layout the first time (no imgui.ini entry for the dockspace yet)
			if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr)
			{
				ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
				ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

				ImGuiID dockMain = dockspace_id;
				ImGuiID dockToolbar = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Up, 0.06f, nullptr, &dockMain);
				ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.28f, nullptr, &dockMain);
				ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.24f, nullptr, &dockMain);
				ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.22f, nullptr, &dockMain);
				ImGuiID dockLeftBottom = ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Down, 0.5f, nullptr, &dockLeft);

				// The toolbar strip should not show a tab bar or be rearranged
				ImGuiDockNode* toolbarNode = ImGui::DockBuilderGetNode(dockToolbar);
				toolbarNode->SetLocalFlags(toolbarNode->LocalFlags | ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe);

				ImGui::DockBuilderDockWindow("##toolbar", dockToolbar);
				ImGui::DockBuilderDockWindow("Scene Hierarchy", dockLeft);
				ImGui::DockBuilderDockWindow("Properties", dockLeftBottom);
				ImGui::DockBuilderDockWindow("Viewport", dockMain);
				ImGui::DockBuilderDockWindow("Stats", dockRight);
				ImGui::DockBuilderDockWindow("Content Browser", dockBottom);
				ImGui::DockBuilderFinish(dockspace_id);
			}

			ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
		}

		style.WindowMinSize.x = minWinSizeX;

		if (ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				// Disabling fullscreen would allow the window to be moved to the front of other windows,
				// which we can't undo at the moment without finer window depth/z control.
				//ImGui::MenuItem("Fullscreen", NULL, &opt_fullscreen_persistant);

				if (ImGui::MenuItem("New", "Ctrl+N"))
					NewScene();

				if (ImGui::MenuItem("Open...", "Ctrl+O"))
					OpenScene();

				if (ImGui::MenuItem("Save", "Ctrl+S"))
					SaveScene();

				if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
					SaveSceneAs();

				if (ImGui::MenuItem("Exit")) Application::Get().Close();
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

				ImGui::EndMenu();
			}

			// Which scene is open, and whether it has unsaved changes.
			//
			// Dirtiness comes from the undo stack's *position* rather than a flag, so undoing
			// back to the saved state correctly clears the asterisk - the property an ad-hoc
			// dirty bool always gets wrong. Asset edits (.gmat fields, prefab Apply) do not
			// touch the scene stack and deliberately do not set it: they are asset dirt, and the
			// .gmat editor's own Save button is their indicator.
			ImGui::Separator();
			const std::string sceneName = m_EditorScenePath.empty()
				? std::string("Untitled") : m_EditorScenePath.filename().string();
			ImGui::TextUnformatted((sceneName + (m_UndoStack.IsDirtySinceSave() ? "*" : "")).c_str());

			ImGui::EndMenuBar();
		}

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

		ImGui::Separator();
		ImGui::Text("Physics Debug:");
		ImGui::Checkbox("Jolt Debug Draw", &m_PhysicsDebugDraw.Enabled);
		ImGui::BeginDisabled(!m_PhysicsDebugDraw.Enabled);
		ImGui::Checkbox("Wireframe Shapes", &m_PhysicsDebugDraw.Wireframe);
		ImGui::Checkbox("Bounding Boxes", &m_PhysicsDebugDraw.BoundingBoxes);
		ImGui::Checkbox("Velocities", &m_PhysicsDebugDraw.Velocities);
		ImGui::Checkbox("Center of Mass", &m_PhysicsDebugDraw.CenterOfMass);
		ImGui::Checkbox("Constraints", &m_PhysicsDebugDraw.Constraints);
		ImGui::EndDisabled();
		ImGui::TextDisabled("Visible during Play (uses Jolt body state)");

		ImGui::End();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0, 0 });
		ImGui::Begin("Viewport");

		m_ViewportFocused = ImGui::IsWindowFocused();
		m_ViewportHovered = ImGui::IsWindowHovered();
		Application::Get().GetImGuiLayer()->BlockEvents(!m_ViewportFocused && !m_ViewportHovered);

		ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
		m_ViewportSize = { viewportPanelSize.x, viewportPanelSize.y };

		// Screen-space bounds of the rendered image, used for mouse picking and the gizmo rect
		ImVec2 viewportScreenPos = ImGui::GetCursorScreenPos();
		m_ViewportBounds[0] = { viewportScreenPos.x, viewportScreenPos.y };
		m_ViewportBounds[1] = { viewportScreenPos.x + viewportPanelSize.x, viewportScreenPos.y + viewportPanelSize.y };

		// Same origin the picking code subtracts: RmlUi wants viewport-local pixels,
		// while engine mouse events arrive in window coordinates.
		UIEngine::SetViewportOrigin(m_ViewportBounds[0].x, m_ViewportBounds[0].y);

		uint32_t textureID = m_SceneRenderer->GetFinalImageRendererID();
		// Render targets are addressed bottom-up on OpenGL and top-down on
		// D3D/Vulkan/Metal, so the V axis has to follow the backend. The old
		// hard-coded {0,1}-{1,0} flip was a GL-only assumption.
		const bool flipV = bgfx::getCaps()->originBottomLeft;
		const ImVec2 uv0 = flipV ? ImVec2{ 0, 1 } : ImVec2{ 0, 0 };
		const ImVec2 uv1 = flipV ? ImVec2{ 1, 0 } : ImVec2{ 1, 1 };

		ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(textureID)),
			ImVec2{ m_ViewportSize.x, m_ViewportSize.y }, uv0, uv1);

		// Three types through one target. The initializer_list overload is mandatory here, not a
		// convenience: ImGui clears the drag payload as soon as one BeginDragDropTarget delivers
		// it, so calling AcceptAssetDrop once per type would let only the first type ever fire.
		if (auto drop = EditorUI::AcceptAssetDrop({ AssetType::Scene, AssetType::StaticMesh, AssetType::Prefab }))
		{
			std::filesystem::path fullPath = g_AssetPath / drop.Path;

			if (drop.Type == AssetType::Scene)
			{
				OpenScene(fullPath);
			}
			else if (drop.Type == AssetType::Prefab && m_SceneState == SceneState::Edit)
			{
				m_SceneHierarchyPanel.InstantiatePrefab(drop.Path);
			}
			else if (drop.Type == AssetType::StaticMesh && m_SceneState == SceneState::Edit)
			{
				Entity entity = MeshImporter::Instantiate(m_ActiveScene.get(), fullPath);
				if (entity)
					m_SceneHierarchyPanel.SetSelectedEntity(entity);
			}
		}

		// Gizmos
		Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
		if (selectedEntity && m_GizmoType != -1 && m_SceneState == SceneState::Edit)
		{
			ImGuizmo::SetOrthographic(false);
			ImGuizmo::SetDrawlist();

			ImGuizmo::SetRect(m_ViewportBounds[0].x, m_ViewportBounds[0].y,
				m_ViewportBounds[1].x - m_ViewportBounds[0].x, m_ViewportBounds[1].y - m_ViewportBounds[0].y);

			// Editor camera
			const glm::mat4& cameraProjection = m_EditorCamera.GetProjection();
			glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();

			// Entity transform (world space so parented entities gizmo correctly)
			auto& tc = selectedEntity.GetComponent<TransformComponent>();
			glm::mat4 transform = m_ActiveScene->GetWorldSpaceTransform(selectedEntity);

			// Snapping
			bool snap = Input::IsKeyPressed(Key::LeftControl);
			float snapValue = 0.5f; // Snap to 0.5m for translation/scale
			// Snap to 45 degrees for rotation
			if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
				snapValue = 45.0f;

			float snapValues[3] = { snapValue, snapValue, snapValue };

			ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection),
				(ImGuizmo::OPERATION)m_GizmoType, ImGuizmo::LOCAL, glm::value_ptr(transform),
				nullptr, snap ? snapValues : nullptr);

			if (ImGuizmo::IsUsing())
			{
				// Rising edge. This is the last moment the pre-drag transform still exists:
				// rotation below is accumulated as a delta against the current value, so one
				// frame later there is nothing left to reconstruct it from.
				if (!m_GizmoUsing)
				{
					m_GizmoUsing = true;
					m_GizmoEntity = selectedEntity.GetUUID();
					m_GizmoBefore = tc;
				}

				// Convert manipulated world transform back to local
				UUID parentID = selectedEntity.GetComponent<RelationshipComponent>().Parent;
				if (parentID != UUID{ 0 })
				{
					Entity parent = m_ActiveScene->FindEntityByUUID(parentID);
					if (parent)
						transform = glm::inverse(m_ActiveScene->GetWorldSpaceTransform(parent)) * transform;
				}

				glm::vec3 translation, rotation, scale;
				Math::DecomposeTransform(transform, translation, rotation, scale);

				// Apply the rotation as a delta to avoid gimbal-lock jumps from decompose
				glm::vec3 deltaRotation = rotation - tc.Rotation;
				tc.Translation = translation;
				tc.Rotation += deltaRotation;
				tc.Scale = scale;

				// Gizmo edits write the component directly, so the world-transform cache has to be
				// told; without this the entity would keep rendering at its pre-drag position.
				m_ActiveScene->MarkChanged<TransformComponent>(selectedEntity);
			}
		}

		// Falling edge, checked outside the gizmo block so a drag that ends with the selection
		// gone (or the gizmo hidden) still commits its one command.
		if (m_GizmoUsing && !ImGuizmo::IsUsing())
		{
			m_GizmoUsing = false;

			Entity dragged = m_EditorScene ? m_EditorScene->FindEntityByUUID(m_GizmoEntity) : Entity{};
			if (dragged && m_SceneState == SceneState::Edit && dragged.HasComponent<TransformComponent>())
			{
				m_UndoStack.Push(CreateScope<ComponentEditCommand<TransformComponent>>(
					"Gizmo Transform", m_GizmoEntity, m_GizmoBefore,
					dragged.GetComponent<TransformComponent>()));
			}
		}

		ImGui::End();
		ImGui::PopStyleVar();

		UI_Toolbar();

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
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 2));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		auto& colors = ImGui::GetStyle().Colors;
		const auto& buttonHovered = colors[ImGuiCol_ButtonHovered];
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(buttonHovered.x, buttonHovered.y, buttonHovered.z, 0.5f));
		const auto& buttonActive = colors[ImGuiCol_ButtonActive];
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(buttonActive.x, buttonActive.y, buttonActive.z, 0.5f));

		ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		float size = ImGui::GetWindowHeight() - 8.0f;
		if (size < 16.0f)
			size = 16.0f;
		const char* icon = m_SceneState == SceneState::Edit ? ICON_LC_PLAY : ICON_LC_SQUARE_STOP;
		ImGui::SetCursorPosX((ImGui::GetWindowSize().x - size) * 0.5f);
		ImGui::PushID("playstop");
		if (ImGui::Button(icon, ImVec2(size, size)))
		{
			if (m_SceneState == SceneState::Edit)
				OnScenePlay();
			else if (m_SceneState == SceneState::Play)
				OnSceneStop();
		}
		ImGui::PopID();

		ImGui::End();

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);
	}

	void EditorLayer::OnEvent(Event& e)
	{
		if (m_SceneState == SceneState::Edit)
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
				m_SceneHierarchyPanel.SetSelectedEntity(m_HoveredEntity);
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
