#include "gepch.h"
#include "Environment.h"

#include "Renderer.h"
#include "Platform/OpenGL/OpenGLEnvironment.h"

namespace GanymedE {

	Ref<Environment> Environment::Create(const std::string& filepath)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None:
			GE_CORE_ASSERT(false, "RendererAPI::None is currently not supported.");
			return nullptr;
		case RendererAPI::API::OpenGL:
			return CreateRef<OpenGLEnvironment>(filepath);
		}

		GE_CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}

}
