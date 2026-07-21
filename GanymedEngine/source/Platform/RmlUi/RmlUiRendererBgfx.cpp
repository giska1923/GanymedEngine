#include "gepch.h"
#include "Platform/RmlUi/RmlUiRendererBgfx.h"

#include "GanymedE/Renderer/Framebuffer.h"
#include "GanymedE/Renderer/RenderPassIDs.h"
#include "GanymedE/Renderer/Renderer.h"
#include "GanymedE/Renderer/Shader.h"

#include <stb_image.h>

#include <algorithm>
#include <cstring>

namespace GanymedE {

	namespace {

		// One compiled mesh. RmlUi keeps the handle for as long as the element
		// exists, so these are static buffers rather than per-frame transients.
		struct RmlGeometry
		{
			bgfx::VertexBufferHandle Vertices = BGFX_INVALID_HANDLE;
			bgfx::IndexBufferHandle Indices = BGFX_INVALID_HANDLE;
			uint32_t IndexCount = 0;
		};

		// RmlUi texture handles are opaque uintptr_t where 0 means "no texture",
		// but a bgfx handle of idx 0 is perfectly valid - so store idx + 1.
		Rml::TextureHandle ToRmlTexture(bgfx::TextureHandle handle)
		{
			return static_cast<Rml::TextureHandle>(handle.idx) + 1;
		}

		bgfx::TextureHandle FromRmlTexture(Rml::TextureHandle handle)
		{
			if (handle == 0)
				return BGFX_INVALID_HANDLE;
			return bgfx::TextureHandle{ static_cast<uint16_t>(handle - 1) };
		}
	}

	bool RmlUiRendererBgfx::Init()
	{
		m_Shader = Shader::Create("RmlUi");
		if (!m_Shader || !m_Shader->IsValid())
		{
			GE_CORE_ERROR("RmlUi bgfx backend: failed to load the RmlUi shader "
				"- run scripts/compile_shaders.bat");
			return false;
		}

		// Must match Rml::Vertex field-for-field, in declaration order:
		// Vector2f position, ColourbPremultiplied colour (RGBA8), Vector2f tex_coord.
		m_Layout.begin()
			.add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.end();

		GE_CORE_ASSERT(m_Layout.getStride() == sizeof(Rml::Vertex),
			"RmlUi vertex layout does not match Rml::Vertex - the memcpy in CompileGeometry "
			"would reinterpret the data");

		m_TextureUniform = bgfx::createUniform("s_Texture", bgfx::UniformType::Sampler);

		const uint32_t white = 0xffffffff;
		m_WhiteTexture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0,
			bgfx::copy(&white, sizeof(white)));

