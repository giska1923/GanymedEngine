// No precompiled header. The engine PCH pulls in spdlog, which includes windows.h, and a UDP
// socket on Windows has to see winsock2.h first.

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <mstcpip.h>
	#pragma comment(lib, "ws2_32.lib")
#else
	#include <arpa/inet.h>
	#include <cerrno>
	#include <cstring>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <sys/time.h>
	#include <unistd.h>
#endif

#include "GanymedE/Online/GamePort.h"

#include <cstring>
#include <cstdio>

namespace GanymedE {

	namespace {

		constexpr int kTimeoutMs = 200;
		constexpr int kMaxDatagram = 2048;

#if defined(_WIN32)
		using SocketHandle = SOCKET;
		constexpr SocketHandle kInvalid = INVALID_SOCKET;

		int LastError() { return WSAGetLastError(); }

		bool IsTimeout(int error)
		{
			// A send to a dead client port makes the next receive fail with WSAECONNRESET.
			// The socket is still good. N2 disables this with SIO_UDP_CONNRESET as well.
			return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK || error == WSAECONNRESET;
		}

		void CloseSocket(SocketHandle socket) { closesocket(socket); }
#else
		using SocketHandle = int;
		constexpr SocketHandle kInvalid = -1;

		int LastError() { return errno; }

		bool IsTimeout(int error)
		{
			return error == EAGAIN || error == EWOULDBLOCK || error == ETIMEDOUT || error == EINTR;
		}

		void CloseSocket(SocketHandle socket) { ::close(socket); }
#endif

		std::string ErrorText(const char* what, int error)
		{
			char buffer[128];
			std::snprintf(buffer, sizeof(buffer), "%s (%d)", what, error);
			return buffer;
		}

		void FormatPeer(GamePeer& peer, const sockaddr_in& address)
		{
			char host[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%s:%u", host, static_cast<unsigned>(ntohs(address.sin_port)));
			peer.Text = buffer;
		}

	}

	GamePort::~GamePort()
	{
		Close();
	}

	bool GamePort::Bind(int port, std::string& error)
	{
		if (port <= 0 || port > 65535)
		{
			error = "game port is out of range";
			return false;
		}

#if defined(_WIN32)
		// IXWebSocket's initNetSystem has usually already done this. WSAStartup is counted, and
		// this TU does not call WSACleanup: a cleanup here would drop the count IXWebSocket still
		// holds and break the agent's HTTP client.
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		{
			error = "WSAStartup failed";
			return false;
		}
#endif

		const SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (socket == kInvalid)
		{
			error = ErrorText("socket", LastError());
			return false;
		}

#if defined(_WIN32)
		// Not in every SDK's mstcpip.h. The value is stable: vendor ioctl 12, documented since XP.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
		// ICMP port-unreachable must not fail the next receive. Ignoring the error is the
		// fallback for a platform that refuses the ioctl.
		BOOL reportReset = FALSE;
		DWORD returned = 0;
		WSAIoctl(socket, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset),
			nullptr, 0, &returned, nullptr, nullptr);

		DWORD timeout = kTimeoutMs;
		setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = kTimeoutMs * 1000;
		setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		address.sin_port = htons(static_cast<uint16_t>(port));

		if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			error = ErrorText("bind", LastError());
			CloseSocket(socket);
			return false;
		}

		m_Socket = static_cast<uintptr_t>(socket);
		m_Open = true;
		return true;
	}

	void GamePort::Close()
	{
		if (!m_Open)
			return;

		m_Open = false;
		CloseSocket(static_cast<SocketHandle>(m_Socket));
		m_Socket = static_cast<uintptr_t>(kInvalid);
	}

	GamePort::Recv GamePort::Receive(std::string& text, GamePeer& from, std::string& error)
	{
		if (!m_Open)
			return Recv::Closed;

		char buffer[kMaxDatagram];
		sockaddr_in address{};
#if defined(_WIN32)
		int length = sizeof(address);
#else
		socklen_t length = sizeof(address);
#endif
		const int received = static_cast<int>(recvfrom(static_cast<SocketHandle>(m_Socket), buffer,
			sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&address), &length));

		if (received >= 0)
		{
			text.assign(buffer, buffer + received);
			from = {};
			if (length > 0 && static_cast<size_t>(length) <= sizeof(from.Address))
			{
				std::memcpy(from.Address, &address, static_cast<size_t>(length));
				from.Length = static_cast<int>(length);
				if (address.sin_family == AF_INET)
					FormatPeer(from, address);
			}
			return Recv::Packet;
		}

		if (!m_Open)
			return Recv::Closed;

		const int errorCode = LastError();
		if (IsTimeout(errorCode))
			return Recv::Timeout;

		error = ErrorText("recvfrom", errorCode);
		return Recv::Error;
	}

	bool GamePort::Send(const GamePeer& to, const std::string& text)
	{
		if (!m_Open || to.Length <= 0)
			return false;

		const int sent = static_cast<int>(sendto(static_cast<SocketHandle>(m_Socket), text.data(),
			static_cast<int>(text.size()), 0, reinterpret_cast<const sockaddr*>(to.Address),
#if defined(_WIN32)
			to.Length));
#else
			static_cast<socklen_t>(to.Length)));
#endif
		return sent == static_cast<int>(text.size());
	}

}
