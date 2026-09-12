#pragma once

#include "GanymedE/Assets/AssetTypes.h"

#include <imgui/imgui.h>

namespace GanymedE::EditorUI {

	// Indexed by AssetType. Prefab is the last enumerator; append-only, same rule as the enum.
	static constexpr int kAssetTintCount = static_cast<int>(AssetType::Prefab) + 1;

	// Flat token bag. Not a theming system: one Apply() with one call site. ImU32 because
	// most consumers are ImDrawList; convert at the PushStyleColor boundary via Color().
	// Semantic names, never colour names — re-branding is a data change.
	struct EditorTheme
	{
		// Neutral ramp
		ImU32 ChromeBg, SurfaceSunken, SurfaceBg, Border, GrabBg;
		ImU32 TextPrimary, TextDim, TextDisabled;
		// Semantic
		ImU32 Accent, AccentHover, AccentActive, AccentText, TextOnAccent;
		ImU32 Link, Success, Warning, Error;
		ImU32 FieldMixed, FieldOverride;
		ImU32 AxisX, AxisY, AxisZ;
		ImU32 AssetTint[kAssetTintCount];
		// Metrics (consumed by later phases; ApplyTheme writes the ImGuiStyle equivalents)
		float RowHeight, ToolbarHeight, TabBarHeight, StatusBarHeight, ColumnHeaderHeight;
	};

	const EditorTheme& Theme();
	EditorTheme MakeGanymedTheme();
	EditorTheme MakeColdwarTheme();
	void ApplyTheme(const EditorTheme& theme);

	inline ImVec4 Color(ImU32 c)
	{
		return ImGui::ColorConvertU32ToFloat4(c);
	}

	inline ImVec4 ColorHover(ImU32 c)
	{
		ImVec4 v = Color(c);
		v.x += (1.0f - v.x) * 0.15f;
		v.y += (1.0f - v.y) * 0.15f;
		v.z += (1.0f - v.z) * 0.15f;
		return v;
	}

	inline ImU32 WithAlpha(ImU32 c, float a)
	{
		ImVec4 v = Color(c);
		v.w = a;
		return ImGui::ColorConvertFloat4ToU32(v);
	}

}
