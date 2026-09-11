#pragma once

#include "RenderCommand.h"
#include "RendererAPI.h"

#include "GanymedE/Renderer/Shader.h"

namespace GanymedE {

	// Projection matrices that match the running backend's clip space.
	//
	// BGFX_MIGRATION.md §9.3. The workspace compiles with `GLM_FORCE_DEPTH_ZERO_TO_ONE`, so every
	// `glm::perspective` in the engine produced a matrix for `[0,1]` clip depth - correct on
	// D3D, Vulkan and Metal, and wrong on OpenGL, which wants `[-1,1]`. Being a compile-time
	// define, it could not be wrong *loudly*: a GL build would have rendered with incorrect
	// near-plane clipping and half its depth precision, which reads as a subtle artifact rather
	// than a failure. `BgfxContext` logs an error when it detects the mismatch; these functions
	// are what make the error unnecessary.
	//
	// Built with glm's convention-explicit forms (`perspectiveRH_ZO` / `perspectiveRH_NO`), which
	// are what `glm::perspective` already resolves to depending on the define. That makes the
	// `[0,1]` branch identical to the call it replaces *by construction*, and adds the `[-1,1]`
	// branch the define could never express.
	namespace Projection {

		// True when the backend wants `[-1,1]` clip depth (OpenGL) rather than `[0,1]`.
		//
		// **False before the GPU is up**, which is deliberate: that is what
		// `GLM_FORCE_DEPTH_ZERO_TO_ONE` meant, so a camera constructed before `Renderer::Init`
		// gets exactly the matrix it got before this existed. Every camera recomputes on resize
		// and on any setter, so nothing keeps a pre-init matrix into a rendered frame.
		bool HomogeneousDepth();

		// True when render-target texel (0,0) is bottom-left (OpenGL) rather than top-left.
		// Shaders answer this per profile with `BGFX_SHADER_LANGUAGE_GLSL` instead - the shading
		// language *is* the backend there - so this is for C++ that needs the same fact.
		bool OriginBottomLeft();

		glm::mat4 Perspective(float fovYRadians, float aspect, float nearZ, float farZ);
		glm::mat4 Orthographic(float left, float right, float bottom, float top,
			float nearZ, float farZ);

	}

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
