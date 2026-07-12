#include "gepch.h"
#include "Platform/OpenGL/OpenGLFramebuffer.h"

#include <glad/glad.h>

namespace GanymedE {

	static const uint32_t s_MaxFramebufferSize = 8192;

	namespace Utils {

		static void AttachColorTexture(uint32_t id, GLenum internalFormat, GLenum format, GLenum type, uint32_t width, uint32_t height, int index)
		{
			glBindTexture(GL_TEXTURE_2D, id);
			glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, nullptr);

			// Integer textures do not support linear filtering
			GLint filter = (format == GL_RED_INTEGER) ? GL_NEAREST : GL_LINEAR;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_TEXTURE_2D, id, 0);
		}

		static void AttachDepthTexture(uint32_t id, GLenum internalFormat, GLenum format, GLenum type, GLenum attachmentType, uint32_t width, uint32_t height)
		{
			glBindTexture(GL_TEXTURE_2D, id);
			glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			// Clamp so shadow lookups outside the map are unlit (handled in-shader too)
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			glFramebufferTexture2D(GL_FRAMEBUFFER, attachmentType, GL_TEXTURE_2D, id, 0);
		}

		static bool IsDepthFormat(FramebufferTextureFormat format)
		{
			switch (format)
			{
				case FramebufferTextureFormat::DEPTH24STENCIL8: return true;
				case FramebufferTextureFormat::DEPTH32F: return true;
				default: return false;
			}
		}

	}

	OpenGLFramebuffer::OpenGLFramebuffer(const FramebufferSpecification& spec)
		: m_Specification(spec)
	{
		for (auto attachmentSpec : m_Specification.Attachments.Attachments)
		{
			if (!Utils::IsDepthFormat(attachmentSpec.TextureFormat))
				m_ColorAttachmentSpecifications.emplace_back(attachmentSpec);
			else
				m_DepthAttachmentSpecification = attachmentSpec;
		}

		Invalidate();
	}

	OpenGLFramebuffer::~OpenGLFramebuffer()
	{
		glDeleteFramebuffers(1, &m_RendererID);
		glDeleteTextures((GLsizei)m_ColorAttachments.size(), m_ColorAttachments.data());
		glDeleteTextures(1, &m_DepthAttachment);
	}

	void OpenGLFramebuffer::Invalidate()
	{
		if (m_RendererID)
		{
			glDeleteFramebuffers(1, &m_RendererID);
			glDeleteTextures((GLsizei)m_ColorAttachments.size(), m_ColorAttachments.data());
			glDeleteTextures(1, &m_DepthAttachment);

			m_ColorAttachments.clear();
			m_DepthAttachment = 0;
		}

		glGenFramebuffers(1, &m_RendererID);
		glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);

		// Attachments
		if (m_ColorAttachmentSpecifications.size())
		{
			m_ColorAttachments.resize(m_ColorAttachmentSpecifications.size());
			glGenTextures((GLsizei)m_ColorAttachments.size(), m_ColorAttachments.data());

			for (size_t i = 0; i < m_ColorAttachments.size(); i++)
			{
				switch (m_ColorAttachmentSpecifications[i].TextureFormat)
				{
					case FramebufferTextureFormat::RGBA8:
						Utils::AttachColorTexture(m_ColorAttachments[i], GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, m_Specification.Width, m_Specification.Height, (int)i);
						break;
					case FramebufferTextureFormat::RGBA16F:
						Utils::AttachColorTexture(m_ColorAttachments[i], GL_RGBA16F, GL_RGBA, GL_FLOAT, m_Specification.Width, m_Specification.Height, (int)i);
						break;
					case FramebufferTextureFormat::RED_INTEGER:
						Utils::AttachColorTexture(m_ColorAttachments[i], GL_R32I, GL_RED_INTEGER, GL_INT, m_Specification.Width, m_Specification.Height, (int)i);
						break;
				}
			}
		}

		if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
		{
			glGenTextures(1, &m_DepthAttachment);

			switch (m_DepthAttachmentSpecification.TextureFormat)
			{
				case FramebufferTextureFormat::DEPTH24STENCIL8:
					Utils::AttachDepthTexture(m_DepthAttachment, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, GL_DEPTH_STENCIL_ATTACHMENT, m_Specification.Width, m_Specification.Height);
					break;
				case FramebufferTextureFormat::DEPTH32F:
					Utils::AttachDepthTexture(m_DepthAttachment, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, GL_DEPTH_ATTACHMENT, m_Specification.Width, m_Specification.Height);
					break;
			}
		}

		if (m_ColorAttachments.size() > 1)
		{
			GE_CORE_ASSERT(m_ColorAttachments.size() <= 4, "Only up to 4 color attachments are supported!");
			GLenum buffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
			glDrawBuffers((GLsizei)m_ColorAttachments.size(), buffers);
		}
		else if (m_ColorAttachments.empty())
		{
			// Depth-only pass (e.g. shadow map)
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
		}

		GE_CORE_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Framebuffer is incomplete!");

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	void OpenGLFramebuffer::Bind()
	{
		// Remember whatever was bound so nested passes can restore it on Unbind()
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_PreviousBinding);
		glGetIntegerv(GL_VIEWPORT, m_PreviousViewport);

		glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);
		glViewport(0, 0, m_Specification.Width, m_Specification.Height);
	}

	void OpenGLFramebuffer::Unbind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)m_PreviousBinding);
		glViewport(m_PreviousViewport[0], m_PreviousViewport[1], m_PreviousViewport[2], m_PreviousViewport[3]);
	}

	void OpenGLFramebuffer::BindColorTexture(uint32_t attachmentIndex, uint32_t slot) const
	{
		GE_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Color attachment index out of range!");
		glActiveTexture(GL_TEXTURE0 + slot);
		glBindTexture(GL_TEXTURE_2D, m_ColorAttachments[attachmentIndex]);
	}

	void OpenGLFramebuffer::BindDepthTexture(uint32_t slot) const
	{
		glActiveTexture(GL_TEXTURE0 + slot);
		glBindTexture(GL_TEXTURE_2D, m_DepthAttachment);
	}

	void OpenGLFramebuffer::Resize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0 || width > s_MaxFramebufferSize || height > s_MaxFramebufferSize)
		{
			GE_CORE_WARN("Attempted to rezize framebuffer to {0}, {1}", width, height);
			return;
		}

		m_Specification.Width = width;
		m_Specification.Height = height;

		Invalidate();
	}

	int OpenGLFramebuffer::ReadPixel(uint32_t attachmentIndex, int x, int y)
	{
		GE_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Color attachment index out of range!");

		GLint previousFramebuffer = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousFramebuffer);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_RendererID);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
		int pixelData = -1;
		glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixelData);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, previousFramebuffer);
		return pixelData;
	}

	void OpenGLFramebuffer::ClearAttachment(uint32_t attachmentIndex, int value)
	{
		GE_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Color attachment index out of range!");

		// glClearTexImage is GL 4.4+, so clear through the framebuffer to stay on the GL 4.1 baseline
		GLint previousFramebuffer = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_RendererID);
		glClearBufferiv(GL_COLOR, attachmentIndex, &value);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousFramebuffer);
	}
}
