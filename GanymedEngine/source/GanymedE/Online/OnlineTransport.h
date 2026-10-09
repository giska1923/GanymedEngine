#pragma once

// PRIVATE to Online/. The layer under Online's public API: single HTTP attempts over IXWebSocket,
// with no idea of sessions, tokens or retries. OnlineSession.cpp builds those on top of it.
// Online.cpp is its only implementation, and the only engine TU that includes IXWebSocket.

#include "GanymedE/Online/Online.h"

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
}
