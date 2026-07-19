#pragma once

#include <glm/glm.hpp>

namespace GanymedE {

	// Replaces the std140 uniform blocks (CameraUBO / LightsUBO) that bgfx has no
	// equivalent for. See docs/toDo&done/BGFX_MIGRATION.md §5.2.
	//
	// IMPORTANT - bgfx uniforms are per-DRAW, not per-frame.
	//
	// §5.2 describes setting these "once per frame/view", which is not how bgfx
	// behaves: bgfx::setUniform contributes to the *next* submit and is cleared
	// by it. Setting the same uniform twice before a submit is a hard assert
	// ("Uniform N was already set for this draw call"), which is exactly what
	// happens when a draw is skipped and never consumes the pending state.
	//
	// So the setters here only record values CPU-side. Apply() pushes them, and
	// RenderCommand calls it immediately before every submit. The one exception
	// is the camera matrices: bgfx::setViewTransform is genuine view state that
	// persists across draws, so it is applied immediately and feeds the
	// predefined u_view / u_proj / u_viewProj / u_modelViewProj uniforms - which
	// is why only the camera *position* needs a uniform of its own.
	namespace FrameUniforms {

		void Init();
		void Shutdown();

		// Applies the view transform immediately; records the camera position.
		void SetCamera(uint16_t viewId, const glm::mat4& view, const glm::mat4& projection,
			const glm::vec3& cameraPosition);

		// All recorded, matching the old LightsUBO members of the same names.
		void SetDirectionalLight(const glm::vec4& direction, const glm::vec4& color);
		void SetAmbient(const glm::vec4& sky, const glm::vec4& ground);

		// Four vec4s per light (position, direction, colour, spot) - the old
		// GPULight layout unchanged. `count` is the live light count; the array
		// uniform is always sized at maxLights because bgfx fixes an array's
		// size when the uniform is created.
		void SetLights(const glm::vec4* packed, uint32_t count, uint32_t maxLights);

		// Pushes every recorded value. Called by RenderCommand before each submit.
		void Apply();

	}

}
