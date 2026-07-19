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

		// bgfx handles die with bgfx::shutdown(), and bgfx frees its own memory.
		// Anything still holding a handle after that - a function-local static, a
		// cached Ref, a leaked resource - would call bgfx::destroy on a dead
		// context and fault inside its mutex. Every resource destructor checks
		// this rather than trusting destruction order, which C++ does not
		// guarantee for statics relative to main().
		static bool IsGpuAlive();
		static void SetGpuAlive(bool alive); // BgfxContext only

		// bgfx completes work asynchronously, so anything waiting on the GPU
		// (currently entity-ID readback) needs to know which frame has landed.
		// BgfxContext reports it after each bgfx::frame().
		static void OnFrameSubmitted(uint32_t frameNumber);
		static uint32_t GetFrameNumber();

		// bgfx's built-in stats/debug-text overlay (F1 in the running app).
		static void SetDebugStatsEnabled(bool enabled);
		static bool IsDebugStatsEnabled();

		inline static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }

		// Every renderer subsystem now runs on bgfx: buffers, shaders, textures,
		// framebuffers, the ImGui backend, and - since the §5.2 rework - the
		// former uniform blocks, which are view transforms and vec4[] uniforms.
		//
		// Shaders that have not been ported yet simply produce an invalid program
		// and their draws are skipped, so the scene path is safe to run even
		// though most of it is still invisible.
		//
		// Kept as a named switch purely so this can be flipped back in one place
		// while the remaining shaders land; it goes away in Phase 7.
		inline static bool IsSceneRenderPathDormant() { return false; }
	};

}
