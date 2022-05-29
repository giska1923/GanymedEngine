#include "gepch.h"
#include "RenderCommand.h"

#include "Platform/OpenGL/OpenGLRendererAPI.h"

namespace GanymedE {
	RendererAPI* RenderCommand::s_RendererAPI = new OpenGLRendererAPI;
}