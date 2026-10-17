#pragma once

#include <cmath>
#include <cstdint>

namespace GanymedE::Net {

	// No map in this tree publishes a bound. ±8192 m at 1 mm fits in 24 bits with a bias
	// (0 .. 16,384,000, and 2^24 is 16,777,216). Past the bound the value clamps.
	constexpr int kPosSpanMm = 8192000;
	constexpr int kPosBits = 24;

	inline int QuantizeMeters(float meters)
	{
		long mm = std::lround(static_cast<double>(meters) * 1000.0);
		if (mm < -kPosSpanMm)
			mm = -kPosSpanMm;
		if (mm > kPosSpanMm)
			mm = kPosSpanMm;
		return static_cast<int>(mm);
	}

	inline uint32_t BiasMillimetres(int mm)
	{
		return static_cast<uint32_t>(mm + kPosSpanMm);
	}

	inline int UnbiasMillimetres(uint32_t packed)
	{
		return static_cast<int>(packed) - kPosSpanMm;
	}

	inline float MillimetresToMeters(int mm)
	{
		return static_cast<float>(mm) / 1000.0f;
	}

	// A full turn, uint16. Used for a character's yaw.
	inline uint16_t QuantizeYaw(float radians)
	{
		constexpr double kTwoPi = 6.28318530717958647692;
		double turns = std::fmod(static_cast<double>(radians), kTwoPi);
		if (turns < 0.0)
			turns += kTwoPi;
		int q = static_cast<int>(std::lround(turns / kTwoPi * 65536.0));
		if (q < 0)
			q = 0;
		if (q > 65535)
			q = 65535;
		return static_cast<uint16_t>(q);
	}

	inline float UnquantizeYaw(uint16_t packed)
	{
		constexpr float kTwoPi = 6.28318530717958647692f;
		return static_cast<float>(packed) / 65536.0f * kTwoPi;
	}

	// One axis of an Euler triple, wrapped into [-pi, pi] and stored in 16 bits.
	inline uint16_t QuantizeSignedAngle(float radians)
	{
		constexpr double kPi = 3.14159265358979323846;
		double wrapped = std::fmod(static_cast<double>(radians) + kPi, 2.0 * kPi);
		if (wrapped < 0.0)
			wrapped += 2.0 * kPi;
		wrapped -= kPi;
		int q = static_cast<int>(std::lround((wrapped + kPi) / (2.0 * kPi) * 65535.0));
		if (q < 0)
			q = 0;
		if (q > 65535)
			q = 65535;
		return static_cast<uint16_t>(q);
	}

	inline float UnquantizeSignedAngle(uint16_t packed)
	{
		constexpr float kPi = 3.14159265358979323846f;
		return static_cast<float>(packed) / 65535.0f * (2.0f * kPi) - kPi;
	}

	// ±32 m/s at 1 mm/s. Signed 16 bits stops at 32767, so the clamp is 32000.
	inline int16_t QuantizeSpeed(float metresPerSecond)
	{
		long mm = std::lround(static_cast<double>(metresPerSecond) * 1000.0);
		if (mm < -32000)
			mm = -32000;
		if (mm > 32000)
			mm = 32000;
		return static_cast<int16_t>(mm);
	}

	inline float UnquantizeSpeed(int16_t mmPerSecond)
	{
		return static_cast<float>(mmPerSecond) / 1000.0f;
	}

}
