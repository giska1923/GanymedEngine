#pragma once

#include "GanymedE/Core/Core.h"

namespace GanymedE {

	class Framebuffer;
	class Shader;

	// Small fullscreen-pass helper. Owns the shared fullscreen quad and the
	// tonemap pass; SceneRenderer builds the larger post stack (bloom, FXAA) on
	// top of DrawFullscreenQuad.
	class PostProcess
	{
	public:
		static void Init();
		static void Shutdown();

		// Samples the HDR color attachment (index 0) of `source` and tonemaps it into
		// whatever framebuffer is currently bound. `bloom` (optional) is composited
		// additively in HDR before the curve.
		static void Tonemap(const Ref<Framebuffer>& source, float exposure,
			const Ref<Framebuffer>& bloom = nullptr, float bloomIntensity = 0.0f);

		// Draws the shared fullscreen quad with whatever shader/textures are bound.
		// Depth test/write and culling are disabled for the draw and restored after.
		static void DrawFullscreenQuad();
	};

}
