#pragma once

#include "GanymedE/Core/Input.h"

namespace GanymedE {
	class WindowsInput : public Input {
	protected:
		virtual bool IsKeyPressedImpl(int keycode) override;

		virtual bool IsMousePressedImpl(int button) override;
		virtual std::pair<float, float> GetMousePositionImpl() override;
		virtual float GetMouseXImpl() override;
		virtual float GetMouseYImpl() override;
	};
}