#pragma once

#include <cstdint>

namespace GanymedE {

	// Seedable PCG32 (Melissa O'Neill). 16 bytes of state, copyable: a copied Random
	// continues the same sequence from the copy point without aliasing the source's
	// stream — the property Scene::Copy needs for per-emitter particle RNG.
	//
	// std::mt19937 is 2.5KB/instance and its float distributions are not pinned across
	// standard-library implementations. UUID still uses a file-static mt19937_64; that
	// generator wants uncorrelated 64-bit ids, not a small replayable stream.
	//
	// Float01 is [0, 1) via division by 2^32. Domain mapping (cone directions, etc.)
	// stays at the call site.
	class Random
	{
	public:
		explicit Random(uint32_t seed)
		{
			// One uint32 → two uint64s via splitmix64, then the PCG seed dance
			// (inc forced odd, two dummy draws) so stream 0 is not special.
			uint64_t sm = seed;
			const uint64_t initState = SplitMix64(sm);
			const uint64_t initSeq = SplitMix64(sm);

			m_State = 0;
			m_Inc = (initSeq << 1u) | 1u;
			Next();
			m_State += initState;
			Next();
		}

		float Float01()
		{
			return static_cast<float>(UInt()) * 0x1p-32f; // 2^-32 → [0, 1)
		}

		float Range(float min, float max)
		{
			return min + (max - min) * Float01();
		}

		uint32_t UInt()
		{
			return Next();
		}

	private:
		static uint64_t SplitMix64(uint64_t& state)
		{
			uint64_t z = (state += 0x9E3779B97F4A7C15ull);
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}

		uint32_t Next()
		{
			const uint64_t old = m_State;
			m_State = old * 6364136223846793005ull + m_Inc;
			const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
			const uint32_t rot = static_cast<uint32_t>(old >> 59u);
			return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
		}

		uint64_t m_State;
		uint64_t m_Inc;
	};

	static_assert(sizeof(Random) == 16, "Random is two uint64s; copies must stay cheap");

}
