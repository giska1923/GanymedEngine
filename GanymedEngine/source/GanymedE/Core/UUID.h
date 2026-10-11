#pragma once

#include <cstdint>
#include <functional>

namespace GanymedE {

	class UUID
	{
	public:
		UUID();
		UUID(uint64_t uuid);
		UUID(const UUID&) = default;

		explicit operator uint64_t() const { return m_UUID; }

		bool operator==(const UUID& other) const { return m_UUID == other.m_UUID; }
		bool operator!=(const UUID& other) const { return m_UUID != other.m_UUID; }
	private:
		uint64_t m_UUID;
	};

	// Child i of a prefab instance. Index 0 is the root and is not derived: the caller minted it
	// or pinned it. The same root and the same index produce the same id on every machine, which
	// is how a server and a client name one child. Two instances differ because their roots differ.
	inline UUID DerivePrefabChildUUID(UUID root, uint32_t index)
	{
		uint64_t z = static_cast<uint64_t>(root)
			+ 0x9E3779B97F4A7C15ull * (static_cast<uint64_t>(index) + 1ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		z = z ^ (z >> 31);
		if (z == 0)
			z = 1;
		return UUID(z);
	}

}

namespace std {

	template<>
	struct hash<GanymedE::UUID>
	{
		std::size_t operator()(const GanymedE::UUID& uuid) const
		{
			return hash<uint64_t>()(static_cast<uint64_t>(uuid));
		}
	};

}
