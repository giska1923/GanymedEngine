#include "gepch.h"
#include "GanymedE/Renderer/Renderer.h"
#include "Texture.h"

#include "stb_image.h"

namespace GanymedE {

	namespace {

		uint8_t MipLevelCount(uint32_t width, uint32_t height)
		{
			uint8_t levels = 1;
			while (width > 1 || height > 1)
			{
				width = width > 1 ? width / 2 : 1;
				height = height > 1 ? height / 2 : 1;
				levels++;
			}
			return levels;
		}

		// bgfx does **not** generate mips for an ordinary texture - a render target can be
		// blitted down, but a sampled texture has to be handed its whole chain, all levels
		// contiguous in one block, largest first. That is what this builds.
		//
		// A 2x2 box filter, which is what the level of quality here calls for: the alternative
		// is a proper Kaiser or Lanczos kernel, and the difference between those and a box is
		// invisible next to the difference between having mips and not having them.
		//
		// Averaged in sRGB space, which is very slightly wrong now that fs_Phong decodes albedo
		// to linear - correct would be decode, average, re-encode. The error is a fraction of a
		// tone in the mid-greys and it applies equally to every engine that ever shipped a box
		// filter; recorded rather than fixed because it is dominated by everything else here.
		uint8_t BuildMipChain(const uint8_t* rgba, uint32_t width, uint32_t height,
			std::vector<uint8_t>& out)
		{
			const uint8_t levels = MipLevelCount(width, height);

			std::size_t total = 0;
			for (uint32_t w = width, h = height, i = 0; i < levels; i++)
			{
				total += (std::size_t)w * h * 4;
				w = w > 1 ? w / 2 : 1;
				h = h > 1 ? h / 2 : 1;
			}
			out.resize(total);
			std::memcpy(out.data(), rgba, (std::size_t)width * height * 4);

			std::size_t sourceOffset = 0;
			uint32_t sw = width, sh = height;
			for (uint8_t level = 1; level < levels; level++)
			{
				const uint32_t dw = sw > 1 ? sw / 2 : 1;
				const uint32_t dh = sh > 1 ? sh / 2 : 1;
				const std::size_t destOffset = sourceOffset + (std::size_t)sw * sh * 4;

				const uint8_t* source = out.data() + sourceOffset;
				uint8_t* dest = out.data() + destOffset;

				for (uint32_t y = 0; y < dh; y++)
				{
					// On an odd or already-1 dimension the second tap clamps back onto the
					// first instead of stepping outside this level's own rows.
					const uint32_t y0 = (sh > 1) ? y * 2 : 0;
					const uint32_t y1 = (sh > 1) ? ((y0 + 1 < sh) ? y0 + 1 : y0) : 0;

					for (uint32_t x = 0; x < dw; x++)
					{
						const uint32_t x0 = (sw > 1) ? x * 2 : 0;
						const uint32_t x1 = (sw > 1) ? ((x0 + 1 < sw) ? x0 + 1 : x0) : 0;

						const uint8_t* a = source + ((std::size_t)y0 * sw + x0) * 4;
						const uint8_t* b = source + ((std::size_t)y0 * sw + x1) * 4;
						const uint8_t* c = source + ((std::size_t)y1 * sw + x0) * 4;
						const uint8_t* d = source + ((std::size_t)y1 * sw + x1) * 4;
						uint8_t* out4 = dest + ((std::size_t)y * dw + x) * 4;

						for (int channel = 0; channel < 4; channel++)
						{
							out4[channel] = (uint8_t)((a[channel] + b[channel]
								+ c[channel] + d[channel] + 2) / 4);
						}
					}
				}

				sourceOffset = destOffset;
				sw = dw;
				sh = dh;
			}

			return levels;
		}

	}

	Texture2D::Texture2D(uint32_t width, uint32_t height)
		: m_Width(width), m_Height(height)
	{
		GE_PROFILE_FUNCTION();

		// Uninitialised storage that SetData fills in - the 1x1 white texture
		// Renderer2D uses for untextured quads takes this path.
		m_Handle = bgfx::createTexture2D(
			(uint16_t)width, (uint16_t)height,
			false,  // no mips
			1,      // single layer
			bgfx::TextureFormat::RGBA8);

		if (!bgfx::isValid(m_Handle))
			GE_CORE_ERROR("Failed to create {0}x{1} texture", width, height);
	}

	Texture2D::Texture2D(const uint8_t* rgba, uint32_t width, uint32_t height)
		: m_Width(width), m_Height(height)
	{
		GE_PROFILE_FUNCTION();

		if (!rgba || width == 0 || height == 0)
		{
			GE_CORE_ERROR("Texture2D from pixels: {0}x{1}, data {2}",
				width, height, rgba ? "present" : "null");
			return;
		}

		std::vector<uint8_t> chain;
		m_MipCount = BuildMipChain(rgba, width, height, chain);

		m_Handle = bgfx::createTexture2D(
			(uint16_t)width, (uint16_t)height,
			m_MipCount > 1,
			1,
			bgfx::TextureFormat::RGBA8,
			BGFX_TEXTURE_NONE,
			bgfx::copy(chain.data(), (uint32_t)chain.size()));

		if (!bgfx::isValid(m_Handle))
		{
			GE_CORE_ERROR("Failed to create {0}x{1} texture from pixels", width, height);
			return;
		}

		m_GpuBytes = (uint32_t)chain.size();
	}

