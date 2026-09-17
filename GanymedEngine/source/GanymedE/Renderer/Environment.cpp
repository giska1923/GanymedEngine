#include "gepch.h"
#include "GanymedE/Renderer/Environment.h"

#include "GanymedE/Renderer/Buffer.h"
#include "GanymedE/Renderer/RenderCommand.h"
#include "GanymedE/Renderer/RenderPassIDs.h"
#include "GanymedE/Renderer/Renderer.h"
#include "GanymedE/Renderer/Shader.h"

#include "stb_image.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>

namespace GanymedE {

	namespace {

		// Views are consumed strictly in increasing order so each stage reads what
		// the previous one wrote: bgfx processes a frame's views in ID order.
		struct ViewAllocator
		{
			uint16_t Next = RenderPass::EnvironmentBake;

			uint16_t Take()
			{
				GE_CORE_ASSERT(Next < RenderPass::Shadow,
					"Ran out of environment bake views - raise EnvironmentBakeViewCount and shift "
					"the passes after it in RenderPassIDs.h");
				return Next++;
			}
		};

		// The six cube faces, looking down each axis. Matches the GL bake exactly.
		void BuildCaptureViews(glm::mat4 (&views)[6])
		{
			const glm::vec3 o(0.0f);
			views[0] = glm::lookAt(o, glm::vec3( 1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f));
			views[1] = glm::lookAt(o, glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f));
			views[2] = glm::lookAt(o, glm::vec3( 0.0f,  1.0f,  0.0f), glm::vec3(0.0f,  0.0f,  1.0f));
			views[3] = glm::lookAt(o, glm::vec3( 0.0f, -1.0f,  0.0f), glm::vec3(0.0f,  0.0f, -1.0f));
			views[4] = glm::lookAt(o, glm::vec3( 0.0f,  0.0f,  1.0f), glm::vec3(0.0f, -1.0f,  0.0f));
			views[5] = glm::lookAt(o, glm::vec3( 0.0f,  0.0f, -1.0f), glm::vec3(0.0f, -1.0f,  0.0f));
		}

		Geometry CreateUnitCube()
		{
			// Positions only - the bake shaders use the local position as the
			// sampling direction.
			const float v[] = {
				-1,-1,-1,  1,-1,-1,  1, 1,-1, -1, 1,-1,
				-1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,
			};
			const uint32_t i[] = {
				0,1,2, 2,3,0,  4,5,6, 6,7,4,
				0,4,7, 7,3,0,  1,5,6, 6,2,1,
				3,2,6, 6,7,3,  0,1,5, 5,4,0,
			};

			Geometry g;
			g.Vertices = VertexBuffer::Create(v, sizeof(v), { { ShaderDataType::Float3, "a_Position" } });
			g.Indices = IndexBuffer::Create(i, (uint32_t)(sizeof(i) / sizeof(i[0])));
			return g;
		}

		Geometry CreateFullscreenQuad()
		{
			const float v[] = { -1,-1,  1,-1,  1, 1, -1, 1 };
			const uint32_t i[] = { 0,1,2, 2,3,0 };

			Geometry g;
			g.Vertices = VertexBuffer::Create(v, sizeof(v), { { ShaderDataType::Float2, "a_Position" } });
			g.Indices = IndexBuffer::Create(i, 6);
			return g;
		}

		// A framebuffer targeting one face (and mip) of a cubemap.
		bgfx::FrameBufferHandle FaceFramebuffer(bgfx::TextureHandle cubemap, uint16_t face, uint16_t mip)
		{
			bgfx::Attachment attachment;
			attachment.init(cubemap, bgfx::Access::Write, face, 1, mip);
			return bgfx::createFrameBuffer(1, &attachment, false);
		}

