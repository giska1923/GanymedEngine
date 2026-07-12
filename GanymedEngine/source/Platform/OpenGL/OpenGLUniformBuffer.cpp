#include "gepch.h"
#include "OpenGLUniformBuffer.h"

#include <glad/glad.h>

namespace GanymedE {

	OpenGLUniformBuffer::OpenGLUniformBuffer(uint32_t size, uint32_t binding)
	{
		glGenBuffers(1, &m_RendererID);
		glBindBuffer(GL_UNIFORM_BUFFER, m_RendererID);
		glBufferData(GL_UNIFORM_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
		glBindBufferBase(GL_UNIFORM_BUFFER, binding, m_RendererID);
	}

	OpenGLUniformBuffer::~OpenGLUniformBuffer()
	{
		glDeleteBuffers(1, &m_RendererID);
	}

	void OpenGLUniformBuffer::SetData(const void* data, uint32_t size, uint32_t offset)
	{
		glBindBuffer(GL_UNIFORM_BUFFER, m_RendererID);
		glBufferSubData(GL_UNIFORM_BUFFER, offset, size, data);
	}

	void OpenGLUniformBuffer::Bind(uint32_t binding) const
	{
		glBindBufferBase(GL_UNIFORM_BUFFER, binding, m_RendererID);
	}

}
