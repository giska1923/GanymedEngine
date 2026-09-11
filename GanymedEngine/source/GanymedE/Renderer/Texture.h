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

		// A compiled texture container - DDS or KTX bytes, as the asset compiler writes them.
		// bgfx parses the container itself and reports back what it found, so a block-compressed,
		// fully mipped texture takes exactly the same amount of engine code as an RGBA8 one.
		Texture2D(const uint8_t* containerBytes, uint32_t size);

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

		// Mips and the on-GPU format, for the stats readout - an RGBA8 texture with no mips and
		// a BC7 one with a full chain are the before and after of the asset compiler.
		uint8_t GetMipCount() const { return m_MipCount; }
		uint32_t GetGpuFormat() const { return m_Format; }
		uint32_t GetGpuBytes() const { return m_GpuBytes; }

		static Ref<Texture2D> Create(uint32_t width, uint32_t height);
		static Ref<Texture2D> Create(const std::string& path);
		static Ref<Texture2D> CreateFromContainer(const uint8_t* containerBytes, uint32_t size);
	private:
		std::string m_Path;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;

		// Mirrors the old GL state: repeat wrap (bgfx's default, so no bits) with
		// point magnification, which the 2D renderer relies on for crisp sprites.
		uint32_t m_SamplerFlags = BGFX_SAMPLER_MAG_POINT;

		uint8_t m_MipCount = 1;
		uint32_t m_Format = (uint32_t)bgfx::TextureFormat::RGBA8;
		uint32_t m_GpuBytes = 0;

		bgfx::TextureHandle m_Handle = BGFX_INVALID_HANDLE;
	};
}
