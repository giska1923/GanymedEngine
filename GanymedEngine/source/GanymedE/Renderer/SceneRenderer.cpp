#include "gepch.h"
#include "SceneRenderer.h"

#include "GanymedE/Renderer/PostProcess.h"
#include "GanymedE/Renderer/RenderCommand.h"
#include "GanymedE/Renderer/RenderPassIDs.h"
#include "GanymedE/Renderer/Renderer.h"

#include <algorithm>

namespace GanymedE {

	static constexpr uint32_t kMaxBloomMips = 6;
	static constexpr uint32_t kMinBloomMipSize = 8;

	// Clear-colour palette slots. bgfx keeps a global palette; these two indices
	// belong to the scene pass and must not collide with any other pass's.
	static constexpr uint8_t kSceneColourPalette = 0;
	static constexpr uint8_t kEntityIdPalette = 1;

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

		// Sequential, NOT bgfx's default sort.
		//
		// By default bgfx reorders draws within a view to minimise state changes,
		// which silently breaks anything that depends on submission order - and
		// this renderer depends on it heavily: the skybox draws with depth-test
		// off (it would paint over opaque geometry if it moved after it), and
		// transparents are sorted back-to-front on the CPU. Sequential keeps the
		// order the engine already establishes.
		bgfx::setViewMode(RenderPass::SceneHDR, bgfx::ViewMode::Sequential);

		// The clear is view state rather than an immediate command.
		//
		// The plain setViewClear(rgba) form applies one packed colour to every
		// attachment, which cannot express "clear entity IDs to -1" - packing -1
		// into an 8-bit-per-channel word is meaningless. The palette form gives
		// each attachment its own float4 entry, which bgfx converts per format,
		// so the R32I target genuinely receives -1.
		const glm::vec4& c = m_Settings.ClearColor;
		const float sceneColour[4] = { c.r, c.g, c.b, c.a };
		const float noEntity[4] = { -1.0f, -1.0f, -1.0f, -1.0f };

		bgfx::setPaletteColor(kSceneColourPalette, sceneColour);
		bgfx::setPaletteColor(kEntityIdPalette, noEntity);

		bgfx::setViewClear(RenderPass::SceneHDR,
			BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
			1.0f, 0,
			kSceneColourPalette,  // attachment 0 - HDR colour
			kEntityIdPalette);    // attachment 1 - entity IDs

		// bgfx skips a view that receives no draw calls, and its clear with it.
		// Without this the scene target keeps last frame's contents whenever
		// nothing is submitted - which is every frame while the scene shaders
		// are still unported.
		bgfx::touch(RenderPass::SceneHDR);

		// Renderer2D draws into the same target but needs its OWN view.
		//
		// A view transform is per-view-per-frame, not per-draw: the last
		// setViewTransform before bgfx::frame() applies to every draw in that
		// view. With 2D sharing SceneHDR, Renderer2D::BeginScene (which runs
		// after Renderer3D::EndScene) retroactively re-projected all the 3D
		// geometry with the 2D camera. Separate views keep separate transforms.
		m_SceneFramebuffer->BindToView(RenderPass::SceneTransparent);
		bgfx::setViewMode(RenderPass::SceneTransparent, bgfx::ViewMode::Sequential);
		// No clear: this view composites on top of the 3D pass.
		bgfx::setViewClear(RenderPass::SceneTransparent, BGFX_CLEAR_NONE, 0, 1.0f, 0);
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
		bgfx::touch(RenderPass::Tonemap); // force the clear even with nothing submitted
		PostProcess::Tonemap(m_SceneFramebuffer, m_Settings.Exposure, bloom, m_Settings.BloomIntensity);

		if (m_Settings.FXAAEnabled && m_FXAAShader)
		{
			m_CompositeFramebuffer->BindToView(RenderPass::FXAA);
			RenderCommand::SetViewId(RenderPass::FXAA);
			bgfx::setViewClear(RenderPass::FXAA, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
			bgfx::touch(RenderPass::FXAA);

			m_FXAAShader->Bind();
			m_FXAAShader->SetTexture("u_Texture", 0, m_TonemapFramebuffer->GetColorAttachment(0), BGFX_SAMPLER_UVW_CLAMP);
			m_FXAAShader->SetFloat2("u_TexelSize",
				glm::vec2(1.0f / (float)m_Width, 1.0f / (float)m_Height));
			PostProcess::DrawFullscreenQuad();
		}

		RenderCommand::SetBlend(true);
	}

	void SceneRenderer::RequestEntityID(int x, int y)
	{
		if (!m_SceneFramebuffer || !m_SceneFramebuffer->IsValid())
			return;

		if (x < 0 || y < 0 || x >= (int)m_Width || y >= (int)m_Height)
			return;

		PendingPick* slot = nullptr;
		for (PendingPick& pick : m_Picks)
		{
			if (!pick.InFlight)
			{
				slot = &pick;
				break;
			}
		}

		// Every slot busy means the GPU is more than kMaxPicksInFlight frames
		// behind. Dropping this request is correct - the next frame issues
		// another, and a queue that grows would only add latency.
		if (!slot)
			return;

		const uint32_t readyFrame = m_SceneFramebuffer->RequestPixelRead(
			RenderPass::Picking, 1, x, y, &slot->Value);

		if (readyFrame == 0)
			return;

		slot->InFlight = true;
		slot->ReadyFrame = readyFrame;
	}

	bool SceneRenderer::PollEntityID(int& outEntityID)
	{
		const uint32_t current = Renderer::GetFrameNumber();
		const bool isInteger = m_SceneFramebuffer && m_SceneFramebuffer->IsAttachmentIntegerFormat(1);

		bool found = false;
		uint32_t newest = 0;

		for (PendingPick& pick : m_Picks)
		{
			if (!pick.InFlight || current < pick.ReadyFrame)
				continue;

			// Keep only the most recent landed result; retiring the older ones
			// stops a stale pixel from winning the race.
			if (!found || pick.ReadyFrame >= newest)
			{
				newest = pick.ReadyFrame;
				outEntityID = isInteger ? pick.Value.AsInt : (int)pick.Value.AsFloat;
				found = true;
			}

			pick.InFlight = false;
		}

		return found;
	}

	uint32_t SceneRenderer::GetFinalImageRendererID() const
	{
		return m_CompositeFramebuffer->GetColorAttachmentRendererID();
	}

}
