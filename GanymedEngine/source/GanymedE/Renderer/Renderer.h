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

		// True while Renderer2D/3D and the scene passes cannot run.
		//
		// Buffers, shaders, textures, framebuffers and the ImGui backend are all
		// bgfx now, but the UBO rework (§5.2) is not done: UniformBuffer::Create
		// still returns null under bgfx, and Renderer3D dereferences it
		// unconditionally when uploading camera/light blocks. Turning this off
		// before that lands segfaults on the first frame.
		//
		// ImGui is deliberately NOT covered by this - its backend is ported, so
		// the editor chrome draws while the scene stays dark.
		//
		// Clears when §5.2 and the remaining shaders land; the helper itself
		// disappears with Platform/OpenGL in Phase 7.
		inline static bool IsSceneRenderPathDormant() { return true; }
	};

}
