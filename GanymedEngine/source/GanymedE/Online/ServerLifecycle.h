#pragma once

#include <cstdint>
#include <string>

namespace GanymedE {

	// What the fleet agent puts on the command line (server-lifecycle.md). The scene, the match
	// length and the outcome are the process's, not the agent's.
	struct ServerLaunch
	{
		std::string ServerId;
		std::string Agent;          // no trailing slash
		int Port = 0;
		std::string Advertise;      // host:port, the token's server_addr
		uint8_t PublicKey[32]{};
		std::string Outcome;        // "victory" or "defeat"
	};

	// The game server's half of the fleet: an HTTP client of its agent, and a UDP port for
	// HELLO. Health is its own thread, because a scene load on the main thread must not be
	// able to miss the 2 s interval. The door thread reads the port from the moment it is
	// bound. Neither thread is the frame loop.
	class ServerLifecycle
	{
	public:
		ServerLifecycle() = default;
		~ServerLifecycle();

		ServerLifecycle(const ServerLifecycle&) = delete;
		ServerLifecycle& operator=(const ServerLifecycle&) = delete;

		// Binds the port, then starts health and the door. False: nothing was left running.
		bool Start(const ServerLaunch& launch, std::string& error);

		// Blocks on /ready until an allocation arrives, acknowledges it, and opens admission.
		// False: already torn down. A refused acknowledgement posts /shutdown first.
		bool WaitForAllocation(std::string& error);

		// The scene could not be loaded after an allocation. Posts /shutdown, no result.
		void GiveUp();

		// Match over: stop admitting, report the outcome, post /shutdown, then tear down.
		void Finish();

		// Health was refused. Finish and GiveUp tear the process down on the calling thread.
		bool IsStopped() const;

		const std::string& MatchId() const { return m_MatchId; }

	private:
		void HealthLoop();
		void DoorLoop();
		std::string Answer(const std::string& datagram);
		bool Post(const std::string& path, const std::string& body, int transferTimeoutSeconds,
			int& status, std::string& responseBody, std::string& error);
		void ReportResult();
		void TearDown();
		void RequestStop();

		ServerLaunch m_Launch;
		std::string m_MatchId;

		// The rest of the state lives in the cpp, behind a pointer, so the header stays free of
		// the thread and socket types.
		struct State;
		State* m_State = nullptr;
	};

}
