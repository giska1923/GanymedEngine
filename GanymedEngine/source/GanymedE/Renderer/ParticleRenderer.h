#pragma once

#include "GanymedE/Scene/Components.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace GanymedE {

	// Billboard particle draws into view 5's sequential stream. Mesh particles do
	// not go through here — RenderSystem submits them as ordinary Phong opaques.
	//
	// Flush is called from Renderer3D::EndScene after the transparent mesh loop
	// and before the depth-write restore, so billboards inherit the real view
	// transform, depth-test LESS against opaques, depth-write already off, and
	// Sequential submit-order-is-execution-order. Cost of the seam: particles
	// always composite *after* transparent meshes (a smoke plume behind glass
	// draws over it). Interleaving would mean injecting into the transparent
	// sort; v1 accepts the artifact.
	class ParticleRenderer
	{
	public:
		static void Init();
		static void Shutdown();

		// Camera basis for this frame's CPU billboards. Call before Submit.
		static void SetView(const glm::vec3& position, const glm::vec3& right, const glm::vec3& up);

		static void Submit(int entityID, const ParticleEmitterComponent& emitter, const glm::mat4& world);

		// After transparent meshes. Restores RenderState even on transient-buffer
		// early-out — the sticky singleton is the leak hazard.
		static void Flush();

		// Probe only: pretends transient space holds at most this many quads.
		static void DebugLimitTransientQuads(uint32_t maxQuads);
	};

}
