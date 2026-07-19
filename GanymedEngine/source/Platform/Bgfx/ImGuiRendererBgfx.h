#pragma once

#include <cstdint>

struct ImDrawData;

namespace GanymedE {

	// The render half of the ImGui integration, replacing ImGui_ImplOpenGL3.
	// ImGui_ImplGlfw still handles input and platform windows - only drawing
	// changes. See docs/BGFX_MIGRATION.md §8.
	//
	// Draw data is submitted to one high view ID (RenderPass::ImGui) targeting
	// the backbuffer, so the UI always sorts after every scene and post pass.
	namespace ImGuiRendererBgfx {

		bool Init();
		void Shutdown();

		// Rebuilds the font atlas if ImGui invalidated it.
		void NewFrame();

		void RenderDrawData(ImDrawData* drawData);

		// ImTextureID for a bgfx texture. ImGui stores an opaque ImU64, and the
		// backend decides what goes in it - here, the handle index.
		uint64_t ToImTextureID(uint16_t bgfxTextureIdx);

	}

}
