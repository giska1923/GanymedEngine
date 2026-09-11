#pragma once

#include "GanymedE/Core/Core.h"

#include <bgfx/bgfx.h>

#include <vector>

namespace GanymedE {
	enum class FramebufferTextureFormat
	{
		None = 0,

		// Color
		RGBA8,
		RGBA16F,
		RED_INTEGER,

		// Depth/stencil
		DEPTH24STENCIL8,
		DEPTH32F,

		// Defaults
		Depth = DEPTH24STENCIL8
	};

	struct FramebufferTextureSpecification
	{
		FramebufferTextureSpecification() = default;
		FramebufferTextureSpecification(FramebufferTextureFormat format)
			: TextureFormat(format) {}

		FramebufferTextureFormat TextureFormat = FramebufferTextureFormat::None;
	};

	struct FramebufferAttachmentSpecification
	{
		FramebufferAttachmentSpecification() = default;
		FramebufferAttachmentSpecification(std::initializer_list<FramebufferTextureSpecification> attachments)
			: Attachments(attachments) {}

		std::vector<FramebufferTextureSpecification> Attachments;
	};

	struct FramebufferSpecification
	{
		uint32_t Width = 0, Height = 0;

		FramebufferAttachmentSpecification Attachments;

		// **Nothing sets this, and raising it aborts as the code stands.** MSAA is deferred, not
		// broken: the cause is known and the fix is one line, but enabling it is a decision
		// nobody has taken. See docs/ToDo/rendering.md before touching it.
		//
		// Raising it makes bgfx assert while building the framebuffer, through BX_ASSERT rather
		// than the bgfx callback - so the process dies with nothing in GanymedE.log:
		//
		//     Frame buffer depth MSAA texture cannot be resolved. It must be created with
		//     either `BGFX_TEXTURE_RT_WRITE_ONLY` or `BGFX_TEXTURE_MSAA_SAMPLE` flag.
		//
		// It is the DEPTH attachment. Giving it BGFX_TEXTURE_RT_WRITE_ONLY when Samples > 1 was
		// verified to fix construction on D3D11, with picking still correct - RT_WRITE_ONLY
		// because nothing samples the scene depth, and the guard because the shadow cascades do
		// sample theirs and are single-sample.
		uint32_t Samples = 1;

		bool SwapChainTarget = false;
	};

	// Concrete wrapper over bgfx::FrameBufferHandle.
	//
	// The big shape change from GL: there is no bind/unbind. A framebuffer is
	// attached to a *view*, and every draw submitted to that view lands in it.
	// SceneRenderer owns which view is which - see RenderPassIDs.h.
	class Framebuffer
	{
	public:
		Framebuffer(const FramebufferSpecification& spec);
		~Framebuffer();

		Framebuffer(const Framebuffer&) = delete;
		Framebuffer& operator=(const Framebuffer&) = delete;

		// Points a view at this framebuffer and sizes its viewport to match.
		// Replaces Bind(); there is no Unbind - targeting ends when the view does.
		void BindToView(uint16_t viewId) const;

		void Resize(uint32_t width, uint32_t height);

		// Asynchronous under bgfx: queues a blit + read and returns the frame
		// number at which the result becomes valid. Phase 5 builds the
		// pending-pick queue on top - see docs/history/BGFX_MIGRATION.md §7.
		uint32_t RequestPixelRead(uint16_t viewId, uint32_t attachmentIndex, int x, int y, void* dest);

		// True when the attachment resolved to an integer format. The entity-ID
		// target falls back to R32F where R32I is not renderable, and readback
		// has to reinterpret the bytes accordingly.
		bool IsAttachmentIntegerFormat(uint32_t index) const;

		bgfx::TextureHandle GetColorAttachment(uint32_t index = 0) const;
		bgfx::TextureHandle GetDepthAttachment() const { return m_DepthAttachment; }

		// Kept so ImGui image call sites compile; see Texture2D::GetRendererID.
		uint32_t GetColorAttachmentRendererID(uint32_t index = 0) const;

		bool IsValid() const { return bgfx::isValid(m_Handle); }
		bgfx::FrameBufferHandle GetHandle() const { return m_Handle; }

		const FramebufferSpecification& GetSpecification() const { return m_Specification; }

		static Ref<Framebuffer> Create(const FramebufferSpecification& spec);
	private:
		void Build();
		void Destroy();
	private:
		FramebufferSpecification m_Specification;

		bgfx::FrameBufferHandle m_Handle = BGFX_INVALID_HANDLE;

		// Tracked separately from the framebuffer so they can be sampled by
		// later passes and blitted from for readback.
		std::vector<bgfx::TextureHandle> m_ColorAttachments;
		bgfx::TextureHandle m_DepthAttachment = BGFX_INVALID_HANDLE;

		// 1x1 staging texture for RequestPixelRead, created on first use.
		bgfx::TextureHandle m_ReadBack = BGFX_INVALID_HANDLE;
		bgfx::TextureFormat::Enum m_ReadBackFormat = bgfx::TextureFormat::Count;
	};
}
