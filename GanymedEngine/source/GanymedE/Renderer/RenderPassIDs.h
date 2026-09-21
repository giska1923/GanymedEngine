#pragma once

#include "GanymedE/Core/Core.h"

#include <cstdint>

namespace GanymedE {

	// bgfx sorts submitted draw calls by view ID, so the order of the whole frame
	// is decided by this table rather than by the order calls happen to be made.
	//
	// Scene passes are **offsets from a base**. SceneRenderer::BeginFrame pushes
	// its viewBase as the active base; EndFrame pops it. Renderer3D / Renderer2D
	// / Environment resolve through RenderPass::Id, so a second SceneRenderer
	// can bind a different framebuffer to "SceneHDR" without stealing view 73.
	// Global passes (backbuffer, environment bake, game UI, ImGui) stay absolute
	// — they are not per-renderer.
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

		// Offsets from SceneRenderer's viewBase. MainViewBase is 69 so these land
		// on the same IDs the table used when they were absolute — the main
		// viewport's frame graph does not move.
		constexpr uint16_t Shadow = 0;
		constexpr uint16_t ShadowCascadeCount = 4;

		constexpr uint16_t SceneHDR = 4;
		constexpr uint16_t SceneTransparent = 5;

		constexpr uint16_t BloomDownsample = 6;
		constexpr uint16_t BloomUpsample = 14;
		constexpr uint16_t BloomMipCount = 8;

		constexpr uint16_t Tonemap = 23;
		constexpr uint16_t FXAA = 24;
		constexpr uint16_t Composite = 25;
		constexpr uint16_t Picking = 26;

		// Inclusive span of scene offsets (Shadow through Picking). Tonemap sits
		// one past BloomUpsample+BloomMipCount, matching the unused view 91 the
		// absolute table left.
		constexpr uint16_t SceneViewCount = 27;

		constexpr uint16_t MainViewBase = 69;
		// Preview range: neither EnvironmentBake nor the main 69–95 block.
		// BGFX_CONFIG_MAX_VIEWS is 256; ImGui sits at 200, so 97–199 is free.
		constexpr uint16_t PreviewViewBase = 100;

		// Game UI (RmlUi), composited into the *main* LDR image.
		//
		// Absolute on purpose: a preview must not steal this view. Because bgfx
		// executes views in ID order, this one constant is the whole ordering
		// story — sitting after the main Composite means the UI lands on the
		// tonemapped, anti-aliased image in display space and is never itself
		// tonemapped.
		constexpr uint16_t UI = 96;

		// Editor UI renders last, straight to the backbuffer.
		constexpr uint16_t ImGui = 200;

		static_assert(MainViewBase + Shadow == 69, "main Shadow view moved");
		static_assert(MainViewBase + SceneHDR == 73, "main SceneHDR view moved");
		static_assert(MainViewBase + SceneTransparent == 74, "main SceneTransparent view moved");
		static_assert(MainViewBase + BloomDownsample == 75, "main BloomDownsample view moved");
		static_assert(MainViewBase + BloomUpsample == 83, "main BloomUpsample view moved");
		static_assert(MainViewBase + Tonemap == 92, "main Tonemap view moved");
		static_assert(MainViewBase + FXAA == 93, "main FXAA view moved");
		static_assert(MainViewBase + Composite == 94, "main Composite view moved");
		static_assert(MainViewBase + Picking == 95, "main Picking view moved");
		static_assert(MainViewBase + SceneViewCount == 96, "main scene range no longer ends at UI");
		static_assert(UI == 96, "UI view moved");
		static_assert(PreviewViewBase + SceneViewCount <= ImGui, "preview range collides with ImGui");
		static_assert(PreviewViewBase >= MainViewBase + SceneViewCount, "preview range collides with main");

		inline uint16_t& ActiveBaseStorage()
		{
			static uint16_t base = MainViewBase;
			return base;
		}

		inline bool& InSceneRenderStorage()
		{
			static bool inScene = false;
			return inScene;
		}

		inline uint16_t& SavedBaseStorage()
		{
			static uint16_t saved = MainViewBase;
			return saved;
		}

		inline uint16_t ActiveBase()
		{
			return ActiveBaseStorage();
		}

		inline uint16_t Id(uint16_t offset)
		{
			const uint32_t id = (uint32_t)ActiveBase() + offset;
			GE_CORE_ASSERT(id < 256, "View-ID range overrun resolving a pass offset");
			return (uint16_t)id;
		}

		inline void PushActiveBase(uint16_t base, uint16_t count)
		{
			GE_CORE_ASSERT(!InSceneRenderStorage(),
				"Scene renders do not nest: Renderer3D frame state is a single static. "
				"A preview must be a complete BeginFrame...EndFrame outside the main one.");
			GE_CORE_ASSERT(count > 0 && (uint32_t)base + count <= 256,
				"View-ID range overrun: base and count exceed BGFX_CONFIG_MAX_VIEWS (256)");

			SavedBaseStorage() = ActiveBaseStorage();
			ActiveBaseStorage() = base;
			InSceneRenderStorage() = true;
		}

		inline void PopActiveBase()
		{
			GE_CORE_ASSERT(InSceneRenderStorage(),
				"RenderPass::PopActiveBase without a matching Push");
			ActiveBaseStorage() = SavedBaseStorage();
			InSceneRenderStorage() = false;
		}

	}

}
