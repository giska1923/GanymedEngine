#include "gepch.h"
#include "Renderer.h"

#include "Renderer2D.h"
#include "Renderer3D.h"
#include "PostProcess.h"

#include <bgfx/bgfx.h>

namespace GanymedE {

	static bool s_DebugStats = false;
	static uint32_t s_FrameNumber = 0;

	void Renderer::Init()
	{
		GE_PROFILE_FUNCTION();

		// Renderer2D/3D and PostProcess build their shaders, buffers and
		// framebuffers through OpenGL directly. There is no GL context under
		// bgfx, so they stay dormant until Phases 2-5 port them.
		if (IsLegacyGLPathDormant())
			return;

		RenderCommand::Init();
		Renderer2D::Init();
		Renderer3D::Init();
		PostProcess::Init();
	}

	void Renderer::Shutdown()
	{
		if (IsLegacyGLPathDormant())
			return;

		PostProcess::Shutdown();
		Renderer3D::Shutdown();
		Renderer2D::Shutdown();
	}

	void Renderer::OnWindowResize(uint32_t width, uint32_t height)
	{
		// bgfx resizes with the swapchain (BgfxContext::Resize), and viewports
		// are per-view state set at submit time rather than global.
		if (IsLegacyGLPathDormant())
			return;

		RenderCommand::SetViewport(0, 0, width, height);
	}

	void Renderer::OnFrameSubmitted(uint32_t frameNumber)
	{
		s_FrameNumber = frameNumber;
	}

	uint32_t Renderer::GetFrameNumber()
	{
		return s_FrameNumber;
	}

	void Renderer::SetDebugStatsEnabled(bool enabled)
	{
		s_DebugStats = enabled;
		bgfx::setDebug(enabled ? (BGFX_DEBUG_TEXT | BGFX_DEBUG_STATS) : BGFX_DEBUG_TEXT);
	}

	bool Renderer::IsDebugStatsEnabled()
	{
		return s_DebugStats;
	}

}
