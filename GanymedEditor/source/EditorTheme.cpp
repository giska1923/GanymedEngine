#include "EditorTheme.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <cstdint>

namespace GanymedE::EditorUI {

	namespace {

		EditorTheme s_Theme;

		ImU32 Hex(uint32_t rgb)
		{
			return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
		}

		void FillShared(EditorTheme& t)
		{
			t.Accent       = Hex(0xB182ED);
			t.AccentHover  = Hex(0xC39BFF);
			t.AccentActive = Hex(0x9152E0);
			t.TextOnAccent = Hex(0x1A1A1A);

			t.Success = Hex(0x65CC6B);
			t.Warning = Hex(0xE6B450);
			t.Error   = Hex(0xE5534B);

			t.AxisX = IM_COL32(204, 26, 38, 255);
			t.AxisY = IM_COL32(51, 179, 51, 255);
			t.AxisZ = IM_COL32(26, 64, 204, 255);

			t.AssetTint[static_cast<int>(AssetType::None)]        = IM_COL32(217, 217, 217, 255);
			t.AssetTint[static_cast<int>(AssetType::StaticMesh)]  = IM_COL32(140, 191, 255, 255);
			t.AssetTint[static_cast<int>(AssetType::Environment)] = IM_COL32(255, 191,  89, 255);
			t.AssetTint[static_cast<int>(AssetType::Texture)]     = IM_COL32(255, 140, 217, 255);
			t.AssetTint[static_cast<int>(AssetType::Material)]    = IM_COL32(217, 140, 255, 255);
			t.AssetTint[static_cast<int>(AssetType::Scene)]       = IM_COL32(140, 255, 166, 255);
			t.AssetTint[static_cast<int>(AssetType::Script)]      = IM_COL32(255, 242, 128, 255);
			t.AssetTint[static_cast<int>(AssetType::Audio)]       = IM_COL32(140, 255, 242, 255);
			t.AssetTint[static_cast<int>(AssetType::Prefab)]      = IM_COL32(153, 217, 217, 255);

			t.RowHeight           = 26.0f;
			t.ToolbarHeight       = 41.0f;
			t.TitleBarHeight      = 40.0f;
			t.TabBarHeight        = 35.0f;
			t.StatusBarHeight     = 41.0f;
			t.ColumnHeaderHeight  = 26.0f;
		}

		void FillDarkRamp(EditorTheme& t)
		{
			t.ChromeBg      = Hex(0x1A1A1A);
			t.SurfaceSunken = Hex(0x272727);
			t.SurfaceBg     = Hex(0x313131);
			t.Border        = Hex(0x1A1A1A);
			t.GrabBg        = Hex(0x4D4D4D);
			t.TextPrimary   = Hex(0xCCCCCC);
			t.TextDim       = Hex(0x878787);
			t.TextDisabled  = Hex(0x717171);

			t.AccentText    = Hex(0xB07BF4);
			t.Link          = Hex(0x589FFD);
			t.FieldMixed    = Hex(0xFFC759);
			t.FieldOverride = Hex(0x73B8FF);
		}

		void FillLightRamp(EditorTheme& t)
		{
			// Same three-stop roles as Dark (chrome darkest, surface lightest, inputs
			// recessed), stepped by about the same 23-level span so gutters still read.
			t.ChromeBg      = Hex(0xDEDEDE);
			t.SurfaceSunken = Hex(0xEBEBEB);
			t.SurfaceBg     = Hex(0xF5F5F5);
			t.Border        = Hex(0xDEDEDE);
			t.GrabBg        = Hex(0xC5C5C5);
			t.TextPrimary   = Hex(0x1A1A1A);
			t.TextDim       = Hex(0x5C5C5C);
			t.TextDisabled  = Hex(0x9E9E9E);

			// Lilac/cyan/gold that sit on Dark panels fail as text on Light. Same hue,
			// darker stop — Accent fill itself stays #B182ED.
			t.AccentText    = Hex(0x7B43C2);
			t.Link          = Hex(0x1565C0);
			t.FieldMixed    = Hex(0xB45309);
			t.FieldOverride = Hex(0x185ABC);
		}

