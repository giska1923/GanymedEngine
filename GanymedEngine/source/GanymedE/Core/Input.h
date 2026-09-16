#pragma once

#include <glm/glm.hpp>

#include "KeyCodes.h"
#include "MouseButtonCodes.h"

namespace GanymedE {

	// What the OS cursor does while this window has focus.
	enum class CursorMode : uint8_t
	{
		// Visible, free to leave the window. The editor's mode, and the default.
		Normal = 0,

		// Invisible over the window but still a normal cursor underneath - it moves, it can
		// leave, and its position is still meaningful. For a game that draws its own crosshair
		// but still wants pointer-style aiming.
		Hidden,

		// Captured: hidden, held to the window, and given unbounded virtual coordinates so it
		// never reaches a screen edge. This is the mouse-look mode, and it is the whole reason
		// GetMouseDelta exists - a locked cursor has no meaningful absolute position, so
		// GetMousePosition stops being the right question to ask.
		Locked
	};

	class GE_API Input
	{
	public:
		static bool IsKeyPressed(KeyCode key);

		static bool IsMouseButtonPressed(MouseCode button);
		static glm::vec2 GetMousePosition();
		static float GetMouseX();
		static float GetMouseY();

		// How far the mouse moved during the *previous* frame, in pixels. Sampled once per frame
		// by Application::Run, so every caller in a frame sees the same value - a lazily computed
		// delta would hand the second caller in a frame a zero and be maddening to debug.
		//
		// Valid in every cursor mode; it is only *necessary* in Locked, where the absolute
		// position is a virtual coordinate that means nothing on its own.
		static glm::vec2 GetMouseDelta();

		static void SetCursorMode(CursorMode mode);
		static CursorMode GetCursorMode();

		// Called by Application::Run, once, before the layers update. Not for gameplay.
		static void NewFrame();
	};
}
