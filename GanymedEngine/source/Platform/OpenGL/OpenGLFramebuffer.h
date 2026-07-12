#pragma once

#include "GanymedE/Renderer/Framebuffer.h"

namespace GanymedE {
	class OpenGLFramebuffer : public Framebuffer
	{
	public:
		OpenGLFramebuffer(const FramebufferSpecification& spec);
		virtual ~OpenGLFramebuffer();

		void Invalidate();

		virtual void Bind() override;
		virtual void Unbind() override;

		virtual void Resize(uint32_t width, uint32_t height) override;
		virtual int ReadPixel(uint32_t attachmentIndex, int x, int y) override;

		virtual void ClearAttachment(uint32_t attachmentIndex, int value) override;

		virtual uint32_t GetColorAttachmentRendererID(uint32_t index = 0) const override
		{
			GE_CORE_ASSERT(index < m_ColorAttachments.size(), "Color attachment index out of range!");
			return m_ColorAttachments[index];
		}

		virtual uint32_t GetDepthAttachmentRendererID() const override { return m_DepthAttachment; }

		virtual void BindColorTexture(uint32_t attachmentIndex, uint32_t slot) const override;
		virtual void BindDepthTexture(uint32_t slot) const override;

		virtual const FramebufferSpecification& GetSpecification() const override { return m_Specification; }
	private:
		uint32_t m_RendererID = 0;
		FramebufferSpecification m_Specification;

		// Saved on Bind() and restored on Unbind() so nested passes (e.g. shadow maps)
		// return control to the previously bound framebuffer/viewport.
		int m_PreviousBinding = 0;
		int m_PreviousViewport[4] = { 0, 0, 0, 0 };

		std::vector<FramebufferTextureSpecification> m_ColorAttachmentSpecifications;
		FramebufferTextureSpecification m_DepthAttachmentSpecification = FramebufferTextureFormat::None;

		std::vector<uint32_t> m_ColorAttachments;
		uint32_t m_DepthAttachment = 0;
	};
}
