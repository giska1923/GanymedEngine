#include "gepch.h"
#include "SceneRenderer.h"

#include "GanymedE/Renderer/PostProcess.h"
#include "GanymedE/Renderer/RenderCommand.h"
#include "GanymedE/Renderer/RenderPassIDs.h"

#include <algorithm>

namespace GanymedE {

	static constexpr uint32_t kMaxBloomMips = 6;
	static constexpr uint32_t kMinBloomMipSize = 8;

	// bgfx view clears take a packed 0xRRGGBBAA word rather than four floats.
	static uint32_t PackRGBA(const glm::vec4& c)
	{
		auto channel = [](float v) -> uint32_t
		{
			return (uint32_t)(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
		};

		return (channel(c.r) << 24) | (channel(c.g) << 16) | (channel(c.b) << 8) | channel(c.a);
	}

	SceneRenderer::SceneRenderer(uint32_t width, uint32_t height)
		: m_Width(std::max(width, 1u)), m_Height(std::max(height, 1u))
	{
		FramebufferSpecification sceneSpec;
		sceneSpec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::Depth };
		sceneSpec.Width = m_Width;
		sceneSpec.Height = m_Height;
		m_SceneFramebuffer = Framebuffer::Create(sceneSpec);

		FramebufferSpecification ldrSpec;
		ldrSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
		ldrSpec.Width = m_Width;
		ldrSpec.Height = m_Height;
		m_TonemapFramebuffer = Framebuffer::Create(ldrSpec);
		m_CompositeFramebuffer = Framebuffer::Create(ldrSpec);

		m_BloomDownsampleShader = Shader::Create("assets/shaders/BloomDownsample.glsl");
		m_BloomUpsampleShader = Shader::Create("assets/shaders/BloomUpsample.glsl");
		m_FXAAShader = Shader::Create("assets/shaders/FXAA.glsl");

		RebuildBloomChain();
	}

