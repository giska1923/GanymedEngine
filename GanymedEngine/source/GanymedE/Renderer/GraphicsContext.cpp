#include "gepch.h"
#include "GraphicsContext.h"

#include "GanymedE/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLContext.h"

namespace GanymedE {

	Scope<GraphicsContext> GraphicsContext::Create(void* window)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None:
			GE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
			return nullptr;
		case RendererAPI::API::OpenGL:
			return CreateScope<OpenGLContext>(static_cast<GLFWwindow*>(window));
		case RendererAPI::API::Bgfx:
			// Dormant: the bgfx implementation lands in a later phase.
			// See docs/BGFX_MIGRATION.md.
			return nullptr;
		}

		GE_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}
}
