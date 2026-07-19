#pragma once

#include "GanymedE/Core/Core.h"

#include <bgfx/bgfx.h>

#include <string>

namespace GanymedE {

	// An image-based lighting environment baked from an equirectangular HDR:
	// a filtered environment cubemap (skybox), a diffuse irradiance map, a
	// pre-filtered specular map, and a shared BRDF integration LUT.
	//
	// Concrete over bgfx handles, like the other resources. The bake runs once on
	// construction across a block of transient views (RenderPass::EnvironmentBake),
	// rendering into individual cubemap faces and mips via bgfx::Attachment.
	//
	// Binding is the caller's job: a sampler belongs to a shader, so the handles
	// are exposed and Renderer3D feeds them to Shader::SetTexture.
	class Environment
	{
	public:
		explicit Environment(const std::string& filepath);
		~Environment();

		Environment(const Environment&) = delete;
		Environment& operator=(const Environment&) = delete;

		bgfx::TextureHandle GetSkybox() const { return m_EnvCubemap; }
		bgfx::TextureHandle GetIrradiance() const { return m_Irradiance; }
		bgfx::TextureHandle GetPrefilter() const { return m_Prefilter; }
		bgfx::TextureHandle GetBRDFLut() const { return m_BRDFLut; }

		float GetMaxReflectionLod() const { return (float)(kPrefilterMips - 1); }
		bool IsValid() const { return m_Valid; }
		const std::string& GetFilepath() const { return m_Filepath; }

		static Ref<Environment> Create(const std::string& filepath);
	private:
		void Bake(bgfx::TextureHandle equirect);
	private:
		static constexpr uint32_t kEnvSize = 512;
		static constexpr uint32_t kEnvMips = 5;   // prefilter samples these by roughness
		static constexpr uint32_t kIrradianceSize = 32;
		static constexpr uint32_t kPrefilterSize = 128;
		static constexpr uint32_t kPrefilterMips = 5;
		static constexpr uint32_t kBRDFLutSize = 512;

		std::string m_Filepath;
		bool m_Valid = false;

		bgfx::TextureHandle m_EnvCubemap = BGFX_INVALID_HANDLE;
		bgfx::TextureHandle m_Irradiance = BGFX_INVALID_HANDLE;
		bgfx::TextureHandle m_Prefilter = BGFX_INVALID_HANDLE;
		bgfx::TextureHandle m_BRDFLut = BGFX_INVALID_HANDLE;
	};

}
