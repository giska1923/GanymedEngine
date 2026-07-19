#include "gepch.h"
#include "GanymedE/Renderer/Framebuffer.h"

namespace GanymedE {

	namespace {

		bool IsDepthFormat(FramebufferTextureFormat format)
		{
			return format == FramebufferTextureFormat::DEPTH24STENCIL8
				|| format == FramebufferTextureFormat::DEPTH32F;
		}

		// The entity-ID attachment wants a 32-bit signed integer target, which is
		// not guaranteed to be renderable everywhere. bgfx can tell us up front,
		// so probe rather than discover it as a black screen on some backend.
		bgfx::TextureFormat::Enum ResolveFormat(FramebufferTextureFormat format)
		{
			switch (format)
			{
				case FramebufferTextureFormat::RGBA8:   return bgfx::TextureFormat::RGBA8;
				case FramebufferTextureFormat::RGBA16F: return bgfx::TextureFormat::RGBA16F;

				case FramebufferTextureFormat::RED_INTEGER:
				{
					if (bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::R32I, BGFX_TEXTURE_RT))
						return bgfx::TextureFormat::R32I;

					// Documented fallback (§6.2). Float32 holds entity IDs
					// exactly up to 2^24, same trade-off as the vertex attribute.
					GE_CORE_WARN("R32I is not a valid render target on '{0}'; "
						"falling back to R32F for the entity-ID attachment",
						bgfx::getRendererName(bgfx::getRendererType()));
					return bgfx::TextureFormat::R32F;
				}

				case FramebufferTextureFormat::DEPTH24STENCIL8: return bgfx::TextureFormat::D24S8;
				case FramebufferTextureFormat::DEPTH32F:        return bgfx::TextureFormat::D32F;
			}

			GE_CORE_ASSERT(false, "Unknown framebuffer texture format!");
			return bgfx::TextureFormat::Count;
		}

		uint64_t MsaaFlag(uint32_t samples)
		{
			switch (samples)
			{
				case 2:  return BGFX_TEXTURE_RT_MSAA_X2;
				case 4:  return BGFX_TEXTURE_RT_MSAA_X4;
				case 8:  return BGFX_TEXTURE_RT_MSAA_X8;
				case 16: return BGFX_TEXTURE_RT_MSAA_X16;
				default: return BGFX_TEXTURE_RT;
			}
		}

	}

	Framebuffer::Framebuffer(const FramebufferSpecification& spec)
		: m_Specification(spec)
	{
		Build();
	}

	Framebuffer::~Framebuffer()
	{
		Destroy();

		if (bgfx::isValid(m_ReadBack))
			bgfx::destroy(m_ReadBack);
	}

	void Framebuffer::Build()
	{
		if (m_Specification.Width == 0 || m_Specification.Height == 0)
		{
			GE_CORE_WARN("Framebuffer created with a zero dimension ({0}x{1})",
				m_Specification.Width, m_Specification.Height);
			return;
		}

		const uint64_t rtFlags = MsaaFlag(m_Specification.Samples)
			// Attachments are sampled by later passes (bloom, tonemap, the
			// viewport image), so clamp rather than repeat at the edges.
			| BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;

		std::vector<bgfx::TextureHandle> attachments;

		for (const auto& attachmentSpec : m_Specification.Attachments.Attachments)
		{
			const bgfx::TextureFormat::Enum format = ResolveFormat(attachmentSpec.TextureFormat);

			bgfx::TextureHandle texture = bgfx::createTexture2D(
				(uint16_t)m_Specification.Width,
				(uint16_t)m_Specification.Height,
				false, 1, format, rtFlags);

			if (!bgfx::isValid(texture))
			{
				GE_CORE_ERROR("Failed to create framebuffer attachment");
				continue;
			}

			if (IsDepthFormat(attachmentSpec.TextureFormat))
				m_DepthAttachment = texture;
			else
				m_ColorAttachments.push_back(texture);

			attachments.push_back(texture);
		}

		if (attachments.empty())
			return;

		// destroyTextures = false: the attachment handles are owned here so they
		// stay samplable, and Destroy() releases them explicitly.
		m_Handle = bgfx::createFrameBuffer((uint8_t)attachments.size(), attachments.data(), false);

		if (!bgfx::isValid(m_Handle))
			GE_CORE_ERROR("Failed to create framebuffer");
	}

	void Framebuffer::Destroy()
	{
		if (bgfx::isValid(m_Handle))
			bgfx::destroy(m_Handle);
		m_Handle = BGFX_INVALID_HANDLE;

		for (bgfx::TextureHandle texture : m_ColorAttachments)
		{
			if (bgfx::isValid(texture))
				bgfx::destroy(texture);
		}
		m_ColorAttachments.clear();

		if (bgfx::isValid(m_DepthAttachment))
			bgfx::destroy(m_DepthAttachment);
		m_DepthAttachment = BGFX_INVALID_HANDLE;
	}

	void Framebuffer::BindToView(uint16_t viewId) const
	{
		bgfx::setViewFrameBuffer(viewId, m_Handle);
		bgfx::setViewRect(viewId, 0, 0,
			(uint16_t)m_Specification.Width, (uint16_t)m_Specification.Height);
	}

	void Framebuffer::Resize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return;

		if (m_Specification.Width == width && m_Specification.Height == height)
			return;

		m_Specification.Width = width;
		m_Specification.Height = height;

		// bgfx render targets are immutable in size, same as the old GL path.
		Destroy();
		Build();
	}

	uint32_t Framebuffer::RequestPixelRead(uint16_t viewId, uint32_t attachmentIndex, int x, int y, void* dest)
	{
		bgfx::TextureHandle source = GetColorAttachment(attachmentIndex);
		if (!bgfx::isValid(source) || !dest)
			return 0;

		const bgfx::TextureFormat::Enum format =
			ResolveFormat(m_Specification.Attachments.Attachments[attachmentIndex].TextureFormat);

		// Recreate the staging texture if the format changed under us.
		if (!bgfx::isValid(m_ReadBack) || m_ReadBackFormat != format)
		{
			if (bgfx::isValid(m_ReadBack))
				bgfx::destroy(m_ReadBack);

			m_ReadBack = bgfx::createTexture2D(1, 1, false, 1, format,
				BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
			m_ReadBackFormat = format;
		}

		if (!bgfx::isValid(m_ReadBack))
			return 0;

		bgfx::blit(viewId, m_ReadBack, 0, 0, source, (uint16_t)x, (uint16_t)y, 1, 1);

		// The returned frame number is when dest actually holds the pixel -
		// typically two frames out. The caller must not read it before then.
		return bgfx::readTexture(m_ReadBack, dest);
	}

	bool Framebuffer::IsAttachmentIntegerFormat(uint32_t index) const
	{
		if (index >= m_Specification.Attachments.Attachments.size())
			return false;

		return ResolveFormat(m_Specification.Attachments.Attachments[index].TextureFormat)
			== bgfx::TextureFormat::R32I;
	}

	bgfx::TextureHandle Framebuffer::GetColorAttachment(uint32_t index) const
	{
		if (index >= m_ColorAttachments.size())
			return BGFX_INVALID_HANDLE;

		return m_ColorAttachments[index];
	}

	uint32_t Framebuffer::GetColorAttachmentRendererID(uint32_t index) const
	{
		return GetColorAttachment(index).idx;
	}

	Ref<Framebuffer> Framebuffer::Create(const FramebufferSpecification& spec)
	{
		return CreateRef<Framebuffer>(spec);
	}
}
