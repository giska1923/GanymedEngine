#include "EditorTitleBar.h"

#include "EditorIcons.h"
#include "EditorTheme.h"

#include "GanymedE/main/Application.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Core/Window.h"
#include "GanymedE/Renderer/Texture.h"

#include <imgui/imgui.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace GanymedE::EditorUI {

	namespace {

		Ref<Texture2D> s_AppIcon;

		WindowHitRect ToClient(const ImVec2& min, const ImVec2& max)
		{
			const ImVec2 origin = ImGui::GetMainViewport()->Pos;
			return { min.x - origin.x, min.y - origin.y, max.x - min.x, max.y - min.y };
		}

		bool TitleBarIconButton(const char* id, const char* icon, float width, float height,
			ImU32 hoverFill, ImU32 hoverGlyph)
		{
			ImGui::PushID(id);
			const bool clicked = ImGui::InvisibleButton("##tb", ImVec2(width, height));
			const bool hovered = ImGui::IsItemHovered();
			const bool held = ImGui::IsItemActive();

			const EditorTheme& theme = Theme();
			ImU32 fill = 0;
			ImU32 glyph = theme.TextPrimary;
			if (hovered || held)
			{
				fill = hoverFill;
				glyph = hoverGlyph;
			}

			const ImVec2 p0 = ImGui::GetItemRectMin();
			const ImVec2 p1 = ImGui::GetItemRectMax();
			ImDrawList* draw = ImGui::GetWindowDrawList();
			if (fill)
				draw->AddRectFilled(p0, p1, fill);

			const ImVec2 ts = ImGui::CalcTextSize(icon);
			draw->AddText(
				ImVec2(p0.x + (width - ts.x) * 0.5f, p0.y + (height - ts.y) * 0.5f),
				glyph, icon);

			ImGui::PopID();
			return clicked;
		}

	}

	void InitTitleBar()
	{
		constexpr const char* kIcon = "assets/icons/icon.png";
		if (std::filesystem::exists(kIcon))
			s_AppIcon = Texture2D::Create(kIcon);
		else
			GE_ERROR("EditorTitleBar: app icon not found at '{0}'", kIcon);
	}

	void ShutdownTitleBar()
	{
		s_AppIcon.reset();
	}

	void DrawTitleBar(const TitleBarState& state, const std::function<void()>& drawMenus)
	{
		Window& window = Application::Get().GetWindow();
		const EditorTheme& theme = Theme();
		const float height = theme.TitleBarHeight;
		constexpr float kBtnW = 46.0f;
		constexpr float kIcon = 20.0f;

		const ImVec2 menuPad = ImGui::GetStyle().WindowPadding;
		const ImVec2 menuSpacing = ImGui::GetStyle().ItemSpacing;

		ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(theme.ChromeBg));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
		ImGui::BeginChild("##TitleBar", ImVec2(0.0f, height), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

		const ImVec2 capMin = ImGui::GetWindowPos();
		const ImVec2 capMax = ImVec2(capMin.x + ImGui::GetWindowSize().x, capMin.y + height);

		WindowHitRect exclusions[kMaxTitleBarHitExclusions];
		uint32_t exclusionCount = 0;
		auto excludeLast = [&]()
		{
			if (exclusionCount < kMaxTitleBarHitExclusions)
			{
				exclusions[exclusionCount++] = ToClient(
					ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
			}
		};

		const float yIcon = (height - kIcon) * 0.5f;
		ImGui::SetCursorPos(ImVec2(8.0f, yIcon));
		if (s_AppIcon && s_AppIcon->IsValid())
		{
			ImGui::Image(
				static_cast<ImTextureID>(static_cast<uintptr_t>(s_AppIcon->GetRendererID())),
				ImVec2(kIcon, kIcon));
		}
		else
		{
			ImGui::Dummy(ImVec2(kIcon, kIcon));
		}

		ImGui::SameLine(0.0f, 8.0f);
		{
			const char* label = ICON_LC_MENU "  Menu";
			const ImVec2 ts = ImGui::CalcTextSize(label);
			const float bw = ts.x + 16.0f;
			const float by = (height - 24.0f) * 0.5f;
			ImGui::SetCursorPosY(by);
			if (TitleBarIconButton("menu", label, bw, 24.0f, theme.SurfaceSunken, theme.TextPrimary))
				ImGui::OpenPopup("##EditorTitleBarMenu");
			excludeLast();

			ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));
			// The title-bar child zeros padding so chrome reaches the edges. Popups inherit
			// the style stack, so File/Edit/View would be flush without this restore.
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, menuPad);
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, menuSpacing);
			if (ImGui::BeginPopup("##EditorTitleBarMenu"))
			{
				if (drawMenus)
					drawMenus();
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar(2);
		}

		ImGui::SameLine(0.0f, 10.0f);
		{
			char tab[256];
			std::snprintf(tab, sizeof(tab), "%s%s",
				state.DocumentName.empty() ? "Untitled" : state.DocumentName.c_str(),
				state.Dirty ? "*" : "");

			const ImVec2 ts = ImGui::CalcTextSize(tab);
			const float tabH = 26.0f;
			const float rightReserve = kBtnW * 3.0f + 12.0f;
			float tabW = ts.x + 20.0f;
			const float tabMax = ImGui::GetWindowSize().x - ImGui::GetCursorPosX() - rightReserve;
			if (tabW > tabMax)
				tabW = tabMax;
			if (tabW < 48.0f)
				tabW = 48.0f;
			const float tabY = (height - tabH) * 0.5f;
			ImGui::SetCursorPosY(tabY);

			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const ImVec2 p1 = ImVec2(p0.x + tabW, p0.y + tabH);
			ImDrawList* draw = ImGui::GetWindowDrawList();
			draw->AddRectFilled(p0, p1, theme.SurfaceBg);
			const ImVec2 textPos(
				p0.x + 10.0f,
				p0.y + (tabH - ts.y) * 0.5f);
			draw->AddText(textPos, theme.TextPrimary, tab);
			ImGui::Dummy(ImVec2(tabW, tabH));
		}

		ImGui::SetCursorPos(ImVec2(ImGui::GetWindowSize().x - kBtnW * 3.0f, 0.0f));
		if (TitleBarIconButton("min", ICON_LC_MINUS, kBtnW, height, theme.SurfaceSunken, theme.TextPrimary))
			window.Minimize();
		excludeLast();

		ImGui::SameLine(0.0f, 0.0f);
		const char* maxIcon = window.IsMaximized() ? ICON_LC_SQUARE_SQUARE : ICON_LC_SQUARE;
		if (TitleBarIconButton("max", maxIcon, kBtnW, height, theme.SurfaceSunken, theme.TextPrimary))
			window.ToggleMaximize();
		excludeLast();

		ImGui::SameLine(0.0f, 0.0f);
		if (TitleBarIconButton("close", ICON_LC_X, kBtnW, height, theme.Error, IM_COL32(255, 255, 255, 255)))
			Application::Get().Close();
		excludeLast();

		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();

		window.SetTitleBarHitTest(ToClient(capMin, capMax), exclusions, exclusionCount);
	}

}