		void SetCol(ImGuiStyle& style, ImGuiCol idx, ImU32 c, float alpha = 1.0f)
		{
			ImVec4 v = Color(c);
			v.w *= alpha;
			style.Colors[idx] = v;
		}

	}

	const EditorTheme& Theme()
	{
		return s_Theme;
	}

	EditorTheme MakeDarkTheme()
	{
		EditorTheme t{};
		FillShared(t);
		FillDarkRamp(t);
		return t;
	}

	EditorTheme MakeLightTheme()
	{
		EditorTheme t{};
		FillShared(t);
		FillLightRamp(t);
		return t;
	}

	void ApplyTheme(const EditorTheme& theme)
	{
		s_Theme = theme;

		ImGuiStyle& style = ImGui::GetStyle();

		style.WindowRounding = style.ChildRounding = style.FrameRounding =
		style.PopupRounding  = style.TabRounding   = style.GrabRounding  =
		style.ScrollbarRounding = 0.0f;

		style.WindowBorderSize = style.ChildBorderSize = 0.0f;
		style.FrameBorderSize  = 0.0f;
		style.PopupBorderSize  = 1.0f;
		style.TabBorderSize    = 0.0f;

		style.FramePadding     = ImVec2(6.0f, 3.0f);
		style.ItemSpacing      = ImVec2(6.0f, 4.0f);
		style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
		style.CellPadding      = ImVec2(6.0f, 2.0f);
		style.IndentSpacing    = 18.0f;
		style.ScrollbarSize    = 10.0f;
		style.GrabMinSize      = 10.0f;

		style.WindowMenuButtonPosition = ImGuiDir_None;
		style.TabBarBorderSize   = 0.0f;
		style.TabBarOverlineSize = 0.0f;
		style.DockingSeparatorSize = 1.0f;
		style.SeparatorTextBorderSize = 1.0f;

		SetCol(style, ImGuiCol_Text, theme.TextPrimary);
		SetCol(style, ImGuiCol_TextDisabled, theme.TextDisabled);
		SetCol(style, ImGuiCol_WindowBg, theme.SurfaceBg);
		SetCol(style, ImGuiCol_ChildBg, theme.SurfaceBg);
		SetCol(style, ImGuiCol_PopupBg, theme.SurfaceBg);
		SetCol(style, ImGuiCol_Border, theme.Border);
		SetCol(style, ImGuiCol_BorderShadow, theme.Border, 0.0f);

		// Inputs are recessed: darker than the window, not lighter. ImGui's default is the opposite.
		SetCol(style, ImGuiCol_FrameBg, theme.SurfaceSunken);
		SetCol(style, ImGuiCol_FrameBgHovered, theme.GrabBg);
		SetCol(style, ImGuiCol_FrameBgActive, theme.GrabBg);

		SetCol(style, ImGuiCol_TitleBg, theme.ChromeBg);
		SetCol(style, ImGuiCol_TitleBgActive, theme.ChromeBg);
		SetCol(style, ImGuiCol_TitleBgCollapsed, theme.ChromeBg);
		SetCol(style, ImGuiCol_MenuBarBg, theme.ChromeBg);

		SetCol(style, ImGuiCol_ScrollbarBg, theme.SurfaceBg);
		SetCol(style, ImGuiCol_ScrollbarGrab, theme.GrabBg);
		SetCol(style, ImGuiCol_ScrollbarGrabHovered, theme.TextDim);
		SetCol(style, ImGuiCol_ScrollbarGrabActive, theme.Accent);

		SetCol(style, ImGuiCol_CheckMark, theme.Accent);
		SetCol(style, ImGuiCol_SliderGrab, theme.Accent);
		SetCol(style, ImGuiCol_SliderGrabActive, theme.AccentActive);

		SetCol(style, ImGuiCol_Button, theme.SurfaceSunken);
		SetCol(style, ImGuiCol_ButtonHovered, theme.GrabBg);
		SetCol(style, ImGuiCol_ButtonActive, theme.ChromeBg);

		// Headers are also darker than the window — inspector section headers are
		// ChromeBg on SurfaceBg. ImGui's default is a *lighter* fill.
		// HeaderActive = Accent is the *held* colour only. Idle selected TreeNodes
		// use Header, so rows that overlay TextOnAccent must push Header locally
		// (outliner, Content Browser). Do not set theme Header to Accent — inspector
		// CollapsingHeaders would go lilac.
		SetCol(style, ImGuiCol_Header, theme.ChromeBg);
		SetCol(style, ImGuiCol_HeaderHovered, theme.SurfaceSunken);
		SetCol(style, ImGuiCol_HeaderActive, theme.Accent);

		SetCol(style, ImGuiCol_Separator, theme.Border);
		SetCol(style, ImGuiCol_SeparatorHovered, theme.AccentHover);
		SetCol(style, ImGuiCol_SeparatorActive, theme.Accent);

		SetCol(style, ImGuiCol_ResizeGrip, theme.Accent, 0.20f);
		SetCol(style, ImGuiCol_ResizeGripHovered, theme.AccentHover, 0.70f);
		SetCol(style, ImGuiCol_ResizeGripActive, theme.AccentActive);

		SetCol(style, ImGuiCol_Tab, theme.ChromeBg);
		SetCol(style, ImGuiCol_TabHovered, theme.SurfaceSunken);
		SetCol(style, ImGuiCol_TabSelected, theme.SurfaceBg);
		SetCol(style, ImGuiCol_TabSelectedOverline, theme.Accent, 0.0f);
		SetCol(style, ImGuiCol_TabDimmed, theme.ChromeBg);
		SetCol(style, ImGuiCol_TabDimmedSelected, theme.SurfaceBg);
		SetCol(style, ImGuiCol_TabDimmedSelectedOverline, theme.Accent, 0.0f);

		SetCol(style, ImGuiCol_DockingPreview, theme.Accent, 0.40f);
		SetCol(style, ImGuiCol_DockingEmptyBg, theme.ChromeBg);

		SetCol(style, ImGuiCol_PlotLines, theme.TextDim);
		SetCol(style, ImGuiCol_PlotLinesHovered, theme.Accent);
		SetCol(style, ImGuiCol_PlotHistogram, theme.Success);
		SetCol(style, ImGuiCol_PlotHistogramHovered, theme.Accent);

		SetCol(style, ImGuiCol_TableHeaderBg, theme.SurfaceSunken);
		SetCol(style, ImGuiCol_TableBorderStrong, theme.Border);
		SetCol(style, ImGuiCol_TableBorderLight, theme.Border);
		SetCol(style, ImGuiCol_TableRowBg, theme.SurfaceBg, 0.0f);
		SetCol(style, ImGuiCol_TableRowBgAlt, theme.ChromeBg, 0.30f);

		SetCol(style, ImGuiCol_TextLink, theme.Link);
		SetCol(style, ImGuiCol_TextSelectedBg, theme.Accent, 0.35f);
		SetCol(style, ImGuiCol_DragDropTarget, theme.Accent);
		SetCol(style, ImGuiCol_NavCursor, theme.Accent);
		SetCol(style, ImGuiCol_NavWindowingHighlight, theme.TextPrimary, 0.70f);
		// Veil is always dark. ChromeBg is light under Light, so using it here would
		// wash the editor out instead of dimming it.
		SetCol(style, ImGuiCol_NavWindowingDimBg, Hex(0x1A1A1A), 0.60f);
		SetCol(style, ImGuiCol_ModalWindowDimBg, Hex(0x1A1A1A), 0.70f);

		// View → Theme runs from the Menu popup, while the title bar still has
		// ChildBg pushed. PushStyleColor snapshots the *old* ramp; PopStyleColor
		// would write it back over this Apply() and leave every BeginChild that
		// does not push its own ChildBg on the previous theme (outliner tree,
		// Content Browser folder/file panes). WindowBg is not on that stack, which
		// is why Properties/Stats switched and those three wells did not.
		ImGuiContext* ctx = ImGui::GetCurrentContext();
		if (ctx)
		{
			for (ImGuiColorMod& mod : ctx->ColorStack)
				mod.BackupValue = style.Colors[mod.Col];
		}
	}

}
