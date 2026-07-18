#include <GanymedE.h>
//-------------ENTRY POINT-----------------
#include <GanymedE/main/EntryPoint.h>
//-----------------------------------------

#include "Sandbox2D.h"

// ---------------------------------------------------------------------------
// TEMPORARY - bgfx migration Phase 0 (docs/BGFX_MIGRATION.md §2).
// Proves bx/bimg/bgfx compile, link, and initialise. The Noop backend touches
// no window or GPU, so it is safe to run alongside the existing GL context.
// Delete once Phase 1 hands bgfx the real swapchain.
// ---------------------------------------------------------------------------
#include <bgfx/bgfx.h>

static void BgfxSanityCheck()
{
	GE_INFO("bgfx sanity check: starting");

	// Calling renderFrame() before init() selects bgfx's single-threaded mode.
	bgfx::renderFrame();

	bgfx::Init init;
	init.type = bgfx::RendererType::Noop;
	init.resolution.width = 1;
	init.resolution.height = 1;
	init.resolution.reset = BGFX_RESET_NONE;

	if (!bgfx::init(init))
	{
		GE_ERROR("bgfx sanity check: bgfx::init failed");
		return;
	}

	GE_INFO("bgfx sanity check: initialised, renderer = {0}",
		bgfx::getRendererName(bgfx::getRendererType()));

	bgfx::shutdown();
	GE_INFO("bgfx sanity check: shutdown clean");
}

class Sandbox : public GanymedE::Application {
public:
	Sandbox() {
		PushLayer(new Sandbox2D());
	}
	~Sandbox() {}

};

GanymedE::Application* GanymedE::CreateApplication() {
	BgfxSanityCheck(); // TEMPORARY - see above, bgfx migration Phase 0
	return new Sandbox();
}
