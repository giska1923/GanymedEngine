// No precompiled header. The engine PCH pulls in spdlog, which includes windows.h, and a
// UDP socket on Windows has to see winsock2.h first.

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
	#include <fcntl.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <unistd.h>
#endif

#include "GanymedE/Net/Socket.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace GanymedE::Net {

	namespace {

#if defined(_WIN32)
		using Handle = SOCKET;
		constexpr Handle kInvalid = INVALID_SOCKET;
		int LastError() { return WSAGetLastError(); }
		void CloseHandle(Handle handle) { closesocket(handle); }
		bool IsRefused(int error) { return error == WSAECONNRESET; }
		bool IsWouldBlock(int error) { return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT; }
#else
		using Handle = int;
		constexpr Handle kInvalid = -1;
		int LastError() { return errno; }
		void CloseHandle(Handle handle) { ::close(handle); }
		bool IsRefused(int) { return false; }
		bool IsWouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
#endif

		std::string ErrorText(const char* what, int error)
		{
			char buffer[128];
			std::snprintf(buffer, sizeof(buffer), "%s (%d)", what, error);
			return buffer;
		}

		sockaddr_in ToSock(const Address& address)
		{
			sockaddr_in out{};
			out.sin_family = AF_INET;
			out.sin_addr.s_addr = address.Addr;
			out.sin_port = htons(address.Port);
			return out;
		}

		bool SetNonBlocking(Handle handle, std::string& error)
		{
#if defined(_WIN32)
			u_long on = 1;
			if (ioctlsocket(handle, FIONBIO, &on) != 0)
			{
				error = ErrorText("ioctlsocket", LastError());
				return false;
			}
#else
			const int flags = fcntl(handle, F_GETFL, 0);
			if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0)
			{
				error = ErrorText("fcntl", LastError());
				return false;
			}
#endif
			return true;
		}

	}

	bool Address::IsLoopback() const
	{
		return (ntohl(Addr) & 0xFF000000u) == 0x7F000000u;
	}

	std::string Address::Text() const
	{
		char host[INET_ADDRSTRLEN] = {};
		in_addr addr{};
		addr.s_addr = Addr;
		inet_ntop(AF_INET, &addr, host, sizeof(host));
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "%s:%u", host, static_cast<unsigned>(Port));
		return buffer;
	}

	bool ParseAddress(const std::string& text, Address& out, std::string& error)
	{
		const size_t colon = text.rfind(':');
		if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
		{
			error = "'" + text + "' is not host:port";
			return false;
		}

		const std::string host = text.substr(0, colon);
		const std::string portText = text.substr(colon + 1);
		char* end = nullptr;
		const long port = std::strtol(portText.c_str(), &end, 10);
		if (end == portText.c_str() || *end != '\0' || port <= 0 || port > 65535)
		{
			error = "'" + text + "' is not host:port";
			return false;
		}

		in_addr addr{};
		if (inet_pton(AF_INET, host.c_str(), &addr) != 1)
		{
			error = "'" + host + "' is not an IPv4 address";
			return false;
		}

		out.Addr = addr.s_addr;
		out.Port = static_cast<uint16_t>(port);
		return true;
	}

	Address Loopback(int port)
	{
		Address address;
		address.Addr = htonl(0x7F000001u);
		address.Port = static_cast<uint16_t>(port);
		return address;
	}

	Address Any(int port)
	{
		Address address;
		address.Addr = htonl(INADDR_ANY);
		address.Port = static_cast<uint16_t>(port);
		return address;
	}

	Socket::~Socket()
	{
		Close();
	}

	Socket::Socket(Socket&& other) noexcept
		: m_Handle(other.m_Handle), m_Open(other.m_Open)
	{
		other.m_Handle = static_cast<uintptr_t>(kInvalid);
		other.m_Open = false;
	}

	Socket& Socket::operator=(Socket&& other) noexcept
	{
		if (this != &other)
		{
			Close();
			m_Handle = other.m_Handle;
			m_Open = other.m_Open;
			other.m_Handle = static_cast<uintptr_t>(kInvalid);
			other.m_Open = false;
		}
		return *this;
	}

	bool Socket::Open(const Address& bind, bool server, std::string& error)
	{
		if (bind.Port > 65535)
		{
			error = "port is out of range";
			return false;
		}

#if defined(_WIN32)
		// Counted, and this TU does not call WSACleanup. IXWebSocket's initNetSystem
		// holds a count of its own; a cleanup here would drop it and break HTTP.
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		{
			error = "WSAStartup failed";
			return false;
		}
#endif

		const Handle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (handle == kInvalid)
		{
			error = ErrorText("socket", LastError());
			return false;
		}

		if (!SetNonBlocking(handle, error))
		{
			CloseHandle(handle);
			return false;
		}

		if (server)
		{
#if defined(_WIN32)
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
			BOOL reportReset = FALSE;
			DWORD returned = 0;
			WSAIoctl(handle, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset),
				nullptr, 0, &returned, nullptr, nullptr);
#endif
		}

		const sockaddr_in address = ToSock(bind);
		if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			error = ErrorText("bind", LastError());
			CloseHandle(handle);
			return false;
		}

		m_Handle = static_cast<uintptr_t>(handle);
		m_Open = true;
		return true;
	}

	bool Socket::OpenClient(std::string& error)
	{
		return Open(Any(0), /*server=*/false, error);
	}

	bool Socket::OpenServer(const Address& bind, std::string& error)
	{
		if (bind.Port == 0)
		{
			error = "server port is 0";
			return false;
		}
		return Open(bind, /*server=*/true, error);
	}

	void Socket::Close()
	{
		if (!m_Open)
			return;
		m_Open = false;
		CloseHandle(static_cast<Handle>(m_Handle));
		m_Handle = static_cast<uintptr_t>(kInvalid);
	}

	bool Socket::Wait(int timeoutMs)
	{
		if (!m_Open)
			return false;

		fd_set read;
		FD_ZERO(&read);
		const Handle handle = static_cast<Handle>(m_Handle);
		FD_SET(handle, &read);
		timeval timeout{};
		timeout.tv_sec = timeoutMs / 1000;
		timeout.tv_usec = (timeoutMs % 1000) * 1000;
#if defined(_WIN32)
		const int ready = ::select(0, &read, nullptr, nullptr, &timeout);
#else
		const int ready = ::select(static_cast<int>(handle) + 1, &read, nullptr, nullptr, &timeout);
#endif
		return ready > 0;
	}

	Socket::Recv Socket::Receive(uint8_t* data, int capacity, int& outLength, Address& from, std::string& error)
	{
		outLength = 0;
		if (!m_Open)
			return Recv::Closed;

		sockaddr_in address{};
#if defined(_WIN32)
		int length = sizeof(address);
#else
		socklen_t length = sizeof(address);
#endif
		const int received = static_cast<int>(::recvfrom(static_cast<Handle>(m_Handle),
			reinterpret_cast<char*>(data), capacity, 0, reinterpret_cast<sockaddr*>(&address), &length));
		if (received >= 0)
		{
			outLength = received;
			from.Addr = address.sin_addr.s_addr;
			from.Port = ntohs(address.sin_port);
			return Recv::Packet;
		}

		if (!m_Open)
			return Recv::Closed;

		const int code = LastError();
		if (IsWouldBlock(code))
			return Recv::WouldBlock;
		if (IsRefused(code))
			return Recv::Refused;

		error = ErrorText("recvfrom", code);
		return Recv::Error;
	}

	bool Socket::Send(const Address& to, const uint8_t* data, int length)
	{
		if (!m_Open || to.Port == 0)
			return false;

		const sockaddr_in address = ToSock(to);
		const int sent = static_cast<int>(::sendto(static_cast<Handle>(m_Handle),
			reinterpret_cast<const char*>(data), length, 0,
			reinterpret_cast<const sockaddr*>(&address), sizeof(address)));
		return sent == length;
	}

}
