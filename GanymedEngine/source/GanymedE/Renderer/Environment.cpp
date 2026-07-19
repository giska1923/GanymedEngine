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
				GE_CORE_ASSERT(Next < RenderPass::ImGui, "Ran out of environment bake views!");
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

		bgfx::TextureHandle LoadEquirect(const std::string& filepath, uint32_t& outWidth, uint32_t& outHeight)
		{
			int w = 0, h = 0, channels = 0;
			stbi_set_flip_vertically_on_load(0);

			// Force 4 channels: bgfx has no 3-component float texture format.
			float* data = stbi_loadf(filepath.c_str(), &w, &h, &channels, 4);
			if (!data)
			{
				GE_CORE_ERROR("Failed to load HDR environment '{0}'", filepath);
				return BGFX_INVALID_HANDLE;
			}

			outWidth = (uint32_t)w;
			outHeight = (uint32_t)h;

			const bgfx::Memory* mem = bgfx::copy(data, (uint32_t)(w * h * 4 * sizeof(float)));
			stbi_image_free(data);

			return bgfx::createTexture2D((uint16_t)w, (uint16_t)h, false, 1,
				bgfx::TextureFormat::RGBA32F, BGFX_SAMPLER_UVW_CLAMP, mem);
		}

	}

	Environment::Environment(const std::string& filepath)
		: m_Filepath(filepath)
	{
		uint32_t srcWidth = 0, srcHeight = 0;
		bgfx::TextureHandle equirect = LoadEquirect(filepath, srcWidth, srcHeight);
		if (!bgfx::isValid(equirect))
			return;

		Bake(equirect);

		bgfx::destroy(equirect);

		m_Valid = bgfx::isValid(m_EnvCubemap)
			&& bgfx::isValid(m_Irradiance)
			&& bgfx::isValid(m_Prefilter)
			&& bgfx::isValid(m_BRDFLut);

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

		bgfx::TextureHandle handles[] = { m_EnvCubemap, m_Irradiance, m_Prefilter, m_BRDFLut };
		for (bgfx::TextureHandle h : handles)
		{
			if (bgfx::isValid(h))
				bgfx::destroy(h);
		}
	}

	void Environment::Bake(bgfx::TextureHandle equirect)
	{
		Ref<Shader> equirectShader = Shader::Create("assets/shaders/Equirect.glsl");
		Ref<Shader> irradianceShader = Shader::Create("assets/shaders/Irradiance.glsl");
		Ref<Shader> prefilterShader = Shader::Create("assets/shaders/Prefilter.glsl");
		Ref<Shader> brdfShader = Shader::Create("assets/shaders/BRDFLut.glsl");

		if (!equirectShader->IsValid() || !irradianceShader->IsValid()
			|| !prefilterShader->IsValid() || !brdfShader->IsValid())
		{
			GE_CORE_ERROR("IBL bake aborted: one or more bake shaders failed to load");
			return;
		}

		const uint64_t rtFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_UVW_CLAMP;

		m_EnvCubemap = bgfx::createTextureCube((uint16_t)kEnvSize, true, 1,
			bgfx::TextureFormat::RGBA16F, rtFlags);
		m_Irradiance = bgfx::createTextureCube((uint16_t)kIrradianceSize, false, 1,
			bgfx::TextureFormat::RGBA16F, rtFlags);
		m_Prefilter = bgfx::createTextureCube((uint16_t)kPrefilterSize, true, 1,
			bgfx::TextureFormat::RGBA16F, rtFlags);
		m_BRDFLut = bgfx::createTexture2D((uint16_t)kBRDFLutSize, (uint16_t)kBRDFLutSize, false, 1,
			bgfx::TextureFormat::RG16F, rtFlags);

		if (!bgfx::isValid(m_EnvCubemap) || !bgfx::isValid(m_Irradiance)
			|| !bgfx::isValid(m_Prefilter) || !bgfx::isValid(m_BRDFLut))
		{
			GE_CORE_ERROR("IBL bake aborted: could not create the target textures");
			return;
		}

		glm::mat4 captureViews[6];
		BuildCaptureViews(captureViews);
		const glm::mat4 captureProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

		Geometry cube = CreateUnitCube();
		Geometry quad = CreateFullscreenQuad();

		ViewAllocator views;
		std::vector<bgfx::FrameBufferHandle> framebuffers;

		// The bake writes render targets and samples them back in later stages of
		// the SAME frame. That is only safe because bgfx processes views in ID
		// order and every stage below takes higher IDs than the one it samples.
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

		// --- 2. Diffuse irradiance convolution --------------------------------
		for (uint16_t face = 0; face < 6; face++)
		{
			irradianceShader->SetTexture("u_EnvironmentMap", 0, m_EnvCubemap, BGFX_SAMPLER_UVW_CLAMP);
			renderCubeFace(m_Irradiance, face, 0, kIrradianceSize, irradianceShader);
		}

		// --- 3. Pre-filtered specular environment (one mip per roughness) -----
		for (uint16_t mip = 0; mip < (uint16_t)kPrefilterMips; mip++)
		{
			const uint32_t mipSize = kPrefilterSize >> mip;
			const float roughness = (float)mip / (float)(kPrefilterMips - 1);

			for (uint16_t face = 0; face < 6; face++)
			{
				prefilterShader->SetTexture("u_EnvironmentMap", 0, m_EnvCubemap, BGFX_SAMPLER_UVW_CLAMP);
				prefilterShader->SetFloat("u_Roughness", roughness);
				prefilterShader->SetFloat("u_Resolution", (float)kEnvSize);
				renderCubeFace(m_Prefilter, face, mip, mipSize, prefilterShader);
			}
		}

		// --- 4. BRDF integration LUT ------------------------------------------
		{
			const uint16_t view = views.Take();
			bgfx::Attachment attachment;
			attachment.init(m_BRDFLut, bgfx::Access::Write, 0, 1, 0);
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

			brdfShader->Bind();
			RenderCommand::DrawIndexed(quad);
		}

		// The bake is submitted, not executed: bgfx runs it when the frame is
		// presented. Two frames guarantee the work has been consumed before the
		// transient framebuffers are released (the textures themselves survive).
		bgfx::frame();
		bgfx::frame();

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

	Ref<Environment> Environment::Create(const std::string& filepath)
	{
		return CreateRef<Environment>(filepath);
	}

}