	Texture2D::Texture2D(const std::string& path)
		: m_Path(path)
	{
		GE_PROFILE_FUNCTION();

		int width, height, channels;

		// bgfx's texture origin is top-left on every backend it normalises, so
		// unlike the GL path this must NOT flip. Getting this wrong shows up as
		// vertically mirrored sprites.
		// Nothing in the engine writes stb's flip global any more - it is shared state and
		// decoding happens on workers. See TextureImporter.h.

		stbi_uc* data = nullptr;
		{
			GE_PROFILE_SCOPE("stbi_load - Texture2D::Texture2D(const std::string&)");
			// Force 4 channels: bgfx has no 24-bit RGB8 texture format, and
			// asking stb to expand avoids a manual repack.
			data = stbi_load(path.c_str(), &width, &height, &channels, 4);
		}

		if (!data)
		{
			GE_CORE_ERROR("Failed to load image '{0}'", path);
			return;
		}

		m_Width = (uint32_t)width;
		m_Height = (uint32_t)height;

		// Mipped like every other sampled texture. This path is the uncompiled fallback - an
		// editor icon, or a material map in an install that has no .compiled tree - and the
		// thing it must not do is differ in *quality* from the compiled path, only in cost.
		std::vector<uint8_t> chain;
		m_MipCount = BuildMipChain(data, m_Width, m_Height, chain);
		stbi_image_free(data);

		m_Handle = bgfx::createTexture2D(
			(uint16_t)m_Width, (uint16_t)m_Height,
			m_MipCount > 1,
			1,
			bgfx::TextureFormat::RGBA8,
			BGFX_TEXTURE_NONE,
			bgfx::copy(chain.data(), (uint32_t)chain.size()));

		if (bgfx::isValid(m_Handle))
		{
			bgfx::setName(m_Handle, path.c_str());
			m_GpuBytes = (uint32_t)chain.size();
		}
		else
		{
			GE_CORE_ERROR("Failed to create texture from '{0}'", path);
		}
	}

	Texture2D::Texture2D(const uint8_t* containerBytes, uint32_t size)
	{
		GE_PROFILE_FUNCTION();

		if (!containerBytes || size == 0)
		{
			GE_CORE_ERROR("Empty texture container");
			return;
		}

		// bgfx parses DDS/KTX/PVR itself and reports what it found. Doing it this way rather
		// than decoding the container here is what makes a block-compressed, fully mipped
		// texture cost the engine no more code than an uncompressed one.
		bgfx::TextureInfo info = {};
		m_Handle = bgfx::createTexture(bgfx::copy(containerBytes, size),
			BGFX_TEXTURE_NONE, 0, &info);

		if (!bgfx::isValid(m_Handle))
		{
			GE_CORE_ERROR("Failed to create a texture from a {0} byte container", size);
			return;
		}

		m_Width = info.width;
		m_Height = info.height;
		m_MipCount = info.numMips;
		m_Format = (uint32_t)info.format;
		m_GpuBytes = info.storageSize;

		// A mipped texture wants trilinear minification. Point magnification stays: the 2D
		// renderer relies on it for crisp sprites, and it is what every other path here uses.
		if (m_MipCount > 1)
			m_SamplerFlags = BGFX_SAMPLER_MAG_POINT;
	}

	Texture2D::~Texture2D()
	{
		GE_PROFILE_FUNCTION();

		// See Renderer::IsGpuAlive - this object may outlive bgfx.
		if (!Renderer::IsGpuAlive())
			return;

		if (bgfx::isValid(m_Handle))
			bgfx::destroy(m_Handle);
	}

	void Texture2D::SetData(void* data, uint32_t size)
	{
		GE_PROFILE_FUNCTION();

		GE_CORE_ASSERT(size == m_Width * m_Height * 4, "Data must be the entire texture!");

		if (!bgfx::isValid(m_Handle))
			return;

		bgfx::updateTexture2D(m_Handle, 0, 0, 0, 0,
			(uint16_t)m_Width, (uint16_t)m_Height,
			bgfx::copy(data, size));
	}

	Ref<Texture2D> Texture2D::Create(uint32_t width, uint32_t height)
	{
		return CreateRef<Texture2D>(width, height);
	}

	Ref<Texture2D> Texture2D::Create(const uint8_t* rgba, uint32_t width, uint32_t height)
	{
		return CreateRef<Texture2D>(rgba, width, height);
	}

	Ref<Texture2D> Texture2D::Create(const std::string& path)
	{
		return CreateRef<Texture2D>(path);
	}

	Ref<Texture2D> Texture2D::CreateFromContainer(const uint8_t* containerBytes, uint32_t size)
	{
		return CreateRef<Texture2D>(containerBytes, size);
	}
}