	void SceneRenderer::SetViewportSize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0 || (width == m_Width && height == m_Height))
			return;

		m_Width = width;
		m_Height = height;

		m_SceneFramebuffer->Resize(width, height);
		m_TonemapFramebuffer->Resize(width, height);
		m_CompositeFramebuffer->Resize(width, height);
		RebuildBloomChain();
	}

	void SceneRenderer::RebuildBloomChain()
	{
		m_BloomMips.clear();

		uint32_t mipWidth = m_Width / 2;
		uint32_t mipHeight = m_Height / 2;

		FramebufferSpecification mipSpec;
		mipSpec.Attachments = { FramebufferTextureFormat::RGBA16F };

		for (uint32_t i = 0; i < kMaxBloomMips; i++)
		{
			if (mipWidth < kMinBloomMipSize || mipHeight < kMinBloomMipSize)
				break;

			mipSpec.Width = mipWidth;
			mipSpec.Height = mipHeight;
			m_BloomMips.push_back(Framebuffer::Create(mipSpec));

			mipWidth /= 2;
			mipHeight /= 2;
		}
	}

	void SceneRenderer::BeginFrame()
	{
		// Under bgfx a pass is a view, not a bound framebuffer: point the scene
		// view at the HDR target and everything submitted to it lands there.
		m_SceneFramebuffer->BindToView(RenderPass::SceneHDR);
		RenderCommand::SetViewId(RenderPass::SceneHDR);

		// The clear is view state rather than an immediate command. Clearing the
		// entity-ID attachment to "no entity" (-1) comes along with it: bgfx
		// clears every attachment of the view at once, so the old separate
		// ClearAttachment call has no equivalent and is not needed.
		const glm::vec4& c = m_Settings.ClearColor;
		bgfx::setViewClear(RenderPass::SceneHDR,
			BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
			PackRGBA(c), 1.0f, 0);
	}

	Ref<Framebuffer> SceneRenderer::RenderBloom()
	{
		if (m_BloomMips.size() < 2 || !m_BloomDownsampleShader || !m_BloomUpsampleShader)
			return nullptr;

		RenderCommand::SetBlend(false);

		// Downsample: HDR scene -> mip0 (thresholded) -> mip1 -> ...
		m_BloomDownsampleShader->Bind();
		m_BloomDownsampleShader->SetFloat("u_Threshold", m_Settings.BloomThreshold);
		m_BloomDownsampleShader->SetFloat("u_Knee", glm::max(m_Settings.BloomKnee, 0.01f));

		for (size_t i = 0; i < m_BloomMips.size(); i++)
		{
			const Ref<Framebuffer>& source = (i == 0) ? m_SceneFramebuffer : m_BloomMips[i - 1];
			const auto& sourceSpec = source->GetSpecification();

			// One view per mip so bgfx keeps the chain strictly ordered.
			const uint16_t view = RenderPass::BloomDownsample + (uint16_t)i;
			m_BloomMips[i]->BindToView(view);
			RenderCommand::SetViewId(view);

			m_BloomDownsampleShader->SetTexture("u_Texture", 0, source->GetColorAttachment(0), BGFX_SAMPLER_UVW_CLAMP);
			m_BloomDownsampleShader->SetInt("u_FirstPass", i == 0 ? 1 : 0);
			m_BloomDownsampleShader->SetFloat2("u_TexelSize",
				glm::vec2(1.0f / (float)sourceSpec.Width, 1.0f / (float)sourceSpec.Height));
			PostProcess::DrawFullscreenQuad();
		}

		// Upsample: additively blend each smaller mip onto the next-larger one
		RenderCommand::SetBlend(true);
		RenderCommand::SetBlendMode(RenderState::BlendMode::Additive);

		m_BloomUpsampleShader->Bind();
		m_BloomUpsampleShader->SetFloat("u_FilterRadius", m_Settings.BloomFilterRadius);

		for (size_t i = m_BloomMips.size() - 1; i > 0; i--)
		{
			const Ref<Framebuffer>& source = m_BloomMips[i];
			const auto& sourceSpec = source->GetSpecification();

			const uint16_t view = RenderPass::BloomUpsample + (uint16_t)i;
			m_BloomMips[i - 1]->BindToView(view);
			RenderCommand::SetViewId(view);

			m_BloomUpsampleShader->SetTexture("u_Texture", 0, source->GetColorAttachment(0), BGFX_SAMPLER_UVW_CLAMP);
			m_BloomUpsampleShader->SetFloat2("u_TexelSize",
				glm::vec2(1.0f / (float)sourceSpec.Width, 1.0f / (float)sourceSpec.Height));
			PostProcess::DrawFullscreenQuad();
		}

		RenderCommand::SetBlendMode(RenderState::BlendMode::Alpha);
		return m_BloomMips[0];
	}

	void SceneRenderer::EndFrame()
	{
		Ref<Framebuffer> bloom;
		if (m_Settings.BloomEnabled)
			bloom = RenderBloom();

		RenderCommand::SetBlend(false);

		// Tonemap HDR (+ bloom) into the FXAA input, or straight to the composite
		const Ref<Framebuffer>& tonemapTarget = m_Settings.FXAAEnabled ? m_TonemapFramebuffer : m_CompositeFramebuffer;
		tonemapTarget->BindToView(RenderPass::Tonemap);
		RenderCommand::SetViewId(RenderPass::Tonemap);
		bgfx::setViewClear(RenderPass::Tonemap, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
		PostProcess::Tonemap(m_SceneFramebuffer, m_Settings.Exposure, bloom, m_Settings.BloomIntensity);

		if (m_Settings.FXAAEnabled && m_FXAAShader)
		{
			m_CompositeFramebuffer->BindToView(RenderPass::FXAA);
			RenderCommand::SetViewId(RenderPass::FXAA);
			bgfx::setViewClear(RenderPass::FXAA, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);

			m_FXAAShader->Bind();
			m_FXAAShader->SetTexture("u_Texture", 0, m_TonemapFramebuffer->GetColorAttachment(0), BGFX_SAMPLER_UVW_CLAMP);
			m_FXAAShader->SetFloat2("u_TexelSize",
				glm::vec2(1.0f / (float)m_Width, 1.0f / (float)m_Height));
			PostProcess::DrawFullscreenQuad();
		}

		RenderCommand::SetBlend(true);
	}

	int SceneRenderer::ReadEntityID(int x, int y)
	{
		// bgfx cannot read a render target synchronously. Phase 5 turns this into
		// a request/poll pair (docs/BGFX_MIGRATION.md §7); until then picking is
		// disabled rather than silently returning a stale value.
		(void)x; (void)y;
		return -1;
	}

	uint32_t SceneRenderer::GetFinalImageRendererID() const
	{
		return m_CompositeFramebuffer->GetColorAttachmentRendererID();
	}

}
