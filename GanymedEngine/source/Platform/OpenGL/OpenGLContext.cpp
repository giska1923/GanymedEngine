#include "gepch.h"
#include "OpenGLContext.h"

#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace GanymedE {

	OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
		: m_WindowHandle(windowHandle)
	{
		GE_CORE_ASSERT(windowHandle, "Window handle is null!");
	}

	void OpenGLContext::Init()
	{
		GE_PROFILE_FUNCTION();

		glfwMakeContextCurrent(m_WindowHandle);
		int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		GE_CORE_ASSERT(status, "Failed to initialize Glad!");

		GE_CORE_INFO("OpenGL Info:", glGetString(GL_VENDOR));
		GE_CORE_INFO("  Vendor: {0}", glGetString(GL_VENDOR));
		GE_CORE_INFO("  Renderer: {0}", glGetString(GL_RENDERER));
		GE_CORE_INFO("  Version: {0}", glGetString(GL_VERSION));
	}

	void OpenGLContext::SwapBuffers()
	{
		GE_PROFILE_FUNCTION();

		glfwSwapBuffers(m_WindowHandle);
	}
}