#pragma once

#include "GanymedE.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "EditorPicking.h"

#include <filesystem>

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

		// UI
		void UI_Toolbar();
		void UI_TitleBar();
		void UI_Menus();
		void UI_StatusBar();
		void UI_Viewport();
	private:
		Ref<SceneRenderer> m_SceneRenderer; // owns the HDR target + post stack (bloom, tonemap, FXAA)
		PhysicsDebugDrawSettings m_PhysicsDebugDraw;

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

		Ref<Texture2D> m_CheckerboardTexture;

		bool m_ViewportFocused = false, m_ViewportHovered = false;
		glm::vec2 m_ViewportSize = { 0.0f, 0.0f };
		glm::vec2 m_ViewportBounds[2];

		int m_GizmoType; // ImGuizmo::OPERATION; -1 = hidden, W/E/R switch, Q hides
		bool m_GizmoWorldSpace = false; // ImGuizmo::WORLD when true; LOCAL is the previous default

		// Viewport camera dropdown. UUID{0} = EditorCamera. Any other value is a scene
		// CameraComponent looked through in edit mode (RenderContext::PreviewCamera).
		UUID m_ViewportCamera{ 0 };

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

		bool m_ResetDockLayout = false;

		// Exponential moving average of 1/ts. Raw frame time flickers; this is the
		// status-bar FPS chip. Seeded on the first sane timestep.
		float m_SmoothedFps = 0.0f;
	};
}
