#pragma once

#include "GanymedE/Math/Curve.h"

#include <imgui/imgui.h>

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

}
