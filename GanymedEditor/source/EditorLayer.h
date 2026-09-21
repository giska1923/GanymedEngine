#pragma once

#include "GanymedE.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/MapPanel.h"
#include "EditorPicking.h"
#include "EditorUndo.h"

#include "GanymedE/Core/Random.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GanymedE {
	class EditorLayer : public Layer
	{
	public:
		EditorLayer();
		virtual ~EditorLayer() = default;

		virtual void OnAttach() override;
		virtual void OnDetach() override;

		virtual void OnUpdate(Timestep ts) override;
		virtual void OnImGuiRender() override;
		virtual void OnEvent(Event& e) override;
	private:
		bool OnKeyPressed(KeyPressedEvent& e);
		bool OnMouseButtonPressed(MouseButtonPressedEvent& e);

		void NewScene();
		void OpenScene();
		void OpenScene(const std::filesystem::path& path);
		void SaveScene();       // to the current path; falls back to Save As when there is none
		void SaveSceneAs();
		void SaveSceneTo(const std::filesystem::path& path);

		// Editor-global shortcuts, polled inside the ImGui frame rather than routed through the
		// engine event path - see the comment on the definition.
		void HandleShortcuts();

		// Points the hierarchy panel at the scene it should edit, and at the undo stack it
		// should record into (null in Play, so play-mode edits cannot be recorded at all).
		void RetargetPanels();

		void OnScenePlay();
		void OnSceneStop();

		// Seeds a fresh scene with a default sun + sky so meshes are lit immediately
		void SetupDefaultEnvironment(const Ref<Scene>& scene);

		bool IsPlacing() const { return m_PlaceType != AssetType::None || !m_PlaceMarkerKind.empty(); }
		bool IsScattering() const;
		bool SnapActive() const;
		void CancelPlacement();
		void BeginPlacement(AssetHandle handle, AssetType type);
		void BeginMarkerPlacement(const std::string& kind, const glm::vec4& color, float size);
		void UpdateSurfaceRaycast();
		void RefreshPlaceBounds(Entity root);
		void ApplyPlacementTransform();
		void CommitPlacement(bool chain);

		void TickScatter();
		void BeginScatterStroke(bool erase);
		void EndScatterStroke();
		void AbortScatterStroke();
		void CancelScatterMode();
		void RebuildScatterExclude();
		Entity EnsureScatterGroup();
		bool ScatterTooClose(const glm::vec3& p) const;
		void ScatterRememberPoint(const glm::vec3& p);
		void ScatterPlaceOne(const glm::vec3& point, const glm::vec3& normal);
		void ScatterEraseAt(const glm::vec3& point);

		// UI
		void UI_Toolbar();
		void UI_TitleBar();
		void UI_Menus();
		void UI_StatusBar();
		void UI_Viewport();

		void SetEditorTopOrtho(bool enabled);
	private:
		Ref<SceneRenderer> m_SceneRenderer; // owns the HDR target + post stack (bloom, tonemap, FXAA)
		PhysicsDebugDrawSettings m_PhysicsDebugDraw;
		bool m_ShowColliderGizmos = true;
		bool m_ShowMarkers = true;

		Ref<Scene> m_ActiveScene;
		Ref<Scene> m_EditorScene;
		std::filesystem::path m_EditorScenePath;   // empty until the scene has been opened or saved

		// Scene edits only, and only in Edit state. The stack survives play/stop because
		// m_EditorScene does; it is cleared on New/Open, where every UUID in it stops meaning
		// anything.
		EditorUndoStack m_UndoStack;

		EditorCamera m_EditorCamera;

		Entity m_HoveredEntity;
		SurfaceHit m_SurfaceHit;
		float m_SurfaceRaycastMs = 0.0f;
		Math::Ray m_EditRay;

		Ref<Texture2D> m_CheckerboardTexture;

		bool m_ViewportFocused = false, m_ViewportHovered = false;
		glm::vec2 m_ViewportSize = { 0.0f, 0.0f };
		glm::vec2 m_ViewportBounds[2];

		int m_GizmoType; // ImGuizmo::OPERATION; -1 = hidden, W/E/R switch, Q hides
		bool m_GizmoWorldSpace = false; // ImGuizmo::WORLD when true; LOCAL is the previous default

		// Viewport camera dropdown. UUID{0} = EditorCamera. Any other value is a scene
		// CameraComponent looked through in edit mode (RenderContext::PreviewCamera).
		UUID m_ViewportCamera{ 0 };

		// Viewport combo "Top (Ortho)": EditorCamera in orthographic mode, pitch locked.
		// Independent of m_ViewportCamera so looking through a scene camera does not
		// forget the editor's last projection mode.
		bool m_EditorTopOrtho = false;

		// A gizmo drag writes the transform every frame and accumulates rotation as a delta, so
		// the pre-drag value cannot be reconstructed after the fact - it is snapshotted on the
		// rising edge of ImGuizmo::IsUsing() and committed on the falling one.
		//
		// **Every entity the drag moves**, primary first, because the gizmo drives the whole
		// selection. One drag is one undo entry, so the falling edge folds these into a single
		// CompositeCommand exactly as the inspector's multi-edit does.
		bool m_GizmoUsing = false;
		std::vector<std::pair<UUID, TransformComponent>> m_GizmoBefore;

		enum class SceneState
		{
			Edit = 0,
			Play = 1
		};
		SceneState m_SceneState = SceneState::Edit;

		// Panels
		SceneHierarchyPanel m_SceneHierarchyPanel;
		ContentBrowserPanel m_ContentBrowserPanel;
		MapPanel m_MapPanel;
		MapSnapSettings m_SnapSettings;

		// Placement preview is a real entity, excluded from the surface ray so it cannot
		// snap to itself. UUID rather than a handle: handles are not validity-checked.
		AssetHandle m_PlaceHandle = InvalidAssetHandle;
		AssetType m_PlaceType = AssetType::None;
		UUID m_PlacePreview{ 0 };
		float m_PlaceYaw = 0.0f;
		glm::vec3 m_PlaceBaseEuler{ 0.0f };
		glm::vec3 m_PlaceBaseScale{ 1.0f };
		AABB m_PlaceBounds;
		bool m_PlaceHasBounds = false;
		bool m_PlaceHasTarget = false;
		std::string m_PlaceMarkerKind;
		glm::vec4 m_PlaceMarkerColor{ 0.2f, 0.9f, 0.35f, 1.0f };
		float m_PlaceMarkerSize = 0.5f;

		struct ScatterStroke
		{
			bool Active = false;
			bool Erase = false;
			bool CreatedGroup = false;
			UUID Group{ 0 };
			Random Rng{ 1 };
			uint32_t StrokeSeed = 1;
			AssetHandle FilterMesh = InvalidAssetHandle;
			bool FilterWorkPlane = false;
			bool FilterLocked = false;
			std::vector<EntitySnapshot> GroupSnapshot;
			std::vector<std::vector<EntitySnapshot>> Batches;
			std::vector<glm::vec3> Placed;
			std::unordered_map<uint64_t, std::vector<uint32_t>> Cells;
			std::unordered_set<UUID> Exclude;
			int Count = 0;
			bool HitCap = false;
			bool HitSceneWarn = false;
			AABB SourceBounds;
			bool SourceHasBounds = false;
			glm::vec3 SourceEuler{ 0.0f };
			glm::vec3 SourceScale{ 1.0f };
		};
		ScatterStroke m_ScatterStroke;

		bool m_ResetDockLayout = false;

		// Exponential moving average of 1/ts. Raw frame time flickers; this is the
		// status-bar FPS chip. Seeded on the first sane timestep.
		float m_SmoothedFps = 0.0f;
	};
}
