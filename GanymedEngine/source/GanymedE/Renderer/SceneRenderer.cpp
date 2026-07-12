#include "gepch.h"
#include "SceneRenderer.h"

#include "GanymedE/Renderer/PostProcess.h"
#include "GanymedE/Renderer/RenderCommand.h"

#include <algorithm>

namespace GanymedE {

	static constexpr uint32_t kMaxBloomMips = 6;
	static constexpr uint32_t kMinBloomMipSize = 8;

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
		m_SceneFramebuffer->Bind();
		RenderCommand::SetClearColor(m_Settings.ClearColor);
		RenderCommand::Clear();

		// Clear the entity ID attachment to "no entity"
		m_SceneFramebuffer->ClearAttachment(1, -1);
	}

	Ref<Framebuffer> SceneRenderer::RenderBloom()
	{
		if (m_BloomMips.size() < 2 || !m_BloomDownsampleShader || !m_BloomUpsampleShader)
			return nullptr;

		RenderCommand::SetBlend(false);

		// Downsample: HDR scene -> mip0 (thresholded) -> mip1 -> ...
		m_BloomDownsampleShader->Bind();
		m_BloomDownsampleShader->SetInt("u_Texture", 0);
		m_BloomDownsampleShader->SetFloat("u_Threshold", m_Settings.BloomThreshold);
		m_BloomDownsampleShader->SetFloat("u_Knee", glm::max(m_Settings.BloomKnee, 0.01f));

		for (size_t i = 0; i < m_BloomMips.size(); i++)
		{
			const Ref<Framebuffer>& source = (i == 0) ? m_SceneFramebuffer : m_BloomMips[i - 1];
			const auto& sourceSpec = source->GetSpecification();

			m_BloomMips[i]->Bind();
			source->BindColorTexture(0, 0);
			m_BloomDownsampleShader->SetInt("u_FirstPass", i == 0 ? 1 : 0);
			m_BloomDownsampleShader->SetFloat2("u_TexelSize",
				glm::vec2(1.0f / (float)sourceSpec.Width, 1.0f / (float)sourceSpec.Height));
			PostProcess::DrawFullscreenQuad();
			m_BloomMips[i]->Unbind();
		}

		// Upsample: additively blend each smaller mip onto the next-larger one
		RenderCommand::SetBlend(true);
		RenderCommand::SetBlendMode(RendererAPI::BlendMode::Additive);

		m_BloomUpsampleShader->Bind();
		m_BloomUpsampleShader->SetInt("u_Texture", 0);
		m_BloomUpsampleShader->SetFloat("u_FilterRadius", m_Settings.BloomFilterRadius);

		for (size_t i = m_BloomMips.size() - 1; i > 0; i--)
		{
			const Ref<Framebuffer>& source = m_BloomMips[i];
			const auto& sourceSpec = source->GetSpecification();

			m_BloomMips[i - 1]->Bind();
			source->BindColorTexture(0, 0);
			m_BloomUpsampleShader->SetFloat2("u_TexelSize",
				glm::vec2(1.0f / (float)sourceSpec.Width, 1.0f / (float)sourceSpec.Height));
			PostProcess::DrawFullscreenQuad();
			m_BloomMips[i - 1]->Unbind();
		}

		RenderCommand::SetBlendMode(RendererAPI::BlendMode::Alpha);
		return m_BloomMips[0];
	}

	void SceneRenderer::EndFrame()
	{
		m_SceneFramebuffer->Unbind();

		Ref<Framebuffer> bloom;
		if (m_Settings.BloomEnabled)
			bloom = RenderBloom();

		RenderCommand::SetBlend(false);

		// Tonemap HDR (+ bloom) into the FXAA input, or straight to the composite
		const Ref<Framebuffer>& tonemapTarget = m_Settings.FXAAEnabled ? m_TonemapFramebuffer : m_CompositeFramebuffer;
		tonemapTarget->Bind();
		RenderCommand::Clear();
		PostProcess::Tonemap(m_SceneFramebuffer, m_Settings.Exposure, bloom, m_Settings.BloomIntensity);
		tonemapTarget->Unbind();

		if (m_Settings.FXAAEnabled && m_FXAAShader)
		{
			m_CompositeFramebuffer->Bind();
			RenderCommand::Clear();

			m_TonemapFramebuffer->BindColorTexture(0, 0);
			m_FXAAShader->Bind();
			m_FXAAShader->SetInt("u_Texture", 0);
			m_FXAAShader->SetFloat2("u_TexelSize",
				glm::vec2(1.0f / (float)m_Width, 1.0f / (float)m_Height));
			PostProcess::DrawFullscreenQuad();
			m_CompositeFramebuffer->Unbind();
		}

		RenderCommand::SetBlend(true);
	}

	int SceneRenderer::ReadEntityID(int x, int y)
	{
		return m_SceneFramebuffer->ReadPixel(1, x, y);
	}

	uint32_t SceneRenderer::GetFinalImageRendererID() const
	{
		return m_CompositeFramebuffer->GetColorAttachmentRendererID();
	}

}
