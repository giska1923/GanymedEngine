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
