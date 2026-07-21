#pragma once

#include "GanymedE/Core/Core.h"

#include <RmlUi/Core/RenderInterface.h>

#include <bgfx/bgfx.h>

namespace GanymedE {

	class Framebuffer;

	// RmlUi's RenderInterface on top of bgfx, modeled on ImGuiRendererBgfx.
	//
	// Two structural differences from the ImGui backend, both coming from RmlUi:
	//
	//  - Geometry is COMPILED, not transient. RmlUi hands geometry over once and
	//    re-renders it for as many frames as the element lives, which is the whole
	//    point of the compiled-geometry model in RmlUi 6.x. So these are static
	//    vertex/index buffers with an explicit release, not per-frame transients.
	//  - Colours and textures are PREMULTIPLIED alpha, so the blend function is
	//    (ONE, INV_SRC_ALPHA) rather than the usual (SRC_ALPHA, INV_SRC_ALPHA).
	//
	// Everything submits to RenderPass::UI with an explicit view id. It never
	// touches RenderCommand::SetViewId, whose sticky current-view state would then
	// need restoring - the class of bug that once sent the whole scene into a
	// shadow cascade.
	class RmlUiRendererBgfx : public Rml::RenderInterface
	{
	public:
		bool Init();
		void Shutdown();

		// target may be null, meaning the backbuffer (a shipped game); the editor
		// passes the SceneRenderer's composite target.
		void BeginFrame(const Ref<Framebuffer>& target, uint32_t width, uint32_t height);
		void EndFrame();

		// ---- Rml::RenderInterface ----
		Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
			Rml::Span<const int> indices) override;
		void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
			Rml::TextureHandle texture) override;
		void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

		Rml::TextureHandle LoadTexture(Rml::Vector2i& textureDimensions, const Rml::String& source) override;
		Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
			Rml::Vector2i sourceDimensions) override;
		void ReleaseTexture(Rml::TextureHandle texture) override;

		void EnableScissorRegion(bool enable) override;
		void SetScissorRegion(Rml::Rectanglei region) override;

	private:
		Ref<class Shader> m_Shader;
		bgfx::VertexLayout m_Layout;
		bgfx::UniformHandle m_TextureUniform = BGFX_INVALID_HANDLE;

		// Bound when RmlUi passes texture handle 0 ("untextured"): sampling a 1x1
		// white texel is cheaper than maintaining a second shader variant.
		bgfx::TextureHandle m_WhiteTexture = BGFX_INVALID_HANDLE;

		uint32_t m_Width = 0;
		uint32_t m_Height = 0;

		bool m_ScissorEnabled = false;
		Rml::Rectanglei m_Scissor;
	};
}
