#pragma once

#include "GanymedE/Core/Core.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>
#include <vector>

namespace GanymedE {

	// The equirectangular HDR, decoded and still on the CPU. RGBA32F because bgfx has no
	// three-component float texture format.
	//
	// This is `Environment`'s Parse product, and the reason it finally has a Parse stage at all.
	// The decode is ~25 ms for a 1K panorama (measured, Release) and is pure CPU work; it used to
	// run inside the constructor on the submit thread because that is where the bake has to be.
	struct EquirectImage
	{
		std::vector<float> Pixels;
		uint32_t Width = 0;
		uint32_t Height = 0;

		bool IsValid() const { return !Pixels.empty() && Width > 0 && Height > 0; }
	};

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
		// `image` is what `Load` produced. Main thread: this uploads it and submits the bake.
		Environment(const std::string& filepath, EquirectImage image);
		~Environment();

		Environment(const Environment&) = delete;
		Environment& operator=(const Environment&) = delete;

		bgfx::TextureHandle GetSkybox() const { return m_EnvCubemap; }
		bgfx::TextureHandle GetIrradiance() const { return m_Irradiance; }
		bgfx::TextureHandle GetPrefilter() const { return m_Prefilter; }

		// **Shared by every environment, and owned by none of them.** The LUT is the split-sum
		// approximation's second term: a pure function of the BRDF over (NdotV, roughness), with
		// no dependence on the HDR at all. Baking it per environment produced an identical
		// 512x512 texture every time, at the cost of a view, a framebuffer and a draw per load.
		bgfx::TextureHandle GetBRDFLut() const;

		float GetMaxReflectionLod() const { return (float)(kPrefilterMips - 1); }
		bool IsValid() const { return m_Valid; }
		const std::string& GetFilepath() const { return m_Filepath; }

		// **Worker thread.** stb decode only - no bgfx call is reachable from here, which is the
		// contract the asset layer's Parse stage depends on.
		static EquirectImage Load(const std::string& filepath);

		static Ref<Environment> Create(const std::string& filepath, EquirectImage image);

		// The four bake programs and the shared BRDF LUT outlive any one environment. Released by
		// Renderer::Shutdown while bgfx is still alive - static destruction runs after
		// bgfx::shutdown, which is how these leak or crash if left to it (see MeshShader.h).
		static void ReleaseSharedResources();
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
	};

}
