#pragma once

// PRIVATE to Online/. The layer under Online's public API: single HTTP attempts over IXWebSocket,
// with no idea of sessions, tokens or retries. OnlineSession.cpp builds those on top of it.
// Online.cpp is its only implementation, and the only engine TU that includes IXWebSocket.

#include "GanymedE/Online/Online.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace GanymedE::OnlineTransport {

	using AttemptId = uint64_t;

	// Starts the network threads. `baseUrl` has no trailing slash.
	void Init(const std::string& baseUrl);

	// Joins the network threads. Attempts still running are abandoned: their completions never run.
	// Returns how many were abandoned.
	size_t Shutdown();

	bool IsInitialized();

	// One HTTP attempt. `bearer`, when not empty, is sent as `Authorization: Bearer <bearer>`. The
	// completion runs on the main thread (from JobSystem::OnUpdate), never inline - including for an
	// attempt that could not be queued, which completes with a TransportError. Returns 0 then.
	AttemptId Start(const OnlineRequest& request, const std::string& bearer,
		std::function<void(const OnlineResponse&)> completion);

	// The completion will never run. A queued attempt is never sent; one in flight stops at the
	// network thread's next cancellation check. The connection is not closed (see online.md).
	void Abort(AttemptId id);

	// Responses that came back for aborted attempts, and attempts not yet completed.
	uint64_t DroppedLate();
	uint32_t InFlight();

	// ---- The push socket (one at a time) ---------------------------------------------------

	// Every event runs on the main thread, from JobSystem::OnUpdate, and only for the socket that
	// is current: events still queued from a socket that has since been closed or replaced are
	// dropped. After a close or a failure the socket is finished; opening again is the caller's
	// decision (the session reconnects by close code).
	struct SocketEvents
	{
		std::function<void()> OnOpen;
		std::function<void(const std::string& text)> OnText;
		std::function<void(int code, const std::string& reason, bool remote)> OnClose;
		// The connect or the upgrade failed. httpStatus is the upgrade's status (401 for a refused
		// token), or 0 when no HTTP response arrived. The response body is not available.
		std::function<void(int httpStatus, const std::string& reason)> OnFailed;
	};

	// Opens a WebSocket to the base URL (http -> ws) + `path`, sending `bearer` at the upgrade.
	// Closes any socket already open first.
	void OpenSocket(const std::string& path, const std::string& bearer, SocketEvents events);

	// Closes the socket, if any; none of its events run after this returns.
	void CloseSocket();

	// ---- One UDP request and its reply (joining a game server) -----------------------------

	using ExchangeId = uint64_t;

	// Sends `payload` as one datagram to `address` ("host:port") and waits up to `timeout` for one
	// datagram back. `completion(ok, text)` gets the reply, or why there is none ("no answer",
	// "nothing is listening"). It runs on the main thread, from PollExchanges, never inline.
	ExchangeId Exchange(const std::string& address, const std::string& payload, std::chrono::milliseconds timeout,
		std::function<void(bool ok, const std::string& text)> completion);

	// The completion will never run.
	void AbortExchange(ExchangeId id);

	// Main thread, once per frame (Online::OnUpdate): one non-blocking receive per pending exchange.
	// No thread waits on a datagram; a reply is noticed within a frame of arriving.
	void PollExchanges();
}
