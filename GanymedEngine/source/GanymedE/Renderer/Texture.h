#pragma once

#include <string>

#include "GanymedE/Core/Core.h"

#include <bgfx/bgfx.h>

namespace GanymedE {

	// Concrete wrapper over a bgfx 2D texture. The abstract Texture base is gone:
	// with one backend it had a single implementation, and nothing referenced it.
	//
	// Two things differ from the GL version:
	//   - Sampler state (filter/wrap) is no longer baked into the texture object.
	//     bgfx takes it as flags at bind time, so it is stored here and handed to
	//     bgfx::setTexture by Shader::SetTexture.
	//   - There is no Bind(slot). Binding a texture means naming the sampler
	//     uniform it feeds, which only the shader knows - see Shader::SetTexture.
	class Texture2D
	{
	public:
		Texture2D(uint32_t width, uint32_t height);
		Texture2D(const std::string& path);
		~Texture2D();

		Texture2D(const Texture2D&) = delete;
		Texture2D& operator=(const Texture2D&) = delete;

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		// ImGui still identifies textures by an opaque integer. Phase 6 swaps
		// this for a proper bgfx::TextureHandle typedef (§8.3).
		uint32_t GetRendererID() const { return m_Handle.idx; }
		bgfx::TextureHandle GetHandle() const { return m_Handle; }

		uint32_t GetSamplerFlags() const { return m_SamplerFlags; }
		void SetSamplerFlags(uint32_t flags) { m_SamplerFlags = flags; }

		void SetData(void* data, uint32_t size);

		bool IsValid() const { return bgfx::isValid(m_Handle); }

		bool operator==(const Texture2D& other) const { return m_Handle.idx == other.m_Handle.idx; }

		static Ref<Texture2D> Create(uint32_t width, uint32_t height);
		static Ref<Texture2D> Create(const std::string& path);
	private:
		std::string m_Path;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;

		// Mirrors the old GL state: repeat wrap (bgfx's default, so no bits) with
		// point magnification, which the 2D renderer relies on for crisp sprites.
		uint32_t m_SamplerFlags = BGFX_SAMPLER_MAG_POINT;

		bgfx::TextureHandle m_Handle = BGFX_INVALID_HANDLE;
	};
}
