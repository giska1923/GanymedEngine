#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GanymedE::Net {

	// The four types a script may name in `Replicated`. The number is the 2-bit tag on the wire,
	// so a schema mismatch cannot slide the rest of the bitstream. There is no float: a quantized
	// float that does not round-trip is a desync with no error. No tables and no functions.
	constexpr uint8_t kFieldBool = 0;
	constexpr uint8_t kFieldU8 = 1;
	constexpr uint8_t kFieldTick = 2;
	constexpr uint8_t kFieldStr16 = 3;

	struct FieldValue
	{
		uint8_t Index = 0;
		uint8_t Type = kFieldU8;
		uint32_t Number = 0;
		std::string Text;
	};

	// Arguments of one `Net.Broadcast`. Flat, and small enough that a reliable copy fits the
	// 256-byte reliable payload. A nested table is rejected at the binding.
	constexpr uint8_t kArgBool = 0;
	constexpr uint8_t kArgNumber = 1;
	constexpr uint8_t kArgString = 2;
	constexpr uint8_t kArgVec3 = 3;

	struct EventArg
	{
		std::string Key;
		uint8_t Type = kArgNumber;
		float Number = 0.0f;
		std::string Text;
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
	};

}
