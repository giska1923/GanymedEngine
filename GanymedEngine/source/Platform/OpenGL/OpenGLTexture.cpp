#include "gepch.h"
#include "OpenGLTexture.h"

#include "stb_image.h"

#include <glad/glad.h>

namespace GanymedE {
	OpenGLTexture2D::OpenGLTexture2D(uint32_t width, uint32_t height)
		: m_Width(width), m_Height(height)
	{
		GE_PROFILE_FUNCTION();

		m_InternalFormat = GL_RGBA8;
		m_DataFormat = GL_RGBA;

		glGenTextures(1, &m_RendererID);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
		glTexImage2D(GL_TEXTURE_2D, 0, m_InternalFormat, m_Width, m_Height, 0, m_DataFormat, GL_UNSIGNED_BYTE, nullptr);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}

	OpenGLTexture2D::OpenGLTexture2D(const std::string& path)
		: m_Path(path)
	{
		GE_PROFILE_FUNCTION();

		int width, height, channels;
		stbi_set_flip_vertically_on_load(1);
		stbi_uc* data = nullptr;
		{
			GE_PROFILE_SCOPE("stbi_load - OpenGLTexture2D::OpenGLTexture2D(const std::string&)");
			data = stbi_load(path.c_str(), &width, &height, &channels, 0);
		}
		GE_CORE_ASSERT(data, "Failed to load image!");
		m_Width = width;
		m_Height = height;

		GLenum internalFormat = 0, dataFormat = 0;
		if (channels == 4)
		{
			internalFormat = GL_RGBA8;
			dataFormat = GL_RGBA;
		}
		else if (channels == 3)
		{
			internalFormat = GL_RGB8;
			dataFormat = GL_RGB;
		}

		m_InternalFormat = internalFormat;
		m_DataFormat = dataFormat;

		GE_CORE_ASSERT(internalFormat & dataFormat, "Format not supported!");

		glGenTextures(1, &m_RendererID);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);

		// Tightly packed rows from stb_image; RGB rows are not 4-byte aligned
		if (dataFormat == GL_RGB)
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, m_Width, m_Height, 0, dataFormat, GL_UNSIGNED_BYTE, data);

		if (dataFormat == GL_RGB)
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

		stbi_image_free(data);
	}

	OpenGLTexture2D::~OpenGLTexture2D()
	{
		GE_PROFILE_FUNCTION();

		glDeleteTextures(1, &m_RendererID);
	}

	void OpenGLTexture2D::SetData(void* data, uint32_t size)
	{
		GE_PROFILE_FUNCTION();

		uint32_t bpp = m_DataFormat == GL_RGBA ? 4 : 3;
		GE_CORE_ASSERT(size == m_Width * m_Height * bpp, "Data must be entire texture!");
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Width, m_Height, m_DataFormat, GL_UNSIGNED_BYTE, data);
	}

	void OpenGLTexture2D::Bind(uint32_t slot) const
	{
		GE_PROFILE_FUNCTION();

		glActiveTexture(GL_TEXTURE0 + slot);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
	}
}
