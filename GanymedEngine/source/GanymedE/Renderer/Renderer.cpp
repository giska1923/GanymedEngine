#include "gepch.h"
#include "Renderer.h"

#include "Renderer2D.h"
#include "Renderer3D.h"
#include "PostProcess.h"

#include <bgfx/bgfx.h>

namespace GanymedE {

	static bool s_DebugStats = false;
	static uint32_t s_FrameNumber = 0;
	static bool s_GpuAlive = false;

	void Renderer::Init()
	{
		GE_PROFILE_FUNCTION();

		// Renderer2D/3D and PostProcess still allocate UBOs, which have no bgfx
		// equivalent yet (§5.2). Initialising them would hand out null uniform
		// buffers that the first frame dereferences.
		if (IsSceneRenderPathDormant())
			return;

		RenderCommand::Init();
		Renderer2D::Init();
		Renderer3D::Init();
		PostProcess::Init();
	}

	void Renderer::Shutdown()
	{
		if (IsSceneRenderPathDormant())
			return;

		PostProcess::Shutdown();
		Renderer3D::Shutdown();
		Renderer2D::Shutdown();
	}

	void Renderer::OnWindowResize(uint32_t width, uint32_t height)
	{
		// bgfx resizes with the swapchain (BgfxContext::Resize), and viewports
		// are per-view state set at submit time rather than global.
		if (IsSceneRenderPathDormant())
			return;

		RenderCommand::SetViewport(0, 0, width, height);
	}

	bool Renderer::IsGpuAlive() { return s_GpuAlive; }
	void Renderer::SetGpuAlive(bool alive) { s_GpuAlive = alive; }

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
