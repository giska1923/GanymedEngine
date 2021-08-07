#pragma once

#include "GanymedE/Renderer/GraphicsContext.h"

struct GLFWwindow;

namespace GanymedE {

	class OpenGLContext : public GraphicsContext
	{
	public:
		OpenGLContext(GLFWwindow* windowHandle);

		void Init() override;
		void SwapBuffers() override;
	private:
		GLFWwindow* m_WindowHandle;
	};
}