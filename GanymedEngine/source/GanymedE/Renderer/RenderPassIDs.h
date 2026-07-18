#pragma once

#include <cstdint>

namespace GanymedE {

	// bgfx sorts submitted draw calls by view ID, so the render order of the
	// whole frame is decided by this table rather than by call order.
	//
	// Only the backbuffer view exists so far. Phase 4 adds the shadow, HDR
	// scene, bloom chain, tonemap and composite views, and Phase 6 the ImGui
	// view - see the map in docs/BGFX_MIGRATION.md §6.3.
	namespace RenderPass {

		// Cleared and presented every frame by BgfxContext.
		constexpr uint16_t Backbuffer = 0;

	}

}
