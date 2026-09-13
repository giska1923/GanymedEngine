#pragma once

// Shared by LinuxWindow / macOSWindow. Windows uses a Win32 subclass instead:
// glfwSetWindowPos cannot provide Aero Snap, edge resize, or a drop shadow.

#include "GanymedE/Core/Window.h"

#include <GLFW/glfw3.h>

namespace GanymedE {

	struct TitleBarDragState
	{
		bool Dragging = false;
		bool WasDown = false;
		double LastClickTime = 0.0;
		double OriginX = 0.0;
		double OriginY = 0.0;
	};

	inline void TitleBarManualUpdate(GLFWwindow* window, const TitleBarHitTest& hit,
		TitleBarDragState& drag)
	{
		double cx = 0.0, cy = 0.0;
		glfwGetCursorPos(window, &cx, &cy);
		const bool down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
		const bool pressed = down && !drag.WasDown;
		drag.WasDown = down;

		if (!down)
		{
			drag.Dragging = false;
			return;
		}

		const bool draggable = hit.DraggableAt((float)cx, (float)cy);

		if (pressed && draggable)
		{
			const double now = glfwGetTime();
			if (drag.LastClickTime > 0.0 && (now - drag.LastClickTime) < 0.4)
			{
				if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
					glfwRestoreWindow(window);
				else
					glfwMaximizeWindow(window);
				drag.LastClickTime = 0.0;
				drag.Dragging = false;
				return;
			}

			drag.LastClickTime = now;
			drag.OriginX = cx;
			drag.OriginY = cy;
			if (!glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
				drag.Dragging = true;
		}

		if (!drag.Dragging || glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
			return;

		int wx = 0, wy = 0;
		glfwGetWindowPos(window, &wx, &wy);
		glfwSetWindowPos(window, wx + (int)(cx - drag.OriginX), wy + (int)(cy - drag.OriginY));
	}

}