		bgfx::TextureHandle UploadEquirect(const EquirectImage& image)
		{
			const bgfx::Memory* mem = bgfx::copy(image.Pixels.data(),
				(uint32_t)(image.Pixels.size() * sizeof(float)));

			return bgfx::createTexture2D((uint16_t)image.Width, (uint16_t)image.Height, false, 1,
				bgfx::TextureFormat::RGBA32F, BGFX_SAMPLER_UVW_CLAMP, mem);
		}

		// Shared by every bake. Recreating the four programs per environment cost 1.6-1.8 ms of
		// file IO and shader creation for an identical result, and the BRDF LUT below is a whole
		// render target and draw for an identical result. Both are held until Renderer::Shutdown
		// releases them - see Environment::ReleaseSharedResources.
		struct SharedBakeResources
		{
			Ref<Shader> Equirect;
			Ref<Shader> Irradiance;
			Ref<Shader> Prefilter;
			Ref<Shader> BRDF;

			// Baked once, by whichever environment loads first.
			bgfx::TextureHandle BRDFLut = BGFX_INVALID_HANDLE;

			bool ShadersValid() const
			{
				return Equirect && Equirect->IsValid() && Irradiance && Irradiance->IsValid()
					&& Prefilter && Prefilter->IsValid() && BRDF && BRDF->IsValid();
			}
		};

		SharedBakeResources& Shared()
		{
			static SharedBakeResources s_Shared;
			return s_Shared;
		}

