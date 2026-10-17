#pragma once

#include <cstdint>

namespace GanymedE::Net {

	// One tick of what the player did. Sampled on the client, or from the keyboard when there
	// is no server. The wire form is the last four of these in a command packet: i16 sticks,
	// u16 yaw, u16 pitch, and one button byte. Equip is the high nibble (0–15).
	struct PlayerCommand
	{
		uint32_t Tick = 0;
		float MoveX = 0.0f;
		float MoveZ = 0.0f;
		float Yaw = 0.0f;
		float Pitch = 0.0f;
		bool Jump = false;
		bool Fire = false;
		bool Melee = false;
		bool Interact = false;
		uint8_t Equip = 0;

		uint8_t PackButtons() const
		{
			uint8_t buttons = 0;
			if (Jump)
				buttons |= 1u;
			if (Fire)
				buttons |= 2u;
			if (Melee)
				buttons |= 4u;
			if (Interact)
				buttons |= 8u;
			buttons |= static_cast<uint8_t>((Equip & 15u) << 4);
			return buttons;
		}

		void UnpackButtons(uint8_t buttons)
		{
			Jump = (buttons & 1u) != 0;
			Fire = (buttons & 2u) != 0;
			Melee = (buttons & 4u) != 0;
			Interact = (buttons & 8u) != 0;
			Equip = static_cast<uint8_t>((buttons >> 4) & 15u);
		}
	};

}
