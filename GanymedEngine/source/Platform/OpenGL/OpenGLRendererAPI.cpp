#include "gepch.h"
#include "OpenGLRendererAPI.h"

#include <glad/glad.h>

namespace GanymedE {

	static GLenum DepthFuncToGL(RendererAPI::DepthFunc func)
	{
		switch (func)
		{
			case RendererAPI::DepthFunc::Never:       return GL_NEVER;
			case RendererAPI::DepthFunc::Less:        return GL_LESS;
			case RendererAPI::DepthFunc::Equal:       return GL_EQUAL;
			case RendererAPI::DepthFunc::LessEqual:   return GL_LEQUAL;
			case RendererAPI::DepthFunc::Greater:     return GL_GREATER;
			case RendererAPI::DepthFunc::NotEqual:    return GL_NOTEQUAL;
			case RendererAPI::DepthFunc::GreaterEqual:return GL_GEQUAL;
			case RendererAPI::DepthFunc::Always:      return GL_ALWAYS;
		}
		return GL_LESS;
	}

	void OpenGLRendererAPI::Init()
	{
		GE_PROFILE_FUNCTION();

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glEnable(GL_DEPTH_TEST);
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}

	void OpenGLRendererAPI::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		glViewport(x, y, width, height);
	}

	void OpenGLRendererAPI::SetClearColor(const glm::vec4& color)
	{
		glClearColor(color.r, color.g, color.b, color.a);
	}

	void OpenGLRendererAPI::Clear()
	{
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	void OpenGLRendererAPI::SetDepthTest(bool enabled)
	{
		if (enabled)
			glEnable(GL_DEPTH_TEST);
		else
			glDisable(GL_DEPTH_TEST);
	}

	void OpenGLRendererAPI::SetDepthWrite(bool enabled)
	{
		glDepthMask(enabled ? GL_TRUE : GL_FALSE);
	}

	void OpenGLRendererAPI::SetDepthFunc(DepthFunc func)
	{
		glDepthFunc(DepthFuncToGL(func));
	}

	void OpenGLRendererAPI::SetCullFace(bool enabled)
	{
		if (enabled)
			glEnable(GL_CULL_FACE);
		else
			glDisable(GL_CULL_FACE);
	}

	void OpenGLRendererAPI::SetCullMode(CullMode mode)
	{
		switch (mode)
		{
			case CullMode::Front:        glCullFace(GL_FRONT); break;
			case CullMode::Back:         glCullFace(GL_BACK); break;
			case CullMode::FrontAndBack: glCullFace(GL_FRONT_AND_BACK); break;
		}
	}

	void OpenGLRendererAPI::SetBlend(bool enabled)
	{
		if (enabled)
			glEnable(GL_BLEND);
		else
			glDisable(GL_BLEND);
	}

	void OpenGLRendererAPI::SetBlendMode(BlendMode mode)
	{
		switch (mode)
		{
			case BlendMode::Alpha:    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
			case BlendMode::Additive: glBlendFunc(GL_ONE, GL_ONE); break;
		}
	}

	void OpenGLRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount)
	{
		vertexArray->Bind();
		uint32_t count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();
		glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
	}

	void OpenGLRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex)
	{
		vertexArray->Bind();
		glDrawElementsBaseVertex(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT,
			(void*)(sizeof(uint32_t) * baseIndex), baseVertex);
	}

	void OpenGLRendererAPI::DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex, uint32_t instanceCount)
	{
		vertexArray->Bind();
		glDrawElementsInstancedBaseVertex(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT,
			(void*)(sizeof(uint32_t) * baseIndex), instanceCount, baseVertex);
	}

	void OpenGLRendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount)
	{
		vertexArray->Bind();
		glDrawArrays(GL_LINES, 0, vertexCount);
	}

}
