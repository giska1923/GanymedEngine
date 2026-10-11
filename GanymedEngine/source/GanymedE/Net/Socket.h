#pragma once

#include <cstdint>
#include <string>

namespace GanymedE::Net {

	// IPv4 only. N2's socket does not grow a second address family until something
	// on the other end of a match speaks it.
	struct Address
	{
		uint32_t Addr = 0;   // network order
		uint16_t Port = 0;   // host order

		bool operator==(const Address& other) const { return Addr == other.Addr && Port == other.Port; }
		bool operator!=(const Address& other) const { return !(*this == other); }

		bool IsLoopback() const;
		std::string Text() const;
	};

	bool ParseAddress(const std::string& text, Address& out, std::string& error);
	Address Loopback(int port);
	Address Any(int port);

	// One UDP socket. Non-blocking sendto/recvfrom. The header stays free of the
	// platform socket API: Online.cpp already includes IXWebSocket, and that
	// header's UDP socket cannot bind or report the sender.
	//
	// A server socket turns off Windows' ICMP-port-unreachable error. One dead
	// client would otherwise fail the next receive and stop admission for everyone
	// else. A client socket leaves it on, so JoinMatch can still tell "nothing is
	// listening" from a second of silence.
	class Socket
	{
	public:
		Socket() = default;
		~Socket();

		Socket(const Socket&) = delete;
		Socket& operator=(const Socket&) = delete;
		Socket(Socket&& other) noexcept;
		Socket& operator=(Socket&& other) noexcept;

		bool OpenClient(std::string& error);
		bool OpenServer(const Address& bind, std::string& error);
		void Close();
		bool IsOpen() const { return m_Open; }

		// True when a receive would not block. `timeoutMs` of 0 polls.
		bool Wait(int timeoutMs);

		enum class Recv
		{
			Packet,
			WouldBlock,
			Refused,   // ICMP port unreachable, client sockets only
			Closed,
			Error
		};

		Recv Receive(uint8_t* data, int capacity, int& outLength, Address& from, std::string& error);
		bool Send(const Address& to, const uint8_t* data, int length);

	private:
		bool Open(const Address& bind, bool server, std::string& error);

		uintptr_t m_Handle = ~uintptr_t{ 0 };
		bool m_Open = false;
	};

}
