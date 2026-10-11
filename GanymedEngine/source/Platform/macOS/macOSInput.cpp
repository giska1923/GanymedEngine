#include "gepch.h"
#include "GanymedE/Core/Input.h"

#include "GanymedE/main/Application.h"
#include <GLFW/glfw3.h>

namespace GanymedE {

#ifdef GE_PLATFORM_MACOS
	bool Input::IsKeyPressed(const KeyCode key)
	{
		if (!Application::Get().HasWindow())
			return false;
		auto* window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		auto state = glfwGetKey(window, static_cast<int32_t>(key));
		return state == GLFW_PRESS || state == GLFW_REPEAT;
	}

	bool Input::IsMouseButtonPressed(const MouseCode button)
	{
		if (!Application::Get().HasWindow())
			return false;
		auto* window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		auto state = glfwGetMouseButton(window, static_cast<int32_t>(button));
		return state == GLFW_PRESS;
	}

	glm::vec2 Input::GetMousePosition()
	{
		if (!Application::Get().HasWindow())
			return { 0.0f, 0.0f };
		auto* window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		double xpos, ypos;
		glfwGetCursorPos(window, &xpos, &ypos);

		return { (float)xpos, (float)ypos };
	}

	float Input::GetMouseX()
	{
		return GetMousePosition().x;
	}

	float Input::GetMouseY()
	{
		return GetMousePosition().y;
	}
#endif

}