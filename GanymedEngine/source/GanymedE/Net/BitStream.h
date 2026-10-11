#pragma once

#include <cstdint>
#include <vector>

namespace GanymedE::Net {

	// One bit order for write, read, and measure. Bit 0 of the value is the first bit on the
	// wire, little-endian within the byte. A codec calls Bits() and cannot write a layout the
	// reader does not expect, because there is no second implementation.

	class BitWrite
	{
	public:
		static constexpr bool Reading = false;

		explicit BitWrite(std::vector<uint8_t>& out) : m_Out(out) {}

		void Put(uint32_t value, int bits)
		{
			for (int i = 0; i < bits; i++)
			{
				const int byte = m_Bit >> 3;
				const int offset = m_Bit & 7;
				if (byte >= static_cast<int>(m_Out.size()))
					m_Out.push_back(0);
				if ((value >> i) & 1u)
					m_Out[static_cast<size_t>(byte)] |= static_cast<uint8_t>(1u << offset);
				m_Bit++;
			}
		}

		int Bytes() const { return (m_Bit + 7) / 8; }

	private:
		std::vector<uint8_t>& m_Out;
		int m_Bit = 0;
	};

	class BitMeasure
	{
	public:
		static constexpr bool Reading = false;

		void Put(uint32_t, int bits) { m_Bit += bits; }
		int Bytes() const { return (m_Bit + 7) / 8; }

	private:
		int m_Bit = 0;
	};

	class BitRead
	{
	public:
		static constexpr bool Reading = true;

		BitRead(const uint8_t* data, int length) : m_Data(data), m_Length(length) {}

		uint32_t Take(int bits)
		{
			uint32_t value = 0;
			for (int i = 0; i < bits; i++)
			{
				const int byte = m_Bit >> 3;
				const int offset = m_Bit & 7;
				if (!m_Data || byte >= m_Length)
				{
					m_Ok = false;
					return 0;
				}
				if (m_Data[byte] & static_cast<uint8_t>(1u << offset))
					value |= 1u << i;
				m_Bit++;
			}
			return value;
		}

		bool Ok() const { return m_Ok; }

	private:
		const uint8_t* m_Data = nullptr;
		int m_Length = 0;
		int m_Bit = 0;
		bool m_Ok = true;
	};

	template<typename Stream>
	void Bits(Stream& stream, uint32_t& value, int bits)
	{
		if constexpr (Stream::Reading)
			value = stream.Take(bits);
		else
			stream.Put(value, bits);
	}

}
