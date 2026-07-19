#pragma once

#include <cstdint>

namespace GanymedE {

	// bgfx sorts submitted draw calls by view ID, so the order of the whole frame
	// is decided by this table rather than by the order calls happen to be made.
	// Every pass gets a fixed ID here instead of a magic number at the call site.
	//
	// A framebuffer is attached to a view (Framebuffer::BindToView) and every
	// draw submitted to that view lands in it - which is what replaced bind/unbind.
	namespace RenderPass {

		// Cleared and presented every frame by BgfxContext.
		constexpr uint16_t Backbuffer = 0;

		// Directional shadow cascades. Contiguous so cascade N is Shadow + N.
		constexpr uint16_t Shadow = 1;
		constexpr uint16_t ShadowCascadeCount = 4;

		// Main HDR scene: opaque geometry, then skybox.
		constexpr uint16_t SceneHDR = 5;
		// Transparent geometry and 2D overlays, drawn after the opaque pass.
		constexpr uint16_t SceneTransparent = 6;

		// Bloom mip chain: downsample then upsample, one view per mip in each
		// direction. Kept apart so the two halves never interleave.
		constexpr uint16_t BloomDownsample = 7;
		constexpr uint16_t BloomUpsample = 15;
		constexpr uint16_t BloomMipCount = 8;

		constexpr uint16_t Tonemap = 24;
		constexpr uint16_t FXAA = 25;
		constexpr uint16_t Composite = 26;

		// Entity-ID readback blits. Must sort after SceneHDR so the blit copies
		// this frame's IDs rather than the previous frame's.
		constexpr uint16_t Picking = 27;

		// Transient views used only while baking an environment map on load
		// (equirect -> cubemap, irradiance, prefilter, BRDF LUT).
		//
		// The bake needs one view per cubemap face per mip and runs entirely
		// within a single frame, because bgfx guarantees views execute in ID
		// order - which is what lets a later stage sample what an earlier one
		// wrote. That is 6*5 + 6 + 6*5 + 1 = 67 views, so the range runs up to
		// the ImGui view rather than a fixed small count.
		constexpr uint16_t EnvironmentBake = 32;

		// Editor UI renders last, straight to the backbuffer.
		constexpr uint16_t ImGui = 200;

	}

}
