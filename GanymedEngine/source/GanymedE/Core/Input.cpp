#include "gepch.h"
#include "GanymedE/Core/Input.h"

#include "GanymedE/main/Application.h"

#include <GLFW/glfw3.h>

namespace GanymedE {

	// ---- Why this file is not under Platform/ ----
	//
	// IsKeyPressed and friends live in Platform/{Windows,Linux,macOS}Input.cpp as three
	// `#ifdef`-guarded copies of identical GLFW code. That triplication predates GLFW being the
	// only windowing backend and is not worth reproducing: everything below is written once,
	// because there is exactly one implementation of it to write. Consolidating the existing
	// three is a separate change - see docs/ToDo/cross-cutting.md.

	namespace {

		struct CursorState
		{
			CursorMode Mode = CursorMode::Normal;

			glm::vec2 Last{ 0.0f };
			glm::vec2 Delta{ 0.0f };

			// The frame a mode change lands on is not a frame with a meaningful delta: switching
			// to Locked warps the cursor and switching back restores it, and either shows up as
			// one enormous jump. Swallowing exactly one frame is the whole fix.
			bool Resync = true;
		};

		CursorState s_Cursor;

		// Same reach as the platform Input files use. No null guard on the Application for the
		// same reason they have none: Input is only live while one is running.
		GLFWwindow* NativeWindow()
		{
			return static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		}

	}

	glm::vec2 Input::GetMouseDelta()
	{
		return s_Cursor.Delta;
	}

	CursorMode Input::GetCursorMode()
	{
		return s_Cursor.Mode;
	}

	void Input::SetCursorMode(CursorMode mode)
	{
		GLFWwindow* window = NativeWindow();
		if (!window)
			return;

		if (mode == s_Cursor.Mode)
			return;

		int value = GLFW_CURSOR_NORMAL;
		switch (mode)
		{
			case CursorMode::Hidden: value = GLFW_CURSOR_HIDDEN; break;
			case CursorMode::Locked: value = GLFW_CURSOR_DISABLED; break;
			default: break;
		}

		glfwSetInputMode(window, GLFW_CURSOR, value);

		// Raw motion skips the OS pointer acceleration curve, which is what a desktop cursor
		// wants and what mouse-look does not: acceleration makes the same physical movement turn
		// the camera by different amounts depending on how fast it was made. GLFW only honours it
		// while the cursor is disabled, and not every platform has it - hence the support query
		// rather than an unconditional set.
		if (glfwRawMouseMotionSupported())
			glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
				mode == CursorMode::Locked ? GLFW_TRUE : GLFW_FALSE);

		s_Cursor.Mode = mode;
		s_Cursor.Resync = true;
		s_Cursor.Delta = { 0.0f, 0.0f };
	}

	void Input::NewFrame()
	{
		GLFWwindow* window = NativeWindow();
		if (!window)
			return;

		double x = 0.0, y = 0.0;
		glfwGetCursorPos(window, &x, &y);
		const glm::vec2 position{ (float)x, (float)y };

		if (s_Cursor.Resync)
		{
			s_Cursor.Resync = false;
			s_Cursor.Delta = { 0.0f, 0.0f };
		}
		else
		{
			s_Cursor.Delta = position - s_Cursor.Last;
		}

		s_Cursor.Last = position;
	}

}
