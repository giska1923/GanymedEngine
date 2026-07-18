#include "gepch.h"
#include "Buffer.h"

#include "Renderer.h"

#include "Platform/OpenGL/OpenGLBuffer.h"

namespace GanymedE {

	Ref<VertexBuffer> VertexBuffer::Create(uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None:
			GE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
			return nullptr;
		case RendererAPI::API::OpenGL:
			return CreateRef<OpenGLVertexBuffer>(size);
		case RendererAPI::API::Bgfx:
			// Dormant: the bgfx implementation lands in a later phase.
			// See docs/BGFX_MIGRATION.md.
			return nullptr;
		}

		GE_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	Ref<VertexBuffer> VertexBuffer::Create(float* vertices, uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None:
			GE_CORE_ASSERT(false, "RendererAPI::None is currently not supported.");
			return nullptr;
		case RendererAPI::API::OpenGL:
			return std::make_shared<OpenGLVertexBuffer>(vertices, size);
		case RendererAPI::API::Bgfx:
			// Dormant: the bgfx implementation lands in a later phase.
			// See docs/BGFX_MIGRATION.md.
			return nullptr;
		}

		GE_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

	Ref<IndexBuffer> IndexBuffer::Create(uint32_t* indices, uint32_t size)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None:
			GE_CORE_ASSERT(false, "RendererAPI::None is currently not supported.");
			return nullptr;
		case RendererAPI::API::OpenGL:
			return std::make_shared<OpenGLIndexBuffer>(indices, size);
		case RendererAPI::API::Bgfx:
			// Dormant: the bgfx implementation lands in a later phase.
			// See docs/BGFX_MIGRATION.md.
			return nullptr;
		}

		GE_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}