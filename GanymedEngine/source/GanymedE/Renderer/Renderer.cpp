#include "gepch.h"
#include "Renderer.h"

#include <glm/ext/matrix_clip_space.hpp>

#include "Renderer2D.h"
#include "Renderer3D.h"
#include "PostProcess.h"
#include "Environment.h"
#include "MeshShader.h"

#include <bgfx/bgfx.h>

namespace GanymedE {

	namespace Projection {

		bool HomogeneousDepth()
		{
			// getCaps() is only meaningful once bgfx::init has run. IsGpuAlive is the flag the
			// renderer already keeps for exactly this "is the backend up" question.
			if (!Renderer::IsGpuAlive())
				return false;

			return bgfx::getCaps()->homogeneousDepth;
		}

		bool OriginBottomLeft()
		{
			if (!Renderer::IsGpuAlive())
				return false;

			return bgfx::getCaps()->originBottomLeft;
		}

		// glm's convention-explicit forms rather than bx's mtxProj: bx/platform.h requires
		// /Zc:__cplusplus, which only the vendored bgfx projects set, and adding it to the engine
		// to obtain a matrix glm already builds is the wrong trade. It is also the stronger
		// result - `glm::perspective` under GLM_FORCE_DEPTH_ZERO_TO_ONE *is* perspectiveRH_ZO,
		// so the [0,1] branch is identical to the call it replaces by construction rather than
		// by measurement. Handedness is right, glm's default, which the workspace never overrides.
		glm::mat4 Perspective(float fovYRadians, float aspect, float nearZ, float farZ)
		{
			return HomogeneousDepth()
				? glm::perspectiveRH_NO(fovYRadians, aspect, nearZ, farZ)
				: glm::perspectiveRH_ZO(fovYRadians, aspect, nearZ, farZ);
		}

		glm::mat4 Orthographic(float left, float right, float bottom, float top,
			float nearZ, float farZ)
		{
			return HomogeneousDepth()
				? glm::orthoRH_NO(left, right, bottom, top, nearZ, farZ)
				: glm::orthoRH_ZO(left, right, bottom, top, nearZ, farZ);
		}

	}


	static bool s_DebugStats = false;
	static uint32_t s_FrameNumber = 0;
	static bool s_GpuAlive = false;

	void Renderer::Init()
	{
		GE_PROFILE_FUNCTION();

		// Stale-guard from the migration; IsSceneRenderPathDormant() is false now
		// that §5.2 landed. Kept as a single switch until Phase 7 removes it.
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

		// Released here, while bgfx is still alive - see MeshShader.h.
		MeshShader::Release();
		Environment::ReleaseSharedResources();
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
