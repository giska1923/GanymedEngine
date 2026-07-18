#include "gepch.h"
#include "UniformBuffer.h"

#include "Renderer.h"
#include "Platform/OpenGL/OpenGLUniformBuffer.h"

namespace GanymedE {

	Ref<UniformBuffer> UniformBuffer::Create(uint32_t size, uint32_t binding)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None:
			GE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
			return nullptr;
		case RendererAPI::API::OpenGL:
			return CreateRef<OpenGLUniformBuffer>(size, binding);
		case RendererAPI::API::Bgfx:
			// Dormant: the bgfx implementation lands in a later phase.
			// See docs/BGFX_MIGRATION.md.
			return nullptr;
		}

		GE_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

}
