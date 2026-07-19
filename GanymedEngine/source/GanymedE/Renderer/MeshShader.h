#pragma once

#include "GanymedE/Core/Core.h"

namespace GanymedE {

	class Shader;

	// The Phong program that every imported mesh material shares.
	//
	// This used to be a function-local `static Ref<Shader>` duplicated in
	// MeshCache.cpp and MeshImporter.cpp. Statics are destroyed after main()
	// returns - after bgfx::shutdown - so their handles were never released:
	// bgfx reported them as leaked blocks, and before the IsGpuAlive guard they
	// crashed outright. Ownership is explicit here and Renderer::Shutdown
	// releases it while bgfx is still alive.
	namespace MeshShader {

		Ref<Shader> Get();
		void Release();

	}

}
