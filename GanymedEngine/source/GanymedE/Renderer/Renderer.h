#pragma once

#include "RenderCommand.h"
#include "RendererAPI.h"

#include "GanymedE/Renderer/Shader.h"

namespace GanymedE {

	class Renderer
	{
	public:
		static void Init();
		static void Shutdown();

		static void OnWindowResize(uint32_t width, uint32_t height);

		// bgfx completes work asynchronously, so anything waiting on the GPU
		// (currently entity-ID readback) needs to know which frame has landed.
		// BgfxContext reports it after each bgfx::frame().
		static void OnFrameSubmitted(uint32_t frameNumber);
		static uint32_t GetFrameNumber();

		// bgfx's built-in stats/debug-text overlay (F1 in the running app).
		static void SetDebugStatsEnabled(bool enabled);
		static bool IsDebugStatsEnabled();

		inline static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }

		// True while bgfx owns the frame but Renderer2D/3D, PostProcess,
		// Framebuffer and the ImGui backend still speak OpenGL directly. The
		// window is created with GLFW_NO_API, so there is no GL context and
		// those paths would fault on their first gl* call - every one of them is
		// guarded on this.
		//
		// Phases 2-6 port one subsystem each and drop its guard; the helper
		// itself dies with Platform/OpenGL in Phase 7.
		// See docs/BGFX_MIGRATION.md.
		inline static bool IsLegacyGLPathDormant() { return GetAPI() == RendererAPI::API::Bgfx; }
	};

}