		bool EnsureBakeShaders()
		{
			SharedBakeResources& shared = Shared();
			if (shared.ShadersValid())
				return true;

			shared.Equirect = Shader::Create("assets/shaders/Equirect.glsl");
			shared.Irradiance = Shader::Create("assets/shaders/Irradiance.glsl");
			shared.Prefilter = Shader::Create("assets/shaders/Prefilter.glsl");
			shared.BRDF = Shader::Create("assets/shaders/BRDFLut.glsl");

			return shared.ShadersValid();
		}

	}

	EquirectImage Environment::Load(const std::string& filepath)
	{
		GE_PROFILE_FUNCTION();

		EquirectImage image;

		int w = 0, h = 0, channels = 0;

		// Force 4 channels: bgfx has no 3-component float texture format. stb's flip global is
		// deliberately not touched - see TextureImporter.h; the default orientation is the one
		// this path has always wanted.
		float* data = stbi_loadf(filepath.c_str(), &w, &h, &channels, 4);
		if (!data)
		{
			GE_CORE_ERROR("Failed to load HDR environment '{0}'", filepath);
			return image;
		}

		image.Width = (uint32_t)w;
		image.Height = (uint32_t)h;

		// One copy out of stb's buffer, so the result owns itself and can be moved through the
		// asset layer's parse result. ~8 MB for a 1K panorama against a ~25 ms decode.
		image.Pixels.assign(data, data + (std::size_t)w * h * 4);
		stbi_image_free(data);

		return image;

	}

	bgfx::TextureHandle Environment::GetBRDFLut() const { return Shared().BRDFLut; }

	void Environment::ReleaseSharedResources()
	{
		SharedBakeResources& shared = Shared();

		if (Renderer::IsGpuAlive() && bgfx::isValid(shared.BRDFLut))
			bgfx::destroy(shared.BRDFLut);

		shared.BRDFLut = BGFX_INVALID_HANDLE;
		shared.Equirect = nullptr;
		shared.Irradiance = nullptr;
		shared.Prefilter = nullptr;
		shared.BRDF = nullptr;
	}

	Environment::Environment(const std::string& filepath, EquirectImage image)
		: m_Filepath(filepath)
	{
		if (!image.IsValid())
		{
			GE_CORE_ERROR("IBL bake aborted: no image was decoded for '{0}'", filepath);
			return;
		}

		const uint32_t srcWidth = image.Width;
		const uint32_t srcHeight = image.Height;

		bgfx::TextureHandle equirect = UploadEquirect(image);
		if (!bgfx::isValid(equirect))
			return;

		Bake(equirect);

		bgfx::destroy(equirect);

		m_Valid = bgfx::isValid(m_EnvCubemap)
			&& bgfx::isValid(m_Irradiance)
			&& bgfx::isValid(m_Prefilter)
			&& bgfx::isValid(Shared().BRDFLut);

		if (m_Valid)
			GE_CORE_INFO("Baked IBL environment '{0}' ({1}x{2} source)", filepath, srcWidth, srcHeight);
		else
			GE_CORE_ERROR("IBL bake failed for '{0}'", filepath);
	}

	Environment::~Environment()
	{
		// See Renderer::IsGpuAlive - this object may outlive bgfx.
		if (!Renderer::IsGpuAlive())
			return;

		// Not the BRDF LUT: it is shared and Renderer::Shutdown owns its lifetime.
		bgfx::TextureHandle handles[] = { m_EnvCubemap, m_Irradiance, m_Prefilter };
		for (bgfx::TextureHandle h : handles)
		{
			if (bgfx::isValid(h))
				bgfx::destroy(h);
		}
	}

	void Environment::Bake(bgfx::TextureHandle equirect)
	{
		if (!EnsureBakeShaders())
		{
			GE_CORE_ERROR("IBL bake aborted: one or more bake shaders failed to load");
			return;
		}

		SharedBakeResources& shared = Shared();
		const Ref<Shader>& equirectShader = shared.Equirect;
		const Ref<Shader>& irradianceShader = shared.Irradiance;
		const Ref<Shader>& prefilterShader = shared.Prefilter;

		const uint64_t rtFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_UVW_CLAMP;

		m_EnvCubemap = bgfx::createTextureCube((uint16_t)kEnvSize, true, 1,
			bgfx::TextureFormat::RGBA16F, rtFlags);
		m_Irradiance = bgfx::createTextureCube((uint16_t)kIrradianceSize, false, 1,
			bgfx::TextureFormat::RGBA16F, rtFlags);
		m_Prefilter = bgfx::createTextureCube((uint16_t)kPrefilterSize, true, 1,
			bgfx::TextureFormat::RGBA16F, rtFlags);

		// Only the first environment pays for this one.
		const bool bakeLut = !bgfx::isValid(shared.BRDFLut);
		if (bakeLut)
		{
			shared.BRDFLut = bgfx::createTexture2D((uint16_t)kBRDFLutSize, (uint16_t)kBRDFLutSize,
				false, 1, bgfx::TextureFormat::RG16F, rtFlags);
		}

		if (!bgfx::isValid(m_EnvCubemap) || !bgfx::isValid(m_Irradiance)
			|| !bgfx::isValid(m_Prefilter) || !bgfx::isValid(shared.BRDFLut))
		{
			GE_CORE_ERROR("IBL bake aborted: could not create the target textures");
			return;
		}

		glm::mat4 captureViews[6];
		BuildCaptureViews(captureViews);
		// The cubemap bake renders through real projections like any other pass, so it needs
		// the backend's clip convention too - a bake that clipped wrongly would poison every
		// scene lit by the resulting IBL.
		const glm::mat4 captureProj = Projection::Perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

		Geometry cube = CreateUnitCube();
		Geometry quad = CreateFullscreenQuad();

		ViewAllocator views;
		std::vector<bgfx::FrameBufferHandle> framebuffers;

		// Intel ANV (Mesa on Linux, and Windows Intel Vulkan) has hung the GPU
		// (`VK_ERROR_DEVICE_LOST`, i915 "GPU hung on one of our command buffers")
		// when the whole bake - irradiance hemisphere, 1024-sample GGX prefilter,
		// 512^2 BRDF LUT - plus the first editor frame went out as one submit.
		// Discrete GPUs finish that in well under a vsync; ANV's compiler and
		// i915's hangcheck do not. Split only there so D3D11/NVIDIA keep the
		// single-frame bake.
		const bgfx::Caps* caps = bgfx::getCaps();
		const bool splitBakeStages = caps
			&& caps->rendererType == bgfx::RendererType::Vulkan
			&& caps->vendorId == BGFX_PCI_ID_INTEL;

		// Named and timed, because an Intel ANV hang inside the bake is otherwise a silence:
		// the log stops after the four target textures are created and the next thing in it is
		// `GPU hung on one of our command buffers`, 14 seconds later, with no way to tell which
		// convolution the GPU died in.
		//
		// **One bgfx::frame() does not wait for the GPU, and a first version of this assumed it
		// did.** bgfx returns as soon as it can begin the next frame, which against a 3-image
		// swapchain is two to three frames of slack - so the flush that blocks is two or three
		// stages downstream of the one the GPU is actually stuck on, and a stage can report
		// 0.7 ms for 25M cube samples because it never waited for any of them. A run read that
		// way blames the wrong convolution.
		//
		// kDrainFrames extra empty frames force the wait. With three swapchain images, acquiring
		// the fourth cannot succeed until the first is free, which requires this stage's work to
		// have finished. That makes the reported time real GPU time and, more importantly, makes
		// a hang block in the stage that caused it.
		//
		// Only on the split path, and only ever at load: three empty frames per stage is a few
		// vsyncs of extra hitch on the one boot that bakes an environment, in exchange for a
		// diagnosis that is not a guess.
		constexpr int kDrainFrames = 3;

		auto flushBakeStage = [&](const char* stage)
		{
			if (!splitBakeStages)
				return;

			const auto begin = std::chrono::steady_clock::now();

			// Keep Renderer::GetFrameNumber() in lockstep with bgfx; picking
			// polls that, not bgfx's own counter.
			Renderer::OnFrameSubmitted(bgfx::frame());
			views.Next = RenderPass::EnvironmentBake;

			for (int i = 0; i < kDrainFrames; i++)
				Renderer::OnFrameSubmitted(bgfx::frame());

			const double ms = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - begin).count();
			GE_CORE_INFO("IBL bake stage '{0}': {1:.1f} ms (drained)", stage, ms);
		};

		// When splitBakeStages is false, later stages sample earlier ones in the
		// SAME frame - that is only safe because bgfx processes views in ID order
		// and every stage below takes higher IDs than the one it samples. When it
		// is true, flushBakeStage makes the hazard a cross-frame one instead.
		auto renderCubeFace = [&](bgfx::TextureHandle target, uint16_t face, uint16_t mip,
			uint32_t size, const Ref<Shader>& shader)
		{
			const uint16_t view = views.Take();
			bgfx::FrameBufferHandle fb = FaceFramebuffer(target, face, mip);
			framebuffers.push_back(fb);

			bgfx::setViewFrameBuffer(view, fb);
			bgfx::setViewRect(view, 0, 0, (uint16_t)size, (uint16_t)size);
			bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
			bgfx::setViewTransform(view, &captureViews[face][0][0], &captureProj[0][0]);

			RenderCommand::SetViewId(view);
			RenderCommand::SetDepthTest(false);
			RenderCommand::SetDepthWrite(false);
			// The cube is viewed from the inside, so no culling.
			RenderCommand::SetCullFace(false);

			shader->Bind();
			RenderCommand::DrawIndexed(cube);
		};

		// --- 1. Equirectangular panorama -> environment cubemap ---------------
		// Every mip is rendered from the panorama: bgfx cannot generate mipmaps
		// for a render target, and the prefilter stage samples these by roughness.
		for (uint16_t mip = 0; mip < (uint16_t)kEnvMips; mip++)
		{
			const uint32_t mipSize = kEnvSize >> mip;
			for (uint16_t face = 0; face < 6; face++)
			{
				equirectShader->SetTexture("u_EquirectangularMap", 0, equirect, BGFX_SAMPLER_UVW_CLAMP);
				renderCubeFace(m_EnvCubemap, face, mip, mipSize, equirectShader);
			}
		}
		flushBakeStage("equirect");

		// --- 2. Diffuse irradiance convolution --------------------------------
		for (uint16_t face = 0; face < 6; face++)
		{
			irradianceShader->SetTexture("u_EnvironmentMap", 0, m_EnvCubemap, BGFX_SAMPLER_UVW_CLAMP);
			renderCubeFace(m_Irradiance, face, 0, kIrradianceSize, irradianceShader);
		}
		flushBakeStage("irradiance");

		// --- 3. Pre-filtered specular environment (one mip per roughness) -----
		for (uint16_t mip = 0; mip < (uint16_t)kPrefilterMips; mip++)
		{
			const uint32_t mipSize = kPrefilterSize >> mip;
			const float roughness = (float)mip / (float)(kPrefilterMips - 1);

			for (uint16_t face = 0; face < 6; face++)
			{
				prefilterShader->SetTexture("u_EnvironmentMap", 0, m_EnvCubemap, BGFX_SAMPLER_UVW_CLAMP);
				prefilterShader->SetFloat("u_Roughness", roughness);
				// .y is the highest LOD that exists in the source chain. The shader clamps to it;
				// without that it asks for levels two to three times past the end.
				prefilterShader->SetFloat2("u_Resolution",
					glm::vec2((float)kEnvSize, (float)(kEnvMips - 1)));
				renderCubeFace(m_Prefilter, face, mip, mipSize, prefilterShader);
			}

			// Per mip, not per stage. Prefilter is where the ANV hang lives, and five submits
			// instead of one is what turns "it died somewhere in the specular convolution" into a
			// roughness value. Mip 0 is the one case where the shader forces LOD 0, so a hang
			// there would rule the LOD maths out and a hang after it would not.
			char label[48];
			std::snprintf(label, sizeof(label), "prefilter mip%u (roughness %.2f)",
				(unsigned)mip, roughness);
			flushBakeStage(label);
		}

		// --- 4. BRDF integration LUT (once per process, not once per environment) ---
		if (bakeLut)
		{
			const uint16_t view = views.Take();
			bgfx::Attachment attachment;
			attachment.init(shared.BRDFLut, bgfx::Access::Write, 0, 1, 0);
			bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(1, &attachment, false);
			framebuffers.push_back(fb);

			bgfx::setViewFrameBuffer(view, fb);
			bgfx::setViewRect(view, 0, 0, (uint16_t)kBRDFLutSize, (uint16_t)kBRDFLutSize);
			bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);

			const glm::mat4 identity(1.0f);
			bgfx::setViewTransform(view, &identity[0][0], &identity[0][0]);

			RenderCommand::SetViewId(view);
			RenderCommand::SetDepthTest(false);
			RenderCommand::SetDepthWrite(false);
			RenderCommand::SetCullFace(false);

			shared.BRDF->Bind();
			RenderCommand::DrawIndexed(quad);
			flushBakeStage("brdf-lut");
		}

		// The bake is submitted, not executed: bgfx runs it when the frame is presented.
		// On non-Intel-Vulkan that is the same frame as the scene (views sort first, see
		// RenderPassIDs.h). On Intel + Vulkan the flushes above already presented it.
		//
		// Destroying a framebuffer is deferred by bgfx until the frame that used it has
		// been rendered, so the handles below can go back immediately; the cube textures
		// they wrote into are owned by this object and survive.
		for (bgfx::FrameBufferHandle fb : framebuffers)
		{
			if (bgfx::isValid(fb))
				bgfx::destroy(fb);
		}

		// Leave the renderer in a sane state for the frame that follows.
		RenderCommand::SetViewId(RenderPass::SceneHDR);
		RenderCommand::SetDepthTest(true);
		RenderCommand::SetDepthWrite(true);
		RenderCommand::SetCullFace(true);
	}

	Ref<Environment> Environment::Create(const std::string& filepath, EquirectImage image)
	{
		return CreateRef<Environment>(filepath, std::move(image));
	}

}