		return true;
	}

	void RmlUiRendererBgfx::Shutdown()
	{
		// Guarded like every other engine resource destructor: after bgfx::shutdown
		// these handles point at a dead context.
		if (Renderer::IsGpuAlive())
		{
			if (bgfx::isValid(m_TextureUniform))
				bgfx::destroy(m_TextureUniform);
			if (bgfx::isValid(m_WhiteTexture))
				bgfx::destroy(m_WhiteTexture);
		}

		m_TextureUniform = BGFX_INVALID_HANDLE;
		m_WhiteTexture = BGFX_INVALID_HANDLE;
		m_Shader = nullptr;
	}

	void RmlUiRendererBgfx::BeginFrame(const Ref<Framebuffer>& target, uint32_t width, uint32_t height)
	{
		m_Width = width;
		m_Height = height;

		const uint16_t view = RenderPass::UI;

		bgfx::setViewName(view, "RmlUi");
		// Sequential: UI paints back-to-front and bgfx must not reorder within the view.
		bgfx::setViewMode(view, bgfx::ViewMode::Sequential);

		if (target)
			target->BindToView(view);
		else
			bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);

		bgfx::setViewRect(view, 0, 0, (uint16_t)width, (uint16_t)height);

		// The UI composites onto the already-rendered image, so no clear.
		bgfx::setViewClear(view, BGFX_CLEAR_NONE);

		// Ortho over the viewport in pixels, top-left origin. Built by hand from
		// caps rather than with glm because the workspace-wide
		// GLM_FORCE_DEPTH_ZERO_TO_ONE is compile-time and cannot adapt per backend.
		{
			const bool homogeneousDepth = bgfx::getCaps()->homogeneousDepth;
			const float L = 0.0f, R = (float)width, T = 0.0f, B = (float)height;
			const float zn = homogeneousDepth ? -1.0f : 0.0f;
			const float zf = 1.0f;

			float ortho[16];
			memset(ortho, 0, sizeof(ortho));
			ortho[0]  = 2.0f / (R - L);
			ortho[5]  = 2.0f / (T - B);
			ortho[10] = 1.0f / (zf - zn);
			ortho[12] = (R + L) / (L - R);
			ortho[13] = (T + B) / (B - T);
			ortho[14] = zn == 0.0f ? 0.0f : zn / (zn - zf);
			ortho[15] = 1.0f;

			bgfx::setViewTransform(view, nullptr, ortho);
		}

		// bgfx skips a view entirely if nothing is submitted to it, which would
		// leave a stale framebuffer binding; touch keeps the view live.
		bgfx::touch(view);
	}

	void RmlUiRendererBgfx::EndFrame()
	{
		m_ScissorEnabled = false;
	}

	Rml::CompiledGeometryHandle RmlUiRendererBgfx::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
		Rml::Span<const int> indices)
	{
		if (vertices.empty() || indices.empty())
			return 0;

		RmlGeometry* geometry = new RmlGeometry();

		geometry->Vertices = bgfx::createVertexBuffer(
			bgfx::copy(vertices.data(), (uint32_t)(vertices.size() * sizeof(Rml::Vertex))),
			m_Layout);

		// RmlUi indices are int (32-bit), so the buffer must say so - the default
		// is 16-bit and would silently halve every index.
		geometry->Indices = bgfx::createIndexBuffer(
			bgfx::copy(indices.data(), (uint32_t)(indices.size() * sizeof(int))),
			BGFX_BUFFER_INDEX32);

		geometry->IndexCount = (uint32_t)indices.size();

		return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
	}

	void RmlUiRendererBgfx::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
		Rml::TextureHandle texture)
	{
		RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
		if (!geometry || !m_Shader || !m_Shader->IsValid())
			return;

		// The per-draw translation rides in as a model matrix, so the vertex shader
		// needs no RmlUi-specific uniform.
		float transform[16];
		memset(transform, 0, sizeof(transform));
		transform[0] = transform[5] = transform[10] = transform[15] = 1.0f;
		transform[12] = translation.x;
		transform[13] = translation.y;
		bgfx::setTransform(transform);

		if (m_ScissorEnabled)
		{
			// Clamp into the viewport: bgfx takes unsigned scissor rects and a
			// negative origin would wrap to an enormous one.
			const int x0 = std::max(m_Scissor.Left(), 0);
			const int y0 = std::max(m_Scissor.Top(), 0);
			const int x1 = std::min(m_Scissor.Right(), (int)m_Width);
			const int y1 = std::min(m_Scissor.Bottom(), (int)m_Height);

			if (x1 <= x0 || y1 <= y0)
				return;   // fully clipped away

			bgfx::setScissor((uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0));
		}

		bgfx::TextureHandle bound = FromRmlTexture(texture);
		bgfx::setTexture(0, m_TextureUniform, bgfx::isValid(bound) ? bound : m_WhiteTexture);

		// Premultiplied-alpha blending; depth test off (UI is painter-ordered).
		bgfx::setState(BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA));

		bgfx::setVertexBuffer(0, geometry->Vertices);
		bgfx::setIndexBuffer(geometry->Indices, 0, geometry->IndexCount);

		bgfx::submit(RenderPass::UI, m_Shader->GetProgram());
	}

	void RmlUiRendererBgfx::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
	{
		RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
		if (!geometry)
			return;

		if (Renderer::IsGpuAlive())
		{
			if (bgfx::isValid(geometry->Vertices))
				bgfx::destroy(geometry->Vertices);
			if (bgfx::isValid(geometry->Indices))
				bgfx::destroy(geometry->Indices);
		}

		delete geometry;
	}

	Rml::TextureHandle RmlUiRendererBgfx::LoadTexture(Rml::Vector2i& textureDimensions,
		const Rml::String& source)
	{
		// Decoded here with stb_image rather than left to RmlUi: the reference
		// backends only handle TGA, and stb_image is already vendored.
		int width = 0, height = 0, channels = 0;
		stbi_uc* pixels = stbi_load(source.c_str(), &width, &height, &channels, 4);
		if (!pixels)
		{
			GE_CORE_ERROR("RmlUi: failed to load texture '{0}': {1}", source, stbi_failure_reason());
			return 0;
		}

		// RmlUi blends premultiplied, so convert here. GenerateTexture needs no
		// equivalent - that data arrives premultiplied already.
		for (int i = 0; i < width * height; i++)
		{
			stbi_uc* texel = pixels + i * 4;
			const uint32_t alpha = texel[3];
			texel[0] = (stbi_uc)((texel[0] * alpha) / 255);
			texel[1] = (stbi_uc)((texel[1] * alpha) / 255);
			texel[2] = (stbi_uc)((texel[2] * alpha) / 255);
		}

		bgfx::TextureHandle handle = bgfx::createTexture2D(
			(uint16_t)width, (uint16_t)height, false, 1, bgfx::TextureFormat::RGBA8, 0,
			bgfx::copy(pixels, width * height * 4));

		stbi_image_free(pixels);

		if (!bgfx::isValid(handle))
			return 0;

		textureDimensions = Rml::Vector2i(width, height);
		return ToRmlTexture(handle);
	}

	Rml::TextureHandle RmlUiRendererBgfx::GenerateTexture(Rml::Span<const Rml::byte> source,
		Rml::Vector2i sourceDimensions)
	{
		// Font glyph atlases arrive here, already premultiplied by RmlUi.
		if (source.empty())
			return 0;

		bgfx::TextureHandle handle = bgfx::createTexture2D(
			(uint16_t)sourceDimensions.x, (uint16_t)sourceDimensions.y, false, 1,
			bgfx::TextureFormat::RGBA8, 0,
			bgfx::copy(source.data(), (uint32_t)source.size()));

		return bgfx::isValid(handle) ? ToRmlTexture(handle) : 0;
	}

	void RmlUiRendererBgfx::ReleaseTexture(Rml::TextureHandle texture)
	{
		bgfx::TextureHandle handle = FromRmlTexture(texture);
		if (bgfx::isValid(handle) && Renderer::IsGpuAlive())
			bgfx::destroy(handle);
	}

	void RmlUiRendererBgfx::EnableScissorRegion(bool enable)
	{
		m_ScissorEnabled = enable;
	}

	void RmlUiRendererBgfx::SetScissorRegion(Rml::Rectanglei region)
	{
		m_Scissor = region;
	}
}
