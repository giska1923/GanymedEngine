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

		// The mode gameplay *asked for*. It only reaches the OS while the game has focus (below);
		// without focus the cursor is Normal, and the request is re-applied when focus returns.
		// GetCursorMode reads back the request, so a script's view of its own cursor never flips
		// under it.
		static void SetCursorMode(CursorMode mode);
		static CursorMode GetCursorMode();

		// ---- Game focus ----
		//
		// Whether gameplay owns the keyboard and mouse. The runtime never touches it, so there it
		// is true for the life of the process. The editor shares one window between its panels and
		// the game in its viewport, so it hands focus over on a viewport click and takes it back.
		//
		// Focus arriving while the game wants a Locked cursor masks the mouse buttons already held:
		// that click was the editor's "give it back", not a shot. With a Normal cursor the click
		// is passed through, because there it lands on something the game can see.
		static void SetGameFocus(bool focused);
		static bool HasGameFocus();

		// What gameplay reads: the raw answer while the game has focus, nothing pressed and no
		// motion while it does not. Editor code keeps the raw queries above.
		static bool IsGameKeyPressed(KeyCode key);
		static bool IsGameMouseButtonPressed(MouseCode button);
		static glm::vec2 GetGameMouseDelta();

		// Called by Application::Run, once, before the layers update. Not for gameplay.
		static void NewFrame();
	};
}
