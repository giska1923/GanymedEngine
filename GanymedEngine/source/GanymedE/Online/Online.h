#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace GanymedE {

	using RequestId = uint64_t;

	// One HTTP call to the backend. Path is relative to the backend's base URL ("/v1/me"); the base
	// comes from --backend=, never from a caller, so nothing can aim a request anywhere else.
	struct OnlineRequest
	{
		std::string Method = "GET";
		std::string Path;
		std::string Body;                                      // sent as application/json when set
		std::vector<std::pair<std::string, std::string>> Headers;

		// Whole seconds, because that is what the transport takes. The transfer timeout covers
		// everything after the connect; a long-poll (O5b's `ready`) overrides it.
		int ConnectTimeoutSeconds = 5;
		int TransferTimeoutSeconds = 10;
	};

	struct OnlineResponse
	{
		int Status = 0;                    // 0 when no HTTP response arrived at all
		std::string Body;
		std::string ContentType;

		// Empty when an HTTP response arrived, whatever its status. Otherwise the engine's own
		// wording for why none did ("cannot connect to http://127.0.0.1:8080", "timed out") -
		// not the transport's text, which on Windows reports a refused connect as "No error".
		std::string TransportError;

		bool Arrived() const { return TransportError.empty(); }
		bool IsSuccess() const { return Arrived() && Status >= 200 && Status < 300; }
	};

	// Runs on the main thread, from JobSystem::OnUpdate at the top of a frame - never on a network
	// thread and never inside a scene update. Not called at all for a cancelled request.
	using OnlineCompletion = std::function<void(const OnlineResponse&)>;

	// The engine's HTTP client to the backend (docs/engine/online.md).
	//
	// Transport only: it knows nothing of scenes, scripts or owners. Who a response is for, and
	// whether they still exist, is the caller's business; ScriptEngine is the caller that matters,
	// and it cancels what a script owned when the script goes away.
	//
	// Static facade with Init/Shutdown owned by Application, like AudioEngine and ScriptEngine.
	// Every entry point is safe before Init and after Shutdown: Send then fails at once, through
	// the same completion path a network failure takes.
	class Online
	{
	public:
		// Reads --backend=<url> from the command line (default http://127.0.0.1:8080).
		static void Init();

		// Aborts every request in flight and joins the network threads. Completions for those
		// requests are never called.
		static void Shutdown();

		static bool IsInitialized();
		static const std::string& GetBackendUrl();

		// Never blocks. The completion runs on the main thread a frame or more later. Returns 0
		// only when the request could not be queued at all, and then the completion has already
		// been scheduled with a TransportError.
		static RequestId Send(OnlineRequest request, OnlineCompletion completion);

		// Guarantees the completion never runs, and stops the transport waiting: a request still
		// queued is never sent, and one in flight returns at the network thread's next
		// cancellation check (well under a second). It does not close the connection - the
		// server still finishes, and the socket stays open, idle, until that client's next request
		// replaces it or Shutdown. Safe for unknown, finished or already-cancelled ids.
		static void Cancel(RequestId id);

		struct Stats
		{
			uint64_t Sent = 0;
			uint64_t Succeeded = 0;      // completion ran with a 2xx
			uint64_t Failed = 0;         // completion ran with anything else
			uint64_t Cancelled = 0;      // Cancel() reached a request still pending
			uint64_t DroppedLate = 0;    // a response came back for a cancelled request
			uint32_t InFlight = 0;
		};
		static Stats GetStats();
	};
}
