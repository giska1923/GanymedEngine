#pragma once

#include "GanymedE/Math/Curve.h"

#include <glm/glm.hpp>
#include <imgui/imgui.h>

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

	// Chrome, not a property editor. 24x24. Transparent until hover (AccentHover fill +
	// TextOnAccent glyph) or active (Accent fill + TextOnAccent). Drawn with InvisibleButton
	// so the fill/glyph pair can change together — ImGui's ButtonHovered colour cannot retint
	// ImGuiCol_Text, and AccentHover + TextPrimary fails contrast.
	bool IconButton(const char* icon, const char* tooltip, bool active = false);

	// 1 px vertical Border with 4 px horizontal margins, 24 px tall. SameLine around it.
	void ToolbarSeparator();

}
