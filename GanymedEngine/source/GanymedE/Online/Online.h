#pragma once

#include <algorithm>
#include <cctype>
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

		// Sent with the session's access token, signing in first if there is no session, and
		// recovered once on a 401 (refresh or sign in again, then one retry). False only for the
		// sign-in and refresh routes themselves.
		bool Authenticated = true;

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
		std::vector<std::pair<std::string, std::string>> Headers;

		// Empty when an HTTP response arrived, whatever its status. Otherwise the engine's own
		// wording for why none did ("cannot connect to http://127.0.0.1:8080", "timed out",
		// "not signed in: ...") - not the transport's text, which on Windows reports a refused
		// connect as "No error".
		std::string TransportError;

		bool Arrived() const { return TransportError.empty(); }
		bool IsSuccess() const { return Arrived() && Status >= 200 && Status < 300; }

		// A response header's value, by case-insensitive name (HTTP header names are), or null.
		const std::string* Header(const std::string& name) const
		{
			for (const auto& [key, value] : Headers)
			{
				if (key.size() == name.size() && std::equal(key.begin(), key.end(), name.begin(),
					[](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
					return &value;
			}
			return nullptr;
		}
	};

	// Runs on the main thread, from JobSystem::OnUpdate at the top of a frame - never on a network
	// thread and never inside a scene update. Not called at all for a cancelled request.
	using OnlineCompletion = std::function<void(const OnlineResponse&)>;

	// The engine's client to the backend (docs/engine/online.md): HTTP that never blocks a frame,
	// and the player's identity and session.
	//
	// It knows nothing of scenes, scripts or owners. Who a response is for, and whether they still
	// exist, is the caller's business; ScriptEngine is the caller that matters, and it cancels what
	// a script owned when the script goes away.
	//
	// Static facade with Init/Shutdown owned by Application, like AudioEngine and ScriptEngine.
	// Every entry point is safe before Init and after Shutdown: Send then fails at once, through
	// the same completion path a network failure takes.
	class Online
	{
	public:
		// Reads --backend=<url> (default http://127.0.0.1:8080) and --profile=<name> (default
		// "default"), loads or creates that profile's device ID, and starts signing in. Never
		// waits for the network: boot does not depend on the backend.
		static void Init();

		// Abandons every request in flight and joins the network threads. Completions for those
		// requests are never called.
		static void Shutdown();

		static bool IsInitialized();
		static const std::string& GetBackendUrl();

		// Never blocks. The completion runs on the main thread a frame or more later. An
		// authenticated request made with no session waits for a sign-in started on its behalf.
		static RequestId Send(OnlineRequest request, OnlineCompletion completion);

		// Guarantees the completion never runs, and stops the transport waiting: a request still
		// queued is never sent, and one in flight returns at the network thread's next
		// cancellation check (well under a second). It does not close the connection - the
		// server still finishes, and the socket stays open, idle, until that client's next request
		// replaces it or Shutdown. Safe for unknown, finished or already-cancelled ids.
		static void Cancel(RequestId id);

		// ---- Identity ------------------------------------------------------------------------

		enum class Status
		{
			Offline,     // no session: never signed in, sign-in failed, or no usable device ID
			SigningIn,   // a sign-in is in flight (at boot, or recovering a rejected session)
			SignedIn     // a session exists; a request may still find its access token expired
		};
		static Status GetStatus();

		// The --profile= in use, and the account and display name of the signed-in player. The
		// account and name are empty until a sign-in has succeeded (the name a moment later: it is
		// read from the profile after the session exists).
		static const std::string& GetProfile();
		static const std::string& GetAccountId();
		static const std::string& GetPlayerName();

		struct Stats
		{
			uint64_t Sent = 0;           // requests made through Send, including the engine's own profile
			                             // read after sign-in; not retries, not the auth calls themselves
			uint64_t Succeeded = 0;      // completion ran with a 2xx
			uint64_t Failed = 0;         // completion ran with anything else
			uint64_t Cancelled = 0;      // Cancel() reached a request still pending
			uint64_t Retried = 0;        // requests re-sent once after a 401 and a recovered session
			uint64_t SignIns = 0;        // sign-in calls made (boot, lazy, and after a rejected session)
			uint64_t Refreshes = 0;      // refresh calls made
			uint64_t DroppedLate = 0;    // a response came back for a cancelled request
			uint32_t InFlight = 0;       // requests not yet completed, including those waiting to sign in
		};
		static Stats GetStats();

		// A random (version 4) UUID, from the OS's CSPRNG: device IDs and Idempotency-Keys.
		static std::string NewUuid();
	};
}
