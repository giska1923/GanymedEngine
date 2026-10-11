#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace GanymedE {

	// One datagram's sender. Opaque so this header does not include a socket API: Online.cpp is
	// the only TU allowed to include IXWebSocket, and that header's UDP socket cannot bind.
	struct GamePeer
	{
		alignas(8) unsigned char Address[128]{};
		int Length = 0;
		std::string Text;   // "ip:port", for the log. Never a token.
	};

	// The dedicated server's UDP port. N1 only: N2's Net/Socket replaces it. Blocking receive
	// with a short timeout, so the calling thread can notice a stop without a second socket.
	class GamePort
	{
	public:
		GamePort() = default;
		~GamePort();

		GamePort(const GamePort&) = delete;
		GamePort& operator=(const GamePort&) = delete;

		bool Bind(int port, std::string& error);
		void Close();

		enum class Recv
		{
			Packet,
			Timeout,   // or one ignored ICMP reset: the socket is fine
			Closed,
			Error
		};

		Recv Receive(std::string& text, GamePeer& from, std::string& error);
		bool Send(const GamePeer& to, const std::string& text);

	private:
		uintptr_t m_Socket = ~uintptr_t{ 0 };
		std::atomic<bool> m_Open{ false };
	};

}
