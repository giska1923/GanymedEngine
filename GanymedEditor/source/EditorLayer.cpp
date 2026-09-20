#include "EditorLayer.h"
#include "EditorPicking.h"
#include "EditorPrefabOverrides.h"
#include "EditorUndo.h"
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
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
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
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/Renderer.h"
#include "GanymedE/Renderer/Renderer3D.h"
#include "GanymedE/UI/UIEngine.h"
#include "GanymedE/Scene/SceneSingletons.h"
#include "GanymedE/Scene/SceneCamera.h"

#include <ImGuizmo.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace GanymedE {


	namespace {

		// Bump when the DockBuilder default tree changes. Existing imgui.ini otherwise keeps
		// the old splits — including the phase-1 6% toolbar node — and View → Reset Layout
		// is easy to miss on the first launch after a chrome change.
		constexpr int kDockLayoutVersion = 3;
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
			ImGui::DockBuilderDockWindow("Map", dockRight);
			ImGui::DockBuilderDockWindow("Content Browser", dockBottom);
			ImGui::DockBuilderFinish(dockspaceId);
		}

		// Is any ancestor of `entity` also in `selection`?
		//
		// A group gizmo drag applies one world-space delta per selected entity, and a child's
		// world transform already carries its parent's. Moving both would apply the delta twice
		// - the child drifts away at double speed - so a selected entity whose ancestor is also
		// selected is left to move with its parent. Selecting a whole hierarchy and dragging it
		// is the obvious way to hit this, so it is the default case rather than an edge one.
		bool IsDescendantOfSelection(Scene& scene, Entity entity, const std::vector<Entity>& selection)
		{
			if (!entity.HasComponent<RelationshipComponent>())
				return false;

			UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
			while (parentID != UUID{ 0 })
			{
				Entity parent = scene.FindEntityByUUID(parentID);
				if (!parent)
					return false;

				for (Entity selected : selection)
				{
					if (selected == parent)
						return true;
				}

				if (!parent.HasComponent<RelationshipComponent>())
					return false;

				parentID = parent.GetComponent<RelationshipComponent>().Parent;
			}

			return false;
		}

		// Move one entity by a world-space delta, writing the result back as a local transform.
		//
		// Factored out of the gizmo block so the arithmetic can be exercised on its own: a wrong
		// delta compiles perfectly and simply puts things in the wrong place.
		void ApplyWorldDelta(Scene& scene, Entity entity, const glm::mat4& worldDelta)
		{
			if (!entity || !entity.HasComponent<TransformComponent>())
				return;

			glm::mat4 world = worldDelta * scene.GetWorldSpaceTransform(entity);

			const UUID parentID = entity.HasComponent<RelationshipComponent>()
				? entity.GetComponent<RelationshipComponent>().Parent : UUID{ 0 };

			if (parentID != UUID{ 0 })
			{
				Entity parent = scene.FindEntityByUUID(parentID);
				if (parent)
					world = glm::inverse(scene.GetWorldSpaceTransform(parent)) * world;
			}

			glm::vec3 translation, rotation, scale;
			Math::DecomposeTransform(world, translation, rotation, scale);

			auto& tc = entity.GetComponent<TransformComponent>();

			// Rotation as a delta against the current value, matching what the primary does -
			// DecomposeTransform picks one of several equivalent Euler triples, and jumping
			// straight to it makes a continuous drag flip.
			tc.Rotation += rotation - tc.Rotation;
			tc.Translation = translation;
			tc.Scale = scale;

			scene.MarkChanged<TransformComponent>(entity);
		}

		glm::vec3 Quantize(const glm::vec3& p, float step)
		{
			if (step <= 1.0e-6f)
				return p;
			return glm::round(p / step) * step;
		}

		bool IntersectWorkPlane(const Math::Ray& ray, float height, glm::vec3& outPoint)
		{
			if (std::abs(ray.Direction.y) < 1.0e-8f)
				return false;

			const float t = (height - ray.Origin.y) / ray.Direction.y;
			if (t < 0.0f || t > ray.MaxDistance)
				return false;

			outPoint = ray.Origin + ray.Direction * t;
			return true;
		}

		glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to)
		{
			const glm::vec3 f = glm::normalize(from);
			const glm::vec3 t = glm::normalize(to);
			const float d = glm::dot(f, t);
			if (d > 0.9999f)
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			if (d < -0.9999f)
			{
				glm::vec3 axis = glm::cross(f, glm::vec3(1.0f, 0.0f, 0.0f));
				if (glm::dot(axis, axis) < 1.0e-6f)
					axis = glm::cross(f, glm::vec3(0.0f, 0.0f, 1.0f));
				return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
			}

			const glm::vec3 axis = glm::cross(f, t);
			const float s = std::sqrt((1.0f + d) * 2.0f);
			glm::quat q;
			q.x = axis.x / s;
			q.y = axis.y / s;
			q.z = axis.z / s;
			q.w = s * 0.5f;
			return glm::normalize(q);
		}

		glm::mat4 EulerRotationMatrix(const glm::vec3& euler)
		{
			return glm::rotate(glm::mat4(1.0f), euler.x, { 1.0f, 0.0f, 0.0f })
				* glm::rotate(glm::mat4(1.0f), euler.y, { 0.0f, 1.0f, 0.0f })
				* glm::rotate(glm::mat4(1.0f), euler.z, { 0.0f, 0.0f, 1.0f });
		}

		void AccumulateMeshBounds(Scene& scene, Entity entity, const glm::mat4& invRoot,
			AABB& bounds, bool& any)
		{
			if (entity.HasComponent<StaticMeshComponent>())
			{
				const Ref<Mesh>& mesh = entity.GetComponent<StaticMeshComponent>().Mesh.Get();
				if (mesh)
				{
					const AABB local = mesh->GetBounds().Transformed(
						invRoot * scene.GetWorldSpaceTransform(entity));
					if (!any)
					{
						bounds = local;
						any = true;
					}
					else
					{
						bounds.Grow(local.Min);
						bounds.Grow(local.Max);
					}
				}
			}

			if (!entity.HasComponent<RelationshipComponent>())
				return;

			for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
			{
				Entity child = scene.FindEntityByUUID(childID);
				if (child)
					AccumulateMeshBounds(scene, child, invRoot, bounds, any);
			}
		}

		Entity FindScatterGroup(Scene& scene, AssetHandle source)
		{
			if (!IsAssetHandleValid(source))
				return {};

			auto view = scene.Reg().view<ScatterGroupComponent>();
			for (auto handle : view)
			{
				if (view.get<ScatterGroupComponent>(handle).Source == source)
					return Entity{ handle, &scene };
			}
			return {};
		}

		AssetHandle MeshHandleOf(Entity entity)
		{
			if (entity && entity.HasComponent<StaticMeshComponent>())
				return entity.GetComponent<StaticMeshComponent>().Mesh.Handle();
			return InvalidAssetHandle;
		}

		void CollectSubtreeUUIDs(Scene& scene, Entity root, std::unordered_set<UUID>& out)
		{
			std::vector<Entity> subtree;
			std::unordered_set<UUID> visited;
			scene.CollectSubtree(root, subtree, visited);
			for (Entity entity : subtree)
				out.insert(entity.GetUUID());
		}

		uint64_t ScatterCellKey(int x, int y, int z)
		{
			const auto pack = [](int v) -> uint64_t
			{
				return (uint64_t)((uint32_t)(v + 0x100000) & 0x1FFFFFu);
			};
			return pack(x) | (pack(y) << 21) | (pack(z) << 42);
		}

		void TangentAxes(const glm::vec3& normal, glm::vec3& tangent, glm::vec3& bitangent)
		{
			const glm::vec3 n = glm::normalize(normal);
			tangent = (std::abs(n.y) < 0.99f)
				? glm::normalize(glm::cross(n, glm::vec3(0.0f, 1.0f, 0.0f)))
				: glm::normalize(glm::cross(n, glm::vec3(1.0f, 0.0f, 0.0f)));
			bitangent = glm::cross(n, tangent);
		}

		constexpr int kScatterMaxDropRays = 12;
		constexpr size_t kScatterSceneWarn = 1500;
		constexpr float kScatterDropLift = 8.0f;

	}

	EditorLayer::EditorLayer()
		: Layer("EditorLayer"), m_GizmoType(ImGuizmo::OPERATION::TRANSLATE)
	{
	}

	namespace {

		// Parsed here rather than in an editor-wide settings object because it is the only
		// command-line switch the editor has, and one switch does not need a parser.
		std::filesystem::path ProjectRootFromCommandLine()
		{
			const auto& args = Application::GetCommandLineArgs();
			const std::string prefix = "--project=";
			for (int i = 1; i < args.Count; i++)
			{
				if (!args.Args[i])
					continue;

				const std::string arg = args.Args[i];
				if (arg.rfind(prefix, 0) != 0)
					continue;

				std::filesystem::path root = arg.substr(prefix.size());
				if (root.empty())
				{
					GE_WARN("--project= with no path; opening the editor's own assets/");
					break;
				}

				// A project that is not there yields an empty content browser and a scan that
				// indexes nothing, which looks like data loss rather than a typo. Say so.
				std::error_code ec;
				if (!std::filesystem::is_directory(root, ec))
					GE_WARN("--project='{0}' is not a directory; opening it anyway, but the "
						"asset scan will find nothing", root.generic_string());

				GE_INFO("Opening project '{0}'", root.generic_string());
				return root;
			}

			return "assets";
		}

	}

	void EditorLayer::OnAttach()
	{
		GE_PROFILE_FUNCTION();

		// --project=<path> opens a project other than the editor's own assets/ tree. Only the
		// project moves: the editor's fonts, its checkerboard and its HUD document are loaded
		// relative to the working directory because they ship with the editor, not with the
		// content. Without the switch this is exactly what it always was.
		AssetManager::Init(/*writableAssets=*/true, ProjectRootFromCommandLine());

		// After ImGuiLayer::OnAttach (Application's constructor pushed that overlay
		// first): the context exists, the default atlas is already uploaded, and
		// Clear() here makes NewFrame rebuild it with Inter + Lucide.
		EditorUI::EditorFonts::Load();
		EditorUI::ApplyTheme(EditorUI::MakeDarkTheme());
		EditorUI::InitTitleBar();
		RegisterDockLayoutSettingsHandler();

		// A `.gprefab` rewritten on disk - by an external editor, a git checkout, a branch switch
		// - invalidates the prefab-override template cache, which is the only thing in the editor
		// keyed on a prefab's contents. The watcher has always detected this; nothing forwarded
		// it, because Prefab has no asset manager to evict from.
		//
		// Drops every template rather than the one that changed. They rebuild lazily on the next
		// query, one Instantiate each, and only for prefabs an instance is actually being
		// inspected against - which is a smaller cost than keeping a second index to be precise.
		AssetManager::AddAssetChangedListener([](AssetHandle, AssetType type)
		{
			if (type != AssetType::Prefab)
				return false;

			EditorUI::InvalidatePrefabTemplates();
			return true;
		});

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
		m_MapPanel.SetPlaceHandler([this](AssetHandle handle, AssetType type)
		{
			if (m_SceneState == SceneState::Edit)
				BeginPlacement(handle, type);
		});
		m_MapPanel.SetMarkerHandler([this](const std::string& kind, const glm::vec4& color, float size)
		{
			if (m_SceneState == SceneState::Edit)
				BeginMarkerPlacement(kind, color, size);
		});

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
				m_ActiveScene->GetSingleton<PhysicsSettings>().ShowColliderGizmos =
					m_ShowColliderGizmos;
				m_ActiveScene->GetSingleton<PhysicsSettings>().ShowMarkers = m_ShowMarkers;

				// Raycast and write the placement transform before TransformSystem so the
				// preview renders this frame at the hover pose, not last frame's.
				UpdateSurfaceRaycast();
				if (IsPlacing())
					ApplyPlacementTransform();
				TickScatter();

				EditorBoundsOverlay& overlay = m_ActiveScene->GetSingleton<EditorBoundsOverlay>();
				m_MapPanel.FillOverlay(overlay);
				overlay.Spheres.clear();
				if (IsScattering() && m_SurfaceHit.IsHit())
				{
					const bool erase = m_ScatterStroke.Active
						? m_ScatterStroke.Erase
						: (Input::IsKeyPressed(Key::LeftShift)
							|| Input::IsKeyPressed(Key::RightShift));
					overlay.Spheres.push_back({
						m_SurfaceHit.Point,
						m_MapPanel.Scatter().Radius,
						erase
							? glm::vec4(0.95f, 0.40f, 0.35f, 1.0f)
							: glm::vec4(0.35f, 0.85f, 1.0f, 1.0f)
					});
				}

				m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
				break;
			}
			case SceneState::Play:
			{
				// Fall back to the editor camera when the scene has no primary Camera
				m_EditorCamera.OnUpdate(ts);

				PhysicsSettings& physicsSettings = m_ActiveScene->GetSingleton<PhysicsSettings>();
				physicsSettings.DebugDraw = m_PhysicsDebugDraw;
				// Editor-only opt-in: the engine defaults these off so a shipped game never
				// draws authored collider wireframes or marker gizmos. Edit and Play both push
				// the Visualizers / Icons checkboxes every frame because Scene::Copy does not
				// carry singletons.
				physicsSettings.ShowColliderGizmos = m_ShowColliderGizmos;
				physicsSettings.ShowMarkers = m_ShowMarkers;

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

		// Mouse picking: GPU entity-ID (click-select / hover highlight) plus the CPU
		// surface ray (world point + normal, this frame). The GPU path is async and
		// has no depth; the CPU path is what placement will use.
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

		const bool pointerInViewport = mouseX >= 0 && mouseY >= 0
			&& mouseX < (int)viewportSize.x && mouseY < (int)viewportSize.y;

		// Picking is asynchronous now: queue this frame's pick and take whatever
		// has landed. The result trails the cursor by a frame or two, which is
		// invisible for hover highlighting.
		if (pointerInViewport)
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

		if (m_SceneState != SceneState::Edit)
		{
			m_SurfaceHit = {};
			m_SurfaceRaycastMs = 0.0f;
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
		m_MapPanel.OnImGuiRender(m_SnapSettings, m_SceneState == SceneState::Edit, IsPlacing(),
			m_ActiveScene.get(), m_SceneState == SceneState::Edit ? &m_UndoStack : nullptr,
			&m_SceneHierarchyPanel,
			m_ViewportCamera == UUID{ 0 } ? &m_EditorCamera : nullptr,
			m_PlacePreview);

		ImGui::Begin("Stats");

		std::string hoveredEntityName = "None";
		if (m_HoveredEntity)
			hoveredEntityName = m_HoveredEntity.GetComponent<TagComponent>().Tag;
		ImGui::Text("Hovered Entity: %s", hoveredEntityName.c_str());

		if (m_SurfaceHit.FromWorkPlane)
		{
			ImGui::Text("Surface: work plane  (%.3f, %.3f, %.3f)  n=(%.2f, %.2f, %.2f)  %.3f ms",
				m_SurfaceHit.Point.x, m_SurfaceHit.Point.y, m_SurfaceHit.Point.z,
				m_SurfaceHit.Normal.x, m_SurfaceHit.Normal.y, m_SurfaceHit.Normal.z,
				m_SurfaceRaycastMs);
		}
		else if (m_SurfaceHit.Hit)
		{
			ImGui::Text("Surface: %s  (%.3f, %.3f, %.3f)  n=(%.2f, %.2f, %.2f)  %.3f ms",
				m_SurfaceHit.Hit.GetName().c_str(),
				m_SurfaceHit.Point.x, m_SurfaceHit.Point.y, m_SurfaceHit.Point.z,
				m_SurfaceHit.Normal.x, m_SurfaceHit.Normal.y, m_SurfaceHit.Normal.z,
				m_SurfaceRaycastMs);
		}
		else
		{
			ImGui::Text("Surface: none  %.3f ms", m_SurfaceRaycastMs);
		}

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

		// The other throttle, and a different one: `deferred` above is work that finished
		// parsing and did not fit the frame's Apply budget, `refused` here is work that was
		// never started because the in-flight cap was full - so its decoded bytes were never
		// held at all. A steady stream of refusals during a burst is the cap doing its job.
		ImGui::Text("Parses: %u in flight / %u max, %u load(s) refused",
			apply.InFlight, (uint32_t)kMaxParsesInFlight, apply.LoadsDeferred);

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

		if (IsPlacing())
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
				CancelPlacement();

			if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
			{
				m_PlaceYaw -= m_SnapSettings.Rotate;
				ApplyPlacementTransform();
			}
			if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
			{
				m_PlaceYaw += m_SnapSettings.Rotate;
				ApplyPlacementTransform();
			}
		}

		if (IsScattering() && ImGui::IsKeyPressed(ImGuiKey_Escape))
			CancelScatterMode();

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
			m_UndoStack.Undo(*m_EditorScene);

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)
			|| ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
		{
			m_UndoStack.Redo(*m_EditorScene);
		}

		if (IsPlacing())
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Delete))
				CancelPlacement();
			return;
		}

		if (IsScattering())
			return;

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
			const float rightWidth = 24.0f + gap + 24.0f + gap + 24.0f + gap + 9.0f + gap + 72.0f;
			const float rightX = row.x + availX - rightWidth;
			if (rightX > ImGui::GetCursorPosX() + gap)
				ImGui::SetCursorPos(ImVec2(rightX, row.y));
			else
				ImGui::SameLine();

			if (IconButton(ICON_LC_MAGNET, m_SnapSettings.Enabled
				? "Snap settings (Ctrl disables)" : "Snap settings (Ctrl enables)",
				m_SnapSettings.Enabled))
			{
				ImGui::OpenPopup("##SnapSettings");
			}
			if (ImGui::BeginPopup("##SnapSettings"))
			{
				DrawMapSnapControls(m_SnapSettings);
				ImGui::EndPopup();
			}

			ImGui::SameLine();
			if (IconButton(ICON_LC_BOXES, "Visualizers",
				m_ShowColliderGizmos || m_PhysicsDebugDraw.Enabled))
				ImGui::OpenPopup("##Visualizers");
			if (ImGui::BeginPopup("##Visualizers"))
			{
				ImGui::Checkbox("Collider gizmos", &m_ShowColliderGizmos);
				ImGui::SetItemTooltip("Authored box/sphere/capsule wireframes. Edit and Play.");
				ImGui::Separator();
				ImGui::Checkbox("Jolt Debug Draw", &m_PhysicsDebugDraw.Enabled);
				ImGui::BeginDisabled(!m_PhysicsDebugDraw.Enabled);
				ImGui::Checkbox("Wireframe Shapes", &m_PhysicsDebugDraw.Wireframe);
				ImGui::Checkbox("Bounding Boxes", &m_PhysicsDebugDraw.BoundingBoxes);
				ImGui::Checkbox("Velocities", &m_PhysicsDebugDraw.Velocities);
				ImGui::Checkbox("Center of Mass", &m_PhysicsDebugDraw.CenterOfMass);
				ImGui::Checkbox("Constraints", &m_PhysicsDebugDraw.Constraints);
				ImGui::EndDisabled();
				ImGui::TextDisabled("Jolt debug draw is Play-only (live body state).");
				ImGui::EndPopup();
			}

			ImGui::SameLine();
			if (IconButton(ICON_LC_MAP_PIN, "Icons (marker gizmos)", m_ShowMarkers))
				m_ShowMarkers = !m_ShowMarkers;

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
			std::filesystem::path fullPath = GetAssetRoot() / drop.Path;

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
		if (gizmoEntity && m_GizmoType != -1 && editing && !IsPlacing() && !IsScattering())
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

			bool snap = SnapActive();
			float snapValue = m_SnapSettings.Translate;
			if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
				snapValue = m_SnapSettings.Rotate;
			else if (m_GizmoType == ImGuizmo::OPERATION::SCALE)
				snapValue = m_SnapSettings.Scale;
			float snapValues[3] = { snapValue, snapValue, snapValue };

			// Manipulate rewrites `transform` in place, so the pre-drag world matrix has to be
			// kept: it is what the rest of the selection's delta is measured against.
			const glm::mat4 worldBefore = transform;

			ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection),
				(ImGuizmo::OPERATION)m_GizmoType,
				m_GizmoWorldSpace ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
				glm::value_ptr(transform),
				nullptr, snap ? snapValues : nullptr);

			if (ImGuizmo::IsUsing())
			{
				const std::vector<Entity>& selection = m_SceneHierarchyPanel.GetSelection();

				// Rising edge. This is the last moment the pre-drag transforms still exist:
				// rotation below is accumulated as a delta against the current value, so one
				// frame later there is nothing left to reconstruct them from.
				if (!m_GizmoUsing)
				{
					m_GizmoUsing = true;
					m_GizmoBefore.clear();
					m_GizmoBefore.emplace_back(gizmoEntity.GetUUID(), tc);

					for (Entity other : selection)
					{
						if (other == gizmoEntity || !other.HasComponent<TransformComponent>())
							continue;

						// Skip anything that already moves because an ancestor of it is selected
						// too - it would otherwise take the delta twice, once from its parent's
						// transform and once from its own.
						if (IsDescendantOfSelection(*m_ActiveScene, other, selection))
							continue;

						m_GizmoBefore.emplace_back(other.GetUUID(),
							other.GetComponent<TransformComponent>());
					}
				}

				// The world-space change the gizmo just made. Applied to every other entity in
				// the selection, which is what makes a group drag rotate and scale about the
				// primary rather than each object about its own origin.
				const glm::mat4 worldDelta = transform * glm::inverse(worldBefore);

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

				// The rest of the selection. Driven from m_GizmoBefore rather than from the live
				// selection, so an entity that leaves the selection mid-drag is not left half
				// moved, and the ancestor filter above is applied once rather than per frame.
				for (std::size_t i = 1; i < m_GizmoBefore.size(); i++)
				{
					ApplyWorldDelta(*m_ActiveScene,
						m_ActiveScene->FindEntityByUUID(m_GizmoBefore[i].first), worldDelta);
				}
			}
		}

		// Falling edge, checked outside the gizmo block so a drag that ends with the selection
		// gone (or the gizmo hidden) still commits its one command.
		if (m_GizmoUsing && !ImGuizmo::IsUsing())
		{
			m_GizmoUsing = false;

			if (m_EditorScene && editing)
			{
				std::vector<Scope<EditorCommand>> moved;
				moved.reserve(m_GizmoBefore.size());

				for (const auto& entry : m_GizmoBefore)
				{
					Entity dragged = m_EditorScene->FindEntityByUUID(entry.first);
					if (!dragged || !dragged.HasComponent<TransformComponent>())
						continue;

					moved.push_back(CreateScope<ComponentEditCommand<TransformComponent>>(
						"Gizmo Transform", entry.first, entry.second,
						dragged.GetComponent<TransformComponent>()));
				}

				// One drag is one undo entry, the same rule the inspector's multi-edit follows.
				if (moved.size() == 1)
				{
					m_UndoStack.Push(std::move(moved.front()));
				}
				else if (!moved.empty())
				{
					m_UndoStack.Push(CreateScope<CompositeCommand>(
						"Gizmo Transform (" + std::to_string(moved.size()) + " entities)",
						std::move(moved)));
				}
			}

			m_GizmoBefore.clear();
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
		if (e.GetMouseButton() == Mouse::ButtonRight && m_ViewportHovered)
		{
			if (IsPlacing())
			{
				CancelPlacement();
				return false;
			}
			if (IsScattering())
			{
				CancelScatterMode();
				return false;
			}
		}

		if (e.GetMouseButton() == Mouse::ButtonLeft)
		{
			if (IsPlacing())
			{
				if (m_ViewportHovered && !ImGuizmo::IsOver())
				{
					ApplyPlacementTransform();
					const bool chain = Input::IsKeyPressed(Key::LeftShift)
						|| Input::IsKeyPressed(Key::RightShift);
					CommitPlacement(chain);
				}
				return false;
			}

			if (m_MapPanel.IsPaintArmed() && m_SceneState == SceneState::Edit)
			{
				if (m_ViewportHovered && !ImGuizmo::IsOver())
				{
					const bool erase = Input::IsKeyPressed(Key::LeftShift)
						|| Input::IsKeyPressed(Key::RightShift);
					BeginScatterStroke(erase);
				}
				return false;
			}

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
		// The prefab-override diff caches a template per source handle, and its own header says
		// to drop them when the scene changes. Nothing was calling it - the function had no call
		// sites at all - so a `.gprefab` rewritten between two scene loads went on being diffed
		// against the version cached at first sight. This is the choke point every scene change
		// goes through, including play and stop.
		EditorUI::InvalidatePrefabTemplates();

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);

		// Nothing to record into during play: the active scene is a throwaway copy, and a
		// command naming its entities would be replayed against the editor scene on stop.
		m_SceneHierarchyPanel.SetUndoStack(m_SceneState == SceneState::Edit ? &m_UndoStack : nullptr);
	}

	void EditorLayer::NewScene()
	{
		CancelScatterMode();
		CancelPlacement();
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
		//
		// The presence check is the point: this path is project-relative, and it was written when
		// there was only ever one project that happened to ship that file. Any other project got
		// `Failed to load HDR environment` at every new scene. An invalid handle is already the
		// procedural fallback the comment above promises, so the fix is to not ask for a file
		// that is not there rather than to ship the file everywhere.
		Entity sky = scene->CreateEntity("Sky Light");
		auto& skyLight = sky.AddComponent<SkyLightComponent>();

		constexpr const char* kDefaultEnvironment = "environments/studio_small_08_1k.hdr";
		std::error_code ec;
		if (std::filesystem::exists(GetAssetRoot() / kDefaultEnvironment, ec))
			skyLight.Environment = AssetRef<Environment>(AssetManager::ImportAsset(kDefaultEnvironment));

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

		CancelScatterMode();
		CancelPlacement();

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
		CancelScatterMode();
		CancelPlacement();

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

	bool EditorLayer::SnapActive() const
	{
		const bool ctrl = Input::IsKeyPressed(Key::LeftControl)
			|| Input::IsKeyPressed(Key::RightControl);
		return m_SnapSettings.Enabled != ctrl;
	}

	void EditorLayer::CancelPlacement()
	{
		if (m_PlacePreview != UUID{ 0 } && m_ActiveScene)
		{
			Entity preview = m_ActiveScene->FindEntityByUUID(m_PlacePreview);
			if (preview)
			{
				if (m_SceneHierarchyPanel.GetSelectedEntity()
					&& m_SceneHierarchyPanel.GetSelectedEntity().GetUUID() == m_PlacePreview)
				{
					m_SceneHierarchyPanel.SetSelectedEntity({});
				}

				std::vector<EntitySnapshot> snapshots;
				CaptureSubtree(*m_ActiveScene, preview, snapshots);
				RemoveSubtree(*m_ActiveScene, snapshots);
			}
		}

		m_PlacePreview = UUID{ 0 };
		m_PlaceHandle = InvalidAssetHandle;
		m_PlaceType = AssetType::None;
		m_PlaceHasTarget = false;
		m_PlaceHasBounds = false;
		m_PlaceMarkerKind.clear();
	}

	void EditorLayer::BeginPlacement(AssetHandle handle, AssetType type)
	{
		if (!m_ActiveScene || m_SceneState != SceneState::Edit)
			return;
		if (type != AssetType::Prefab && type != AssetType::StaticMesh)
			return;

		CancelScatterMode();

		const float keepYaw = (handle == m_PlaceHandle && type == m_PlaceType) ? m_PlaceYaw : 0.0f;
		CancelPlacement();
		m_PlaceYaw = keepYaw;

		const AssetMetadata* metadata = AssetManager::GetMetadata(handle);
		if (!metadata)
			return;

		Entity root;
		if (type == AssetType::Prefab)
			root = m_SceneHierarchyPanel.InstantiatePrefab(metadata->FilePath, false);
		else
			root = MeshImporter::Instantiate(m_ActiveScene.get(), GetAssetRoot() / metadata->FilePath);

		if (!root)
			return;

		m_PlaceHandle = handle;
		m_PlaceType = type;
		m_PlacePreview = root.GetUUID();
		m_PlaceBaseEuler = root.GetComponent<TransformComponent>().Rotation;
		m_PlaceBaseScale = root.GetComponent<TransformComponent>().Scale;
		RefreshPlaceBounds(root);
		m_SceneHierarchyPanel.SetSelectedEntity(root);
		ApplyPlacementTransform();
	}

	void EditorLayer::BeginMarkerPlacement(const std::string& kind, const glm::vec4& color, float size)
	{
		if (!m_ActiveScene || m_SceneState != SceneState::Edit)
			return;
		if (kind.empty())
			return;

		CancelScatterMode();

		const float keepYaw = (kind == m_PlaceMarkerKind) ? m_PlaceYaw : 0.0f;
		CancelPlacement();
		m_PlaceYaw = keepYaw;

		Entity root = m_ActiveScene->CreateEntity(kind);
		auto& marker = root.AddComponent<MarkerComponent>();
		marker.Kind = kind;
		marker.Color = color;
		marker.Size = glm::clamp(size, 0.05f, 20.0f);
		marker.DrawForward = true;

		m_PlaceMarkerKind = kind;
		m_PlaceMarkerColor = marker.Color;
		m_PlaceMarkerSize = marker.Size;
		m_PlaceHandle = InvalidAssetHandle;
		m_PlaceType = AssetType::None;
		m_PlacePreview = root.GetUUID();
		m_PlaceBaseEuler = root.GetComponent<TransformComponent>().Rotation;
		m_PlaceBaseScale = root.GetComponent<TransformComponent>().Scale;
		m_PlaceHasBounds = false;
		m_PlaceBounds = {};
		m_SceneHierarchyPanel.SetSelectedEntity(root);
		ApplyPlacementTransform();
	}

	void EditorLayer::UpdateSurfaceRaycast()
	{
		m_SurfaceHit = {};
		m_SurfaceRaycastMs = 0.0f;

		auto [mx, my] = ImGui::GetMousePos();
		mx -= m_ViewportBounds[0].x;
		my -= m_ViewportBounds[0].y;
		const glm::vec2 viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];
		const float localX = mx;
		const float localY = my;

		if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
			return;

		const int mouseX = (int)mx;
		const int mouseY = (int)my;
		const bool pointerInViewport = mouseX >= 0 && mouseY >= 0
			&& mouseX < (int)viewportSize.x && mouseY < (int)viewportSize.y;
		if (!pointerInViewport)
			return;

		glm::mat4 viewProjection = m_EditorCamera.GetViewProjection();
		if (m_ViewportCamera != UUID{ 0 })
		{
			Entity preview = m_ActiveScene->FindEntityByUUID(m_ViewportCamera);
			if (preview && preview.HasComponent<CameraComponent>())
			{
				const auto& cc = preview.GetComponent<CameraComponent>();
				const glm::mat4 view = glm::inverse(m_ActiveScene->GetWorldSpaceTransform(preview));
				viewProjection = cc.Camera.GetProjection() * view;
			}
		}

		const glm::vec2 ndc = {
			(localX / viewportSize.x) * 2.0f - 1.0f,
			1.0f - (localY / viewportSize.y) * 2.0f
		};
		const float nearClipZ = Projection::HomogeneousDepth() ? -1.0f : 0.0f;
		m_EditRay = Math::ScreenPointToRay(glm::inverse(viewProjection), ndc, nearClipZ, 1.0f);

		RaycastFilter filter;
		filter.HiddenEntities = &m_SceneHierarchyPanel.HiddenEntities();
		filter.Exclude = m_PlacePreview;
		if (IsScattering())
		{
			RebuildScatterExclude();
			filter.ExcludeSet = &m_ScatterStroke.Exclude;
		}
		filter.GridHeight = m_SnapSettings.GridHeight;

		const auto t0 = std::chrono::high_resolution_clock::now();
		m_SurfaceHit = RaycastScene(m_ActiveScene, m_EditRay, filter);
		const auto t1 = std::chrono::high_resolution_clock::now();
		m_SurfaceRaycastMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
	}

	void EditorLayer::RefreshPlaceBounds(Entity root)
	{
		m_PlaceHasBounds = false;
		m_PlaceBounds = {};
		if (!root || !m_ActiveScene)
			return;

		const glm::mat4 invRoot = glm::inverse(m_ActiveScene->GetWorldSpaceTransform(root));
		AccumulateMeshBounds(*m_ActiveScene, root, invRoot, m_PlaceBounds, m_PlaceHasBounds);
	}

	void EditorLayer::ApplyPlacementTransform()
	{
		if (!IsPlacing() || !m_ActiveScene)
			return;

		Entity preview = m_ActiveScene->FindEntityByUUID(m_PlacePreview);
		if (!preview)
		{
			m_PlacePreview = UUID{ 0 };
			m_PlaceType = AssetType::None;
			m_PlaceHandle = InvalidAssetHandle;
			m_PlaceMarkerKind.clear();
			return;
		}

		if (!preview.HasComponent<TransformComponent>())
			return;

		if (!m_PlaceHasBounds && m_PlaceMarkerKind.empty())
			RefreshPlaceBounds(preview);

		const bool alt = Input::IsKeyPressed(Key::LeftAlt) || Input::IsKeyPressed(Key::RightAlt);
		const bool snap = SnapActive() && !alt;

		glm::vec3 point;
		glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
		bool havePoint = false;

		if (!m_SnapSettings.SnapToSurface)
		{
			havePoint = IntersectWorkPlane(m_EditRay, m_SnapSettings.GridHeight, point);
			normal = { 0.0f, 1.0f, 0.0f };
		}
		else if (m_SurfaceHit.IsHit())
		{
			point = m_SurfaceHit.Point;
			normal = m_SurfaceHit.Normal;
			havePoint = true;
		}

		if (!havePoint)
			return;

		if (snap)
			point = Quantize(point, m_SnapSettings.Translate);

		glm::quat align(1.0f, 0.0f, 0.0f, 0.0f);
		if (m_SnapSettings.AlignToNormal)
			align = RotationBetween(glm::vec3(0.0f, 1.0f, 0.0f), normal);

		const glm::quat yaw = glm::angleAxis(glm::radians(m_PlaceYaw), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 rotation = glm::mat4_cast(align * yaw) * EulerRotationMatrix(m_PlaceBaseEuler);

		glm::vec3 origin = point;
		if (m_SnapSettings.SitOnBounds && m_PlaceHasBounds && m_PlaceMarkerKind.empty())
		{
			const glm::vec3 sitLocal(0.0f, -m_PlaceBounds.Min.y * m_PlaceBaseScale.y, 0.0f);
			origin += glm::vec3(rotation * glm::vec4(sitLocal, 0.0f));
		}

		glm::mat4 world = glm::translate(glm::mat4(1.0f), origin)
			* rotation
			* glm::scale(glm::mat4(1.0f), m_PlaceBaseScale);

		const UUID parentID = preview.HasComponent<RelationshipComponent>()
			? preview.GetComponent<RelationshipComponent>().Parent : UUID{ 0 };
		if (parentID != UUID{ 0 })
		{
			Entity parent = m_ActiveScene->FindEntityByUUID(parentID);
			if (parent)
				world = glm::inverse(m_ActiveScene->GetWorldSpaceTransform(parent)) * world;
		}

		glm::vec3 translation, euler, scale;
		if (!Math::DecomposeTransform(world, translation, euler, scale))
			return;

		auto& tc = preview.GetComponent<TransformComponent>();
		tc.Translation = translation;
		tc.Rotation = euler;
		tc.Scale = m_PlaceBaseScale;
		m_ActiveScene->MarkChanged<TransformComponent>(preview);
		m_PlaceHasTarget = true;
	}

	void EditorLayer::CommitPlacement(bool chain)
	{
		if (!m_ActiveScene || !m_PlaceHasTarget)
			return;

		Entity preview = m_ActiveScene->FindEntityByUUID(m_PlacePreview);
		if (!preview)
		{
			CancelPlacement();
			return;
		}

		std::vector<EntitySnapshot> snapshots;
		CaptureSubtree(*m_ActiveScene, preview, snapshots);
		m_UndoStack.Push(CreateScope<AddEntitiesCommand>(
			"Place '" + preview.GetComponent<TagComponent>().Tag + "'", std::move(snapshots)));

		const AssetHandle handle = m_PlaceHandle;
		const AssetType type = m_PlaceType;
		const std::string markerKind = m_PlaceMarkerKind;
		const glm::vec4 markerColor = m_PlaceMarkerColor;
		const float markerSize = m_PlaceMarkerSize;
		const float yaw = m_PlaceYaw;

		m_PlacePreview = UUID{ 0 };
		m_PlaceHandle = InvalidAssetHandle;
		m_PlaceType = AssetType::None;
		m_PlaceHasTarget = false;
		m_PlaceHasBounds = false;
		m_PlaceMarkerKind.clear();

		m_SceneHierarchyPanel.SetSelectedEntity(preview);

		if (chain)
		{
			if (!markerKind.empty())
				BeginMarkerPlacement(markerKind, markerColor, markerSize);
			else
				BeginPlacement(handle, type);
			m_PlaceYaw = yaw;
			ApplyPlacementTransform();
		}
	}

	bool EditorLayer::IsScattering() const
	{
		return m_MapPanel.IsPaintArmed() || m_ScatterStroke.Active;
	}

	void EditorLayer::TickScatter()
	{
		if (m_SceneState != SceneState::Edit || !m_ActiveScene)
			return;

		if (m_MapPanel.IsPaintArmed() && IsPlacing())
			CancelPlacement();

		if (m_ScatterStroke.Active && !m_MapPanel.IsPaintArmed())
		{
			EndScatterStroke();
			return;
		}

		if (!m_ScatterStroke.Active)
			return;

		if (!Input::IsMouseButtonPressed(Mouse::ButtonLeft))
		{
			EndScatterStroke();
			return;
		}

		if (!m_SurfaceHit.IsHit())
			return;

		const ScatterSettings& settings = m_MapPanel.Scatter();

		if (!m_ScatterStroke.Erase
			&& settings.FilterToStartSurface
			&& !m_ScatterStroke.FilterLocked)
		{
			m_ScatterStroke.FilterWorkPlane = m_SurfaceHit.FromWorkPlane;
			m_ScatterStroke.FilterMesh = MeshHandleOf(m_SurfaceHit.Hit);
			m_ScatterStroke.FilterLocked = true;
		}

		if (m_ScatterStroke.Erase)
		{
			ScatterEraseAt(m_SurfaceHit.Point);
			return;
		}

		if (m_ScatterStroke.Count >= settings.MaxInstancesPerStroke)
		{
			if (!m_ScatterStroke.HitCap)
			{
				GE_WARN("Scatter: hit MaxInstancesPerStroke ({0}).", settings.MaxInstancesPerStroke);
				m_ScatterStroke.HitCap = true;
			}
			return;
		}

		const float radius = std::max(settings.Radius, 0.0f);
		const float area = glm::pi<float>() * radius * radius;
		const int want = std::max(1, (int)std::lround((double)settings.Density * (double)area));

		int inDisc = 0;
		const float radius2 = radius * radius;
		for (const glm::vec3& p : m_ScatterStroke.Placed)
		{
			const glm::vec3 d = p - m_SurfaceHit.Point;
			if (glm::dot(d, d) <= radius2)
				inDisc++;
		}

		int attempts = want - inDisc;
		attempts = std::min(attempts, kScatterMaxDropRays);
		attempts = std::min(attempts, settings.MaxInstancesPerStroke - m_ScatterStroke.Count);
		if (attempts <= 0)
			return;

		glm::vec3 tangent, bitangent;
		TangentAxes(m_SurfaceHit.Normal, tangent, bitangent);

		RaycastFilter filter;
		filter.HiddenEntities = &m_SceneHierarchyPanel.HiddenEntities();
		filter.ExcludeSet = &m_ScatterStroke.Exclude;
		filter.GridHeight = m_SnapSettings.GridHeight;

		for (int i = 0; i < attempts; i++)
		{
			if (m_ScatterStroke.Count >= settings.MaxInstancesPerStroke)
				break;

			const float u = m_ScatterStroke.Rng.Float01();
			const float v = m_ScatterStroke.Rng.Float01();
			const float discR = radius * std::sqrt(u);
			const float theta = v * glm::two_pi<float>();
			const glm::vec3 candidate = m_SurfaceHit.Point
				+ tangent * (discR * std::cos(theta))
				+ bitangent * (discR * std::sin(theta));

			Math::Ray drop;
			drop.Origin = candidate + glm::vec3(0.0f, kScatterDropLift, 0.0f);
			drop.Direction = glm::vec3(0.0f, -1.0f, 0.0f);
			drop.MaxDistance = 256.0f;

			const SurfaceHit hit = RaycastScene(m_ActiveScene, drop, filter);
			if (!hit.IsHit())
				continue;

			if (settings.FilterToStartSurface && m_ScatterStroke.FilterLocked)
			{
				if (m_ScatterStroke.FilterWorkPlane)
				{
					if (!hit.FromWorkPlane)
						continue;
				}
				else if (IsAssetHandleValid(m_ScatterStroke.FilterMesh))
				{
					if (MeshHandleOf(hit.Hit) != m_ScatterStroke.FilterMesh)
						continue;
				}
			}

			if (ScatterTooClose(hit.Point))
				continue;

			ScatterPlaceOne(hit.Point, hit.Normal);
		}
	}

	void EditorLayer::BeginScatterStroke(bool erase)
	{
		if (m_ScatterStroke.Active)
			return;
		if (!m_ActiveScene || m_SceneState != SceneState::Edit)
			return;

		const ScatterSettings& settings = m_MapPanel.Scatter();
		if (!IsAssetHandleValid(settings.Source)
			|| (settings.SourceType != AssetType::Prefab && settings.SourceType != AssetType::StaticMesh))
		{
			GE_WARN("Scatter: pin a prefab or mesh and click it before painting.");
			return;
		}

		if (IsPlacing())
			CancelPlacement();

		m_ScatterStroke = ScatterStroke{};
		m_ScatterStroke.Active = true;
		m_ScatterStroke.Erase = erase;
		m_ScatterStroke.StrokeSeed = settings.Seed;
		m_ScatterStroke.Rng = Random(settings.Seed);

		if (erase)
		{
			Entity group = FindScatterGroup(*m_ActiveScene, settings.Source);
			if (group)
				m_ScatterStroke.Group = group.GetUUID();
			return;
		}

		Entity group = FindScatterGroup(*m_ActiveScene, settings.Source);
		if (!group)
			return;

		m_ScatterStroke.Group = group.GetUUID();
		if (!group.HasComponent<RelationshipComponent>())
			return;

		for (UUID childID : group.GetComponent<RelationshipComponent>().Children)
		{
			Entity child = m_ActiveScene->FindEntityByUUID(childID);
			if (child)
				ScatterRememberPoint(glm::vec3(m_ActiveScene->GetWorldSpaceTransform(child)[3]));
		}
	}

	void EditorLayer::EndScatterStroke()
	{
		if (!m_ScatterStroke.Active)
			return;

		const bool erase = m_ScatterStroke.Erase;
		const int placed = m_ScatterStroke.Count;
		const UUID groupID = m_ScatterStroke.Group;
		const uint32_t seed = m_ScatterStroke.StrokeSeed;

		if (m_ActiveScene)
		{
			if (erase)
			{
				std::vector<Scope<EditorCommand>> steps;
				steps.reserve(m_ScatterStroke.Batches.size());
				for (std::vector<EntitySnapshot>& batch : m_ScatterStroke.Batches)
				{
					if (!batch.empty())
						steps.push_back(CreateScope<DeleteEntitiesCommand>("Scatter instance", std::move(batch)));
				}

				if (steps.size() == 1)
					m_UndoStack.Push(std::move(steps.front()));
				else if (!steps.empty())
					m_UndoStack.Push(CreateScope<CompositeCommand>("Scatter erase", std::move(steps)));
			}
			else if (placed > 0)
			{
				std::vector<Scope<EditorCommand>> steps;
				if (!m_ScatterStroke.GroupSnapshot.empty())
				{
					steps.push_back(CreateScope<AddEntitiesCommand>(
						"Scatter group", std::move(m_ScatterStroke.GroupSnapshot)));
				}
				for (std::vector<EntitySnapshot>& batch : m_ScatterStroke.Batches)
				{
					if (!batch.empty())
						steps.push_back(CreateScope<AddEntitiesCommand>("Scatter instance", std::move(batch)));
				}

				if (steps.size() == 1)
					m_UndoStack.Push(std::move(steps.front()));
				else if (!steps.empty())
					m_UndoStack.Push(CreateScope<CompositeCommand>("Scatter paint", std::move(steps)));

				if (Entity group = m_ActiveScene->FindEntityByUUID(groupID))
				{
					if (group.HasComponent<ScatterGroupComponent>())
						group.GetComponent<ScatterGroupComponent>().LastSeed = seed;
				}
				m_MapPanel.Scatter().Seed = seed + 1u;
			}
			else if (!m_ScatterStroke.GroupSnapshot.empty())
			{
				RemoveSubtree(*m_ActiveScene, m_ScatterStroke.GroupSnapshot);
			}

			if ((erase && !m_ScatterStroke.Batches.empty()) || placed > 0)
			{
				if (Entity group = m_ActiveScene->FindEntityByUUID(groupID))
					m_SceneHierarchyPanel.SetSelectedEntity(group);
			}
		}

		m_ScatterStroke = ScatterStroke{};
	}

	void EditorLayer::AbortScatterStroke()
	{
		if (!m_ScatterStroke.Active)
			return;

		if (m_ActiveScene)
		{
			if (m_ScatterStroke.Erase)
			{
				for (auto it = m_ScatterStroke.Batches.rbegin(); it != m_ScatterStroke.Batches.rend(); ++it)
					RestoreSubtree(*m_ActiveScene, *it);
			}
			else
			{
				for (auto it = m_ScatterStroke.Batches.rbegin(); it != m_ScatterStroke.Batches.rend(); ++it)
					RemoveSubtree(*m_ActiveScene, *it);
				if (!m_ScatterStroke.GroupSnapshot.empty())
					RemoveSubtree(*m_ActiveScene, m_ScatterStroke.GroupSnapshot);
			}
		}

		m_ScatterStroke = ScatterStroke{};
	}

	void EditorLayer::CancelScatterMode()
	{
		if (m_ScatterStroke.Active)
			AbortScatterStroke();
		m_MapPanel.SetPaintArmed(false);
	}

	void EditorLayer::RebuildScatterExclude()
	{
		m_ScatterStroke.Exclude.clear();
		if (!m_ActiveScene)
			return;

		Entity group;
		if (m_ScatterStroke.Group != UUID{ 0 })
			group = m_ActiveScene->FindEntityByUUID(m_ScatterStroke.Group);
		if (!group)
			group = FindScatterGroup(*m_ActiveScene, m_MapPanel.Scatter().Source);
		if (group)
			CollectSubtreeUUIDs(*m_ActiveScene, group, m_ScatterStroke.Exclude);
	}

	Entity EditorLayer::EnsureScatterGroup()
	{
		if (!m_ActiveScene)
			return {};

		if (m_ScatterStroke.Group != UUID{ 0 })
		{
			Entity existing = m_ActiveScene->FindEntityByUUID(m_ScatterStroke.Group);
			if (existing)
				return existing;
			m_ScatterStroke.Group = UUID{ 0 };
			m_ScatterStroke.CreatedGroup = false;
			m_ScatterStroke.GroupSnapshot.clear();
		}

		const ScatterSettings& settings = m_MapPanel.Scatter();
		Entity existing = FindScatterGroup(*m_ActiveScene, settings.Source);
		if (existing)
		{
			m_ScatterStroke.Group = existing.GetUUID();
			return existing;
		}

		const AssetMetadata* metadata = AssetManager::GetMetadata(settings.Source);
		std::string stem = metadata
			? std::filesystem::path(metadata->FilePath).stem().string()
			: "Untitled";
		if (stem.empty())
			stem = "Untitled";

		Entity group = m_ActiveScene->CreateEntity("Scatter/" + stem);
		auto& component = group.AddComponent<ScatterGroupComponent>();
		component.Source = settings.Source;
		component.LastSeed = m_ScatterStroke.StrokeSeed;

		m_ScatterStroke.Group = group.GetUUID();
		m_ScatterStroke.CreatedGroup = true;
		CaptureSubtree(*m_ActiveScene, group, m_ScatterStroke.GroupSnapshot);
		return group;
	}

	bool EditorLayer::ScatterTooClose(const glm::vec3& p) const
	{
		const float spacing = m_MapPanel.Scatter().MinSpacing;
		if (spacing <= 0.0f)
			return false;

		const float spacing2 = spacing * spacing;
		const int cx = (int)std::floor(p.x / spacing);
		const int cy = (int)std::floor(p.y / spacing);
		const int cz = (int)std::floor(p.z / spacing);

		for (int dz = -1; dz <= 1; dz++)
		for (int dy = -1; dy <= 1; dy++)
		for (int dx = -1; dx <= 1; dx++)
		{
			auto it = m_ScatterStroke.Cells.find(ScatterCellKey(cx + dx, cy + dy, cz + dz));
			if (it == m_ScatterStroke.Cells.end())
				continue;
			for (uint32_t index : it->second)
			{
				const glm::vec3 d = m_ScatterStroke.Placed[index] - p;
				if (glm::dot(d, d) < spacing2)
					return true;
			}
		}
		return false;
	}

	void EditorLayer::ScatterRememberPoint(const glm::vec3& p)
	{
		const uint32_t index = (uint32_t)m_ScatterStroke.Placed.size();
		m_ScatterStroke.Placed.push_back(p);

		const float spacing = m_MapPanel.Scatter().MinSpacing;
		if (spacing <= 0.0f)
			return;

		const int cx = (int)std::floor(p.x / spacing);
		const int cy = (int)std::floor(p.y / spacing);
		const int cz = (int)std::floor(p.z / spacing);
		m_ScatterStroke.Cells[ScatterCellKey(cx, cy, cz)].push_back(index);
	}

	void EditorLayer::ScatterPlaceOne(const glm::vec3& point, const glm::vec3& normal)
	{
		if (!m_ActiveScene)
			return;

		const ScatterSettings& settings = m_MapPanel.Scatter();
		const AssetMetadata* metadata = AssetManager::GetMetadata(settings.Source);
		if (!metadata)
			return;

		Entity root;
		if (settings.SourceType == AssetType::Prefab)
			root = PrefabSerializer::InstantiateFromAsset(settings.Source, *m_ActiveScene);
		else if (settings.SourceType == AssetType::StaticMesh)
			root = MeshImporter::Instantiate(m_ActiveScene.get(), GetAssetRoot() / metadata->FilePath);
		if (!root)
			return;

		if (!m_ScatterStroke.SourceHasBounds)
		{
			const glm::mat4 invRoot = glm::inverse(m_ActiveScene->GetWorldSpaceTransform(root));
			AccumulateMeshBounds(*m_ActiveScene, root, invRoot,
				m_ScatterStroke.SourceBounds, m_ScatterStroke.SourceHasBounds);
			m_ScatterStroke.SourceEuler = root.GetComponent<TransformComponent>().Rotation;
			m_ScatterStroke.SourceScale = root.GetComponent<TransformComponent>().Scale;
		}

		Entity group = EnsureScatterGroup();
		if (!group)
		{
			std::vector<EntitySnapshot> failed;
			CaptureSubtree(*m_ActiveScene, root, failed);
			RemoveSubtree(*m_ActiveScene, failed);
			return;
		}

		m_ActiveScene->SetParent(root, group);

		glm::quat align(1.0f, 0.0f, 0.0f, 0.0f);
		if (settings.AlignToNormal)
			align = RotationBetween(glm::vec3(0.0f, 1.0f, 0.0f), normal);

		const float yawDeg = m_ScatterStroke.Rng.Range(-settings.YawJitter, settings.YawJitter);
		const glm::quat yaw = glm::angleAxis(glm::radians(yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 rotation = glm::mat4_cast(align * yaw)
			* EulerRotationMatrix(m_ScatterStroke.SourceEuler);

		float uniform = 1.0f;
		if (settings.ScaleMax > settings.ScaleMin)
			uniform = m_ScatterStroke.Rng.Range(settings.ScaleMin, settings.ScaleMax);
		else
			uniform = settings.ScaleMin;
		const glm::vec3 scale = m_ScatterStroke.SourceScale * uniform;

		glm::vec3 origin = point;
		if (m_SnapSettings.SitOnBounds && m_ScatterStroke.SourceHasBounds)
		{
			const glm::vec3 sitLocal(0.0f, -m_ScatterStroke.SourceBounds.Min.y * scale.y, 0.0f);
			origin += glm::vec3(rotation * glm::vec4(sitLocal, 0.0f));
		}

		glm::mat4 world = glm::translate(glm::mat4(1.0f), origin)
			* rotation
			* glm::scale(glm::mat4(1.0f), scale);

		const UUID parentID = root.HasComponent<RelationshipComponent>()
			? root.GetComponent<RelationshipComponent>().Parent : UUID{ 0 };
		if (parentID != UUID{ 0 })
		{
			Entity parent = m_ActiveScene->FindEntityByUUID(parentID);
			if (parent)
				world = glm::inverse(m_ActiveScene->GetWorldSpaceTransform(parent)) * world;
		}

		glm::vec3 translation, euler, decomposedScale;
		if (!Math::DecomposeTransform(world, translation, euler, decomposedScale))
		{
			std::vector<EntitySnapshot> failed;
			CaptureSubtree(*m_ActiveScene, root, failed);
			RemoveSubtree(*m_ActiveScene, failed);
			return;
		}

		auto& tc = root.GetComponent<TransformComponent>();
		tc.Translation = translation;
		tc.Rotation = euler;
		tc.Scale = scale;
		m_ActiveScene->MarkChanged<TransformComponent>(root);

		std::vector<EntitySnapshot> batch;
		CaptureSubtree(*m_ActiveScene, root, batch);
		m_ScatterStroke.Batches.push_back(std::move(batch));
		CollectSubtreeUUIDs(*m_ActiveScene, root, m_ScatterStroke.Exclude);
		ScatterRememberPoint(point);
		m_ScatterStroke.Count++;

		if (!m_ScatterStroke.HitSceneWarn
			&& m_ActiveScene->Reg().view<IDComponent>().size() >= kScatterSceneWarn)
		{
			GE_WARN("Scatter: scene has {0} entities. The cap exists because each instance is an entity.",
				(uint32_t)m_ActiveScene->Reg().view<IDComponent>().size());
			m_ScatterStroke.HitSceneWarn = true;
		}
	}

	void EditorLayer::ScatterEraseAt(const glm::vec3& point)
	{
		if (!m_ActiveScene)
			return;

		Entity group;
		if (m_ScatterStroke.Group != UUID{ 0 })
			group = m_ActiveScene->FindEntityByUUID(m_ScatterStroke.Group);
		if (!group)
			group = FindScatterGroup(*m_ActiveScene, m_MapPanel.Scatter().Source);
		if (!group || !group.HasComponent<RelationshipComponent>())
			return;

		m_ScatterStroke.Group = group.GetUUID();

		const float radius = std::max(m_MapPanel.Scatter().Radius, 0.0f);
		const float radius2 = radius * radius;
		const std::vector<UUID> children = group.GetComponent<RelationshipComponent>().Children;

		for (UUID childID : children)
		{
			Entity child = m_ActiveScene->FindEntityByUUID(childID);
			if (!child)
				continue;

			const glm::vec3 worldPos = glm::vec3(m_ActiveScene->GetWorldSpaceTransform(child)[3]);
			const glm::vec3 d = worldPos - point;
			if (glm::dot(d, d) > radius2)
				continue;

			std::vector<EntitySnapshot> batch;
			CaptureSubtree(*m_ActiveScene, child, batch);
			RemoveSubtree(*m_ActiveScene, batch);
			if (!batch.empty())
				m_ScatterStroke.Batches.push_back(std::move(batch));
		}
	}
}
