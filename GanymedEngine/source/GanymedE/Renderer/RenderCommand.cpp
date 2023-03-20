#include "gepch.h"
#include "RenderCommand.h"

#include "Platform/OpenGL/OpenGLRendererAPI.h"

namespace GanymedE {
	Scope<RendererAPI> RenderCommand::s_RendererAPI =  CreateScope<OpenGLRendererAPI>();
}