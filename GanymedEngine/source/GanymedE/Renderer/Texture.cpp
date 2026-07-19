#include "gepch.h"
#include "Texture.h"

#include "stb_image.h"

namespace GanymedE {

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

	Texture2D::Texture2D(const std::string& path)
		: m_Path(path)
	{
		GE_PROFILE_FUNCTION();

		int width, height, channels;

		// bgfx's texture origin is top-left on every backend it normalises, so
		// unlike the GL path this must NOT flip. Getting this wrong shows up as
		// vertically mirrored sprites.
		stbi_set_flip_vertically_on_load(0);

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

		// bgfx::copy so stb's buffer can be freed immediately.
		const bgfx::Memory* mem = bgfx::copy(data, (uint32_t)(width * height * 4));
		stbi_image_free(data);

		m_Handle = bgfx::createTexture2D(
			(uint16_t)m_Width, (uint16_t)m_Height,
			false,
			1,
			bgfx::TextureFormat::RGBA8,
			BGFX_TEXTURE_NONE,
			mem);

		if (bgfx::isValid(m_Handle))
			bgfx::setName(m_Handle, path.c_str());
		else
			GE_CORE_ERROR("Failed to create texture from '{0}'", path);
	}

	Texture2D::~Texture2D()
	{
		GE_PROFILE_FUNCTION();

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

	Ref<Texture2D> Texture2D::Create(const std::string& path)
	{
		return CreateRef<Texture2D>(path);
	}
}
