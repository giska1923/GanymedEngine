#pragma once

#include "GanymedE/Assets/AssetTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace GanymedE {

	class Scene;
	class EditorUndoStack;
	class SceneHierarchyPanel;

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
			Scene* scene, EditorUndoStack* undo, SceneHierarchyPanel* hierarchy);
	private:
		void DrawPalette(bool editing);
		void DrawPlacementOptions(MapSnapSettings& snap);
		void DrawDuplicateAlongAxis(bool editing, bool placing, Scene* scene,
			EditorUndoStack* undo, SceneHierarchyPanel* hierarchy);
		void DrawUpcomingSections();

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
	};

}
