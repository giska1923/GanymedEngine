#include "EditorFonts.h"
#include "EditorIcons.h"

#include "GanymedE/Core/Log.h"

#include <imgui/imgui.h>
#include <imgui/misc/freetype/imgui_freetype.h>

#include <filesystem>

namespace GanymedE::EditorUI::EditorFonts {

	namespace {

		constexpr const char* kInterRegular = "assets/fonts/inter/Inter-Regular.ttf";
		constexpr const char* kInterMedium  = "assets/fonts/inter/Inter-Medium.ttf";
		constexpr const char* kLucide       = "assets/fonts/lucide/lucide.ttf";

		ImFont* s_Body = nullptr;
		ImFont* s_Header = nullptr;
		ImFont* s_Small = nullptr;

		ImFont* AddFace(ImGuiIO& io, const char* path, float size, bool mergeIcons)
		{
			ImFontConfig cfg;
			cfg.OversampleH = 1;
			cfg.OversampleV = 1;
			cfg.PixelSnapH = true;
			ImFont* font = io.Fonts->AddFontFromFileTTF(path, size, &cfg);
			if (!font)
				return nullptr;

			if (mergeIcons)
			{
				// Range pointer must outlive atlas Build(); static is the usual
				// ImGui pattern. GlyphMinAdvanceX keeps icon cells monospaced so
				// toolbars align; GlyphOffset sits them on the text baseline.
				static const ImWchar kLucideRange[] = { ICON_MIN_LC, ICON_MAX_LC, 0 };

				ImFontConfig iconCfg;
				iconCfg.MergeMode = true;
				iconCfg.PixelSnapH = true;
				iconCfg.GlyphMinAdvanceX = 18.0f;
				iconCfg.GlyphOffset = ImVec2(0.0f, 3.0f);
				iconCfg.OversampleH = 1;
				iconCfg.OversampleV = 1;
				io.Fonts->AddFontFromFileTTF(kLucide, 16.0f, &iconCfg, kLucideRange);
			}

			return font;
		}

	}

	bool Load()
	{
		ImGuiIO& io = ImGui::GetIO();

		if (!std::filesystem::exists(kInterRegular) || !std::filesystem::exists(kInterMedium))
		{
			GE_ERROR("EditorFonts: Inter not found under assets/fonts/inter/; keeping the built-in ImGui font");
			return false;
		}

		const bool mergeIcons = std::filesystem::exists(kLucide);
		if (!mergeIcons)
			GE_ERROR("EditorFonts: Lucide not found at '{0}'; ICON_LC_* will render as missing glyphs", kLucide);

		// Do not use io.FontGlobalScale — it scales an already-rasterized atlas
		// and is what makes ImGui text look smeared. Each size is its own font.
		io.Fonts->Clear();
		io.FontDefault = nullptr;
		io.Fonts->FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_LightHinting;

		s_Body = AddFace(io, kInterRegular, 18.0f, mergeIcons);
		s_Header = AddFace(io, kInterMedium, 18.0f, mergeIcons);
		s_Small = AddFace(io, kInterRegular, 16.0f, mergeIcons);
		io.FontDefault = s_Body;
		return s_Body != nullptr;
	}

	ImFont* Body() { return s_Body; }
	ImFont* Header() { return s_Header; }
	ImFont* Small() { return s_Small; }

}
