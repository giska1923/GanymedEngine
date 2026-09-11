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

		// Transient views used only while baking an environment map on load
		// (equirect -> cubemap, irradiance, prefilter, BRDF LUT). One view per cubemap
		// face per mip: 6*5 + 6 + 6*5 + 1 = 67.
		//
		// **This is a prepass, so it sorts before everything that samples its output.**
		// It used to sit at 32, after the scene - which meant an environment baked at the
		// top of a frame was not readable by that frame's scene pass, and the bake papered
		// over it by calling `bgfx::frame()` twice inline to force its own frames. Those two
		// forced presentations cost 24-26 ms of blocked main thread, measured, and were the
		// larger half of the load hitch. Ordering the bake first makes it correct within the
		// frame it is submitted in, and the forced frames are gone.
		constexpr uint16_t EnvironmentBake = 1;
		constexpr uint16_t EnvironmentBakeViewCount = 67;

		// Directional shadow cascades. Contiguous so cascade N is Shadow + N.
		constexpr uint16_t Shadow = 69;
		constexpr uint16_t ShadowCascadeCount = 4;

		// Main HDR scene: opaque geometry, then skybox.
		constexpr uint16_t SceneHDR = 73;
		// Transparent geometry and 2D overlays, drawn after the opaque pass.
		constexpr uint16_t SceneTransparent = 74;

		// Bloom mip chain: downsample then upsample, one view per mip in each
		// direction. Kept apart so the two halves never interleave.
		constexpr uint16_t BloomDownsample = 75;
		constexpr uint16_t BloomUpsample = 83;
		constexpr uint16_t BloomMipCount = 8;

		constexpr uint16_t Tonemap = 92;
		constexpr uint16_t FXAA = 93;
		constexpr uint16_t Composite = 94;

		// Entity-ID readback blits. Must sort after SceneHDR so the blit copies
		// this frame's IDs rather than the previous frame's.
		constexpr uint16_t Picking = 95;

		// Game UI (RmlUi), composited into the final LDR image.
		//
		// Because bgfx executes views in ID order, this one constant is the whole
		// ordering story: sitting after Composite (26) means the UI lands on the
		// tonemapped, anti-aliased image in display space and is never itself
		// tonemapped - no matter where in the frame the submit calls happen. It
		// also appears inside the editor's viewport image for free, since that
		// image IS the composite attachment.
		constexpr uint16_t UI = 96;

		// Editor UI renders last, straight to the backbuffer.
		constexpr uint16_t ImGui = 200;

	}

}
