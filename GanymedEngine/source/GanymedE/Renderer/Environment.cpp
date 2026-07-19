#include "gepch.h"
#include "Environment.h"

#include "Renderer.h"

namespace GanymedE {

	Ref<Environment> Environment::Create(const std::string& filepath)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None:
			GE_CORE_ASSERT(false, "RendererAPI::None is currently not supported.");
			return nullptr;
		case RendererAPI::API::OpenGL:
			// OpenGLEnvironment was deleted in Phase 2: its IBL bake was built on
			// VertexArray, which bgfx has no equivalent for. Phase 4 rewrites the
			// bake against bgfx cubemap framebuffers.
			return nullptr;
		case RendererAPI::API::Bgfx:
			// Dormant: the bgfx implementation lands in Phase 4.
			// See docs/BGFX_MIGRATION.md.
			return nullptr;
		}

		GE_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

}
