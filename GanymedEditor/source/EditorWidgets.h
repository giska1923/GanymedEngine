#pragma once

#include "GanymedE/Math/Curve.h"

#include <glm/glm.hpp>
#include <imgui/imgui.h>

#include <cstddef>
#include <initializer_list>
#include <string>

namespace GanymedE::EditorUI {

	// In-house curve/gradient widgets. Not the vendored ImCurveEdit/ImGradient:
	// those bring an unverified ActiveId story, and this editor's undo protocol
	// cannot live without a widget that owns ActiveId for the whole drag.
	//
	// Structural rule: one InvisibleButton spans the canvas. A held InvisibleButton
	// owns ActiveId until release (ImGui 1.91.9b). Hit-testing against keys decides
	// what the drag moves; ImDrawList draws. Return true ONLY on frames a key's
	// value actually changed — a grab-and-release with no motion commits nothing
	// (TrackCommitBoundary drops a pending edit whose Edited stayed false).
	//
	// Mutation goes through FloatCurve/ColorGradient AddKey/RemoveKey/SetKey only.

	bool CurveEditor(const char* label, FloatCurve& curve, float minY, float maxY,
		ImVec2 size = ImVec2(0.0f, 80.0f));

	bool GradientEditor(const char* label, ColorGradient& gradient);

	// The labelled X/Y/Z row with coloured reset buttons. Shared rather than file-static since
	// R2: the reflected property drawer for a vec3 has to produce the *same* widget the
	// hand-written sections do, or converting a component would visibly change the inspector.
	//
	// Returns true when a widget in this row actually edited `values` - the ground truth the
	// undo commit boundary is built on. It used to return void, which meant the caller had to
	// diff the value instead, and a value diff cannot tell a real edit from a float that came
	// back changed through a degrees/radians round-trip.
	// `speed` was hardcoded at 0.1 inside this function until R2, which made the `Speed`
	// attribute registered on every vec3 field silently inert. Threaded through rather than
	// dropped from the registrations: an attribute nothing reads is worse than no attribute.
	bool DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue = 0.0f,
		float columnWidth = 100.0f, float speed = 0.1f);

	// --- Panel furniture (chrome, not property editors) ---
	//
	// IconButton / ToolbarSeparator landed in phase 2. The rest is the vocabulary phases 4–8
	// consume. SearchField is the one helper that participates in text input: it is a real
	// InputTextWithHint, so HandleShortcuts' WantTextInput gate applies (Ctrl+Z is ImGui's
	// text undo while the field is focused). Do not open OverflowMenuButton unless the
	// caller has a popup to show.

	// 24x24. Transparent until hover (AccentHover fill + TextOnAccent glyph) or active
	// (Accent fill + TextOnAccent). InvisibleButton so the fill/glyph pair can change
	// together — ImGui's ButtonHovered colour cannot retint ImGuiCol_Text.
	bool IconButton(const char* icon, const char* tooltip, bool active = false);

	// 1 px vertical Border with 4 px horizontal margins, 24 px tall. SameLine around it.
	void ToolbarSeparator();

	// WindowPadding 0 so toolbar/header rows reach the panel edges. Always pair with
	// EndPanel (ImGui requires End even when Begin returns false). BeginPanelBody
	// re-pushes the style WindowPadding as a child so tree/inspector content is not
	// flush against the frame.
	bool BeginPanel(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0);
	void EndPanel();
	void BeginPanelBody();
	void EndPanelBody();

	// SurfaceBg child, 1 px Border along the bottom. Default 44 is the sampled
	// per-panel toolbar, not Theme().ToolbarHeight (the main 41 px strip).
	// `height <= 0` also falls back to 44.
	bool PanelToolbarRow(const char* id, float height = 44.0f);
	void EndPanelToolbarRow();

	// SurfaceSunken frame, search glyph, hint, clear button when non-empty.
	// Fills remaining width; honour SetNextItemWidth to cap it.
	// Returns true when the buffer changed (edit or clear).
	bool SearchField(const char* id, char* buf, size_t bufSize, const char* hint = "Search...");

	struct ColumnSpec
	{
		const char* label = "";
		float width = 0.0f; // 0 = stretch (the name column); >0 = right-aligned icon/label cell
	};

	// SurfaceSunken strip, Theme().ColumnHeaderHeight, Small font, TextDim.
	void ColumnHeaderRow(std::initializer_list<ColumnSpec> columns);

	// ICON_LC_ELLIPSIS. Returns true on click; the caller OpenPopup.
	bool OverflowMenuButton(const char* id);

	struct RowActionIcon
	{
		const char* icon = "";
		const char* tooltip = nullptr;
		bool active = false;
	};

	// Overlay on the previous item's rect (call right after TreeNodeEx / Selectable).
	// Right-aligned, TextDim, brightens to TextPrimary on row hover or when active.
	// Returns the clicked index, or -1.
	int RowActionIcons(std::initializer_list<RowActionIcon> icons, bool rowHovered);

	// Icon + label pair. `colour == 0` means TextPrimary. SameLine-friendly.
	void StatusBarItem(const char* icon, const char* text, ImU32 colour = 0);

}
