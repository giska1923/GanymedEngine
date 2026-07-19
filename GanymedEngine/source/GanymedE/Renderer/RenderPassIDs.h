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

		// Transient views used only while baking an environment map on load
		// (equirect -> cubemap, irradiance, prefilter, BRDF LUT).
		constexpr uint16_t EnvironmentBake = 32;
		constexpr uint16_t EnvironmentBakeCount = 8;

		// Editor UI renders last, straight to the backbuffer.
		constexpr uint16_t ImGui = 200;

	}

}
