#pragma once

#include "GanymedE/Core/Core.h"

namespace GanymedE {

	class Framebuffer;

	// Small fullscreen-pass helper. The first pass is tonemapping (HDR -> LDR);
	// later passes (bloom, FXAA — roadmap Phase 5) can reuse the same fullscreen triangle setup.
	class PostProcess
	{
	public:
		static void Init();
		static void Shutdown();

		// Samples the HDR color attachment (index 0) of `source` and tonemaps it into
		// whatever framebuffer is currently bound.
		static void Tonemap(const Ref<Framebuffer>& source, float exposure);
	};

}
