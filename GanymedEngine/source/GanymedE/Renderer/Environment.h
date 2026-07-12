#pragma once

#include "GanymedE/Core/Core.h"

#include <string>

namespace GanymedE {

	// An image-based lighting environment baked from an equirectangular HDR:
	// a filtered environment cubemap (skybox), a diffuse irradiance map, a
	// pre-filtered specular map, and a shared BRDF integration LUT.
	class Environment
	{
	public:
		virtual ~Environment() = default;

		// irradiance/prefilter are samplerCube slots, brdfLut is a sampler2D slot
		virtual void BindIBL(uint32_t irradianceSlot, uint32_t prefilterSlot, uint32_t brdfLutSlot) const = 0;
		virtual void BindSkybox(uint32_t slot) const = 0;

		virtual float GetMaxReflectionLod() const = 0;
		virtual bool IsValid() const = 0;
		virtual const std::string& GetFilepath() const = 0;

		static Ref<Environment> Create(const std::string& filepath);
	};

}
