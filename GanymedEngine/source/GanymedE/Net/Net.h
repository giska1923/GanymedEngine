#pragma once

#include "GanymedE/Net/Socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GanymedE::Net {

	// `--net-sim=lat:50,jit:10,loss:10,dup:1,reorder:20`. Times are milliseconds.
	// `loss`, `dup` and `reorder` are percents. Empty means the link is left alone.
	// `reorder` holds that fraction of packets for an extra max(50 ms, lat), which is
	// long enough for a later packet to pass them.
	struct Sim
	{
		int LatencyMs = 0;
		int JitterMs = 0;
		int LossPercent = 0;
		int DuplicatePercent = 0;
		int ReorderPercent = 0;

		bool Enabled() const
		{
			return LatencyMs || JitterMs || LossPercent || DuplicatePercent || ReorderPercent;
		}
	};

	bool ParseSim(const std::string& text, Sim& out, std::string& error);

	// What `Net.GetStats()` reads. On a client this is its one session. On a server
	// it is the mean of the live sessions (bytes and resent are sums).
	struct Stats
	{
		bool Connected = false;
		int Peers = 0;
		float RttMs = 0.0f;
		float LossIn = 0.0f;
		float LossOut = 0.0f;
		float BytesInPerSec = 0.0f;
		float BytesOutPerSec = 0.0f;
		uint64_t Resent = 0;
		uint64_t DroppedSession = 0;
		uint64_t DroppedAddress = 0;
	};

	void SetSim(const Sim& sim);
	const Sim& GetSim();

	// The dedicated server's socket. The door thread is the caller's: PumpServer
	// waits up to `waitMs`, and CloseServer runs only after that thread has joined.
	bool Listen(const Address& bind, std::string& error);
	void SetTextHandler(std::function<std::string(const Address& from, const std::string& text)> handler);
	void NoteAdmitted(const std::string& account, const Address& from);
	void PumpServer(int waitMs);
	void CloseServer();

	// `--connect=`. Sends `HELLO dev:<name>` on a fresh socket, then the session.
	// `test` is empty, `reliable:<count>`, or `spoof`.
	bool ConnectDev(const std::string& hostPort, const std::string& name, const std::string& test, std::string& error);

	// JoinMatch, after WELCOME. The socket is the one every attempt was sent on.
	bool BeginClient(std::unique_ptr<Socket> socket, const std::string& hostPort, const std::string& account, std::string& error);

	void PumpClient();
	void ShutdownClient();

	Stats GetStats();

	// A snapshot the client received. The body is everything after the 21-byte header.
	// `At` is the local time of the receive, which is what the tick estimate measures from.
	struct SnapshotDatagram
	{
		std::vector<uint8_t> Bytes;
		float RttMs = 0.0f;
		std::chrono::steady_clock::time_point At{};
	};

	// Server, main thread. The door thread is the one in `recvfrom`; these copy what it
	// published and hand it a payload to send. The lock is not held across `select`.
	std::vector<std::string> ServerAccounts();
	bool SnapshotAcked(const std::string& account, uint16_t sequence);
	uint16_t SendSnapshot(const std::string& account, const uint8_t* data, int length);

	// Client, main thread. Empties the queue `PumpClient` filled earlier this frame.
	std::vector<SnapshotDatagram> DrainSnapshots();

	// Commands are the other unreliable kind. The client sends them; the server queues
	// the bodies on the door thread and the main thread drains them before the step.
	struct CommandDatagram
	{
		std::string Account;
		std::vector<uint8_t> Bytes;
	};

	void SendCommands(const uint8_t* data, int length);
	std::vector<CommandDatagram> DrainCommands();

}
