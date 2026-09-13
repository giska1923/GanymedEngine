#pragma once

struct ImFont;

namespace GanymedE::EditorUI::EditorFonts {

	// Replaces the atlas ImGuiLayer built (embedded default) with Inter + merged
	// Lucide. Must run after ImGuiLayer::OnAttach — the context has to exist — and
	// before the first NewFrame. ImFont* values held across a Clear() dangle, so
	// FontDefault is reassigned in the same call; ImGuiRendererBgfx::NewFrame
	// rebuilds the bgfx texture when !IsBuilt().
	bool Load();

	ImFont* Body();    // Inter Regular 18, default
	ImFont* Header();  // Inter Medium 18
	ImFont* Small();   // Inter Regular 16

}
