#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/UUID.h"

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

namespace GanymedE {

	class Scene;
	class EditorUndoStack;
	class SceneHierarchyPanel;
	class EditorCamera;
	struct EditorBoundsOverlay;

	// Shared by placement and ImGuizmo. Ctrl inverts Enabled rather than turning snap on:
	// modular kit pieces only line up if snapping is the resting state, and a wall 0.03 m off
	// its neighbour is a worse failure than an accidentally-snapped drag.
	struct MapSnapSettings
	{
		bool Enabled = true;
		float Translate = 0.5f;   // metres
		float Rotate = 15.0f;     // degrees (45 stays as a preset in the popup)
		float Scale = 0.1f;
		bool SnapToSurface = true;
		bool AlignToNormal = false;
		bool SitOnBounds = true;
		float GridHeight = 0.0f;
	};

	void DrawMapSnapControls(MapSnapSettings& snap);

	class MapPanel
	{
	public:
		void SetPlaceHandler(std::function<void(AssetHandle, AssetType)> handler)
		{
			m_OnPlace = std::move(handler);
		}

		void OnImGuiRender(MapSnapSettings& snap, bool editing, bool placing,
			Scene* scene, EditorUndoStack* undo, SceneHierarchyPanel* hierarchy,
			EditorCamera* camera, UUID excludeFromAudit);

		// Copied into EditorBoundsOverlay before OnUpdateEditor. One-frame lag on click is
		// expected: ImGui runs after the 3D submit.
		void FillOverlay(EditorBoundsOverlay& overlay) const;
	private:
		void DrawPalette(bool editing);
		void DrawPlacementOptions(MapSnapSettings& snap);
		void DrawDuplicateAlongAxis(bool editing, bool placing, Scene* scene,
			EditorUndoStack* undo, SceneHierarchyPanel* hierarchy);
		void DrawParityAudit(bool editing, Scene* scene, EditorUndoStack* undo,
			SceneHierarchyPanel* hierarchy, EditorCamera* camera, UUID excludeFromAudit);
		void DrawUpcomingSections();

		void RebuildAudit(Scene* scene, SceneHierarchyPanel* hierarchy, UUID excludeFromAudit);
		void RefreshFocusOverlay(Scene* scene);
		void FocusFinding(Scene* scene, SceneHierarchyPanel* hierarchy,
			EditorCamera* camera, UUID id);
		void GenerateCollidersFromMesh(Scene* scene, EditorUndoStack* undo,
			SceneHierarchyPanel* hierarchy);

		void LoadPalette();
		void SavePalette() const;
		void Pin(const std::string& relativePath);
		void Unpin(const std::string& relativePath);

		std::function<void(AssetHandle, AssetType)> m_OnPlace;
		std::vector<std::string> m_Pinned;
		bool m_PaletteLoaded = false;
		char m_PinSearch[128] = {};

		int m_DupCount = 6;
		float m_DupSpacing = 2.0f;
		int m_DupAxis = 0; // 0=X 1=Y 2=Z

		float m_AuditTolerance = 0.02f;
		bool m_AuditDirty = true;
		const Scene* m_AuditScene = nullptr;
		size_t m_AuditUndo = 0;
		size_t m_AuditRedo = 0;
		size_t m_AuditHiddenHash = 0;
		UUID m_AuditExclude{ 0 };
		float m_AuditToleranceStamp = 0.02f;

		enum class FindingKind { NoCollider, Smaller, Larger, OffsetMismatch };
		struct Finding
		{
			UUID Entity{ 0 };
			FindingKind Kind = FindingKind::NoCollider;
			std::string Name;
		};
		struct Footprint
		{
			UUID Root{ 0 };
			std::string Name;
			float Coverage = 0.0f;
			bool HasCollider = false;
		};
		std::vector<Finding> m_Findings;
		std::vector<Footprint> m_Footprints;

		UUID m_FocusUUID{ 0 };
		struct OverlayBox
		{
			glm::mat4 Transform{ 1.0f };
			glm::vec4 Color{ 1.0f };
		};
		std::vector<OverlayBox> m_OverlayBoxes;
	};

}
