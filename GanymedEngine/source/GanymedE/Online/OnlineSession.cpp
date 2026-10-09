#include "gepch.h"
#include "GanymedE/Online/Online.h"

#include "GanymedE/Core/JobSystem.h"
#include "GanymedE/Online/Json.h"
#include "GanymedE/Online/OnlineTransport.h"
#include "GanymedE/Utils/PlatformUtils.h"
#include "GanymedE/main/Application.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>

// Online's public API, over OnlineTransport: the player's identity (a device ID per --profile),
// the session (access and refresh tokens, in memory only), and the one recovery every
// authenticated request gets on a 401. The backend's rules are in its openapi.yaml (api-v0.1 on):
// refresh tokens rotate, and presenting one twice revokes the whole session - so this file never
// runs two refreshes at once, and never presents a refresh token twice.

namespace GanymedE {

	namespace {

		constexpr const char* kTokenExpired = "urn:ganymed:problem:token-expired";

		// After a failed sign-in, requests that need one fail at once for this long instead of each
		// trying again. Without it a script calling every frame against a backend that is down
		// would start a sign-in, and log a warning, sixty times a second.
		constexpr std::chrono::seconds kSignInBackoff{ 5 };

		struct LogicalRequest
		{
			OnlineRequest Request;
			OnlineCompletion Completion;
			int Attempts = 0;                          // 1 after the first send, 2 after the retry
			uint64_t Generation = 0;                   // the session the last attempt's token came from
			OnlineTransport::AttemptId Attempt = 0;    // 0 while waiting for a session
		};

		struct SessionData
		{
			std::string BackendUrl;
			std::string Profile;
			std::string DeviceId;          // the credential: never logged
			std::string IdentityError;     // why there is no usable device ID; sign-in is never tried

			Online::Status Status = Online::Status::Offline;
			std::string AccountId;
			std::string AccessToken;       // never logged
			std::string RefreshToken;      // never logged; single use
			std::string PlayerName;

			// Bumped by every new access token. A request remembers the generation it was sent
			// with, so a 401 for a token that has already been replaced is retried with the new one
			// rather than starting a second recovery.
			uint64_t Generation = 0;

			// A sign-in or refresh is in flight. There is never more than one.
			bool Recovering = false;

			bool SignInFailed = false;
			std::chrono::steady_clock::time_point LastSignInFailure{};

			std::unordered_map<RequestId, LogicalRequest> Requests;
			std::vector<RequestId> Waiting;   // authenticated requests waiting for a session
			RequestId NextId = 1;
			Online::Stats Stats;
		};

		SessionData* s_Data = nullptr;

		void Dispatch(RequestId id);
		void BeginSignIn();

		// ---- Command line and identity ---------------------------------------------------------

		std::string ReadFlag(const char* flag, std::string fallback)
		{
			const ApplicationCommandLineArgs& args = Application::GetCommandLineArgs();
			const std::size_t length = std::strlen(flag);
			for (int i = 1; i < args.Count; i++)
			{
				if (args.Args[i] && std::strncmp(args.Args[i], flag, length) == 0)
					fallback = args.Args[i] + length;   // last one wins, as --renderer=
			}
			return fallback;
		}

		// A profile names a directory, so it is held to characters that are safe in a path on
		// every OS. A bad one is an error, not a quiet fallback to "default": two test instances
		// that silently share a profile are the same player, and that bug is hard to see.
		bool IsValidProfileName(const std::string& name)
		{
			if (name.empty() || name.size() > 32)
				return false;
			for (const char c : name)
			{
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
					|| c == '-' || c == '_';
				if (!ok)
					return false;
			}
			return true;
		}

		// profiles/<profile>/device_id under the user data directory, created on first use.
		std::string LoadOrCreateDeviceId(const std::string& profile, std::string& error)
		{
			const std::filesystem::path root = UserData::GetDirectory();
			if (root.empty())
			{
				error = "the OS reports no per-user data directory to keep a device ID in";
				return {};
			}

			const std::filesystem::path directory = root / "profiles" / profile;
			const std::filesystem::path file = directory / "device_id";

			std::error_code ec;
			if (std::filesystem::exists(file, ec))
			{
				std::ifstream in(file, std::ios::binary);
				std::string id((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back())))
					id.pop_back();
				if (id.size() >= 32 && id.size() <= 128)
					return id;

				// Never overwritten: it may be the only copy of a real player's credential.
				error = file.string() + " does not hold a usable device ID; it was left untouched";
				return {};
			}

			std::filesystem::create_directories(directory, ec);
			if (ec)
			{
				error = "cannot create " + directory.string() + ": " + ec.message();
				return {};
			}

			// Written beside, then renamed into place, so a crash mid-write leaves no half an ID
			// that the next run would refuse.
			const std::string id = Online::NewUuid();
			const std::filesystem::path temporary = directory / "device_id.tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				out << id << '\n';
				if (!out)
				{
					error = "cannot write " + temporary.string();
					return {};
				}
			}
			std::filesystem::rename(temporary, file, ec);
			if (ec)
			{
				error = "cannot create " + file.string() + ": " + ec.message();
				return {};
			}

			// The device ID is the account's only credential. Owner-only where the OS has the bits;
			// on Windows the directory is already per-user (%LOCALAPPDATA%).
			std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
				std::filesystem::perm_options::replace, ec);

			GE_CORE_INFO("Online: created a device ID for profile '{0}' in {1}", profile, directory.string());
			return id;
		}

		// ---- Session ----------------------------------------------------------------------------

		std::string ProblemType(const OnlineResponse& response)
		{
			if (response.ContentType.find("problem+json") == std::string::npos)
				return {};
			std::optional<JsonValue> problem = ParseJson(response.Body);
			const JsonValue* type = problem ? problem->Find("type") : nullptr;
			return (type && type->Kind == JsonValue::Type::String) ? type->String : std::string{};
		}

		std::string DescribeFailure(const OnlineResponse& response)
		{
			if (!response.Arrived())
				return response.TransportError;
			const std::string type = ProblemType(response);
			return type.empty() ? "HTTP " + std::to_string(response.Status) : type;
		}

		const std::string* StringMember(const JsonValue& object, const char* key)
		{
			const JsonValue* value = object.Find(key);
			return (value && value->Kind == JsonValue::Type::String && !value->String.empty()) ? &value->String : nullptr;
		}

		// Takes a Session body (sign-in or refresh). The refresh token replaces the old one at
		// once: the old one is spent the moment the backend issued this.
		bool AdoptSession(const OnlineResponse& response, std::string& error)
		{
			std::optional<JsonValue> session = ParseJson(response.Body, &error);
			if (!session)
				return false;

			const std::string* account = StringMember(*session, "account_id");
			const std::string* access = StringMember(*session, "access_token");
			const std::string* refresh = StringMember(*session, "refresh_token");
			if (!account || !access || !refresh)
			{
				error = "the session is missing account_id, access_token or refresh_token";
				return false;
			}

			s_Data->AccountId = *account;
			s_Data->AccessToken = *access;
			s_Data->RefreshToken = *refresh;
			s_Data->Generation++;
			s_Data->Status = Online::Status::SignedIn;
			s_Data->Recovering = false;
			return true;
		}

		// ---- Requests ---------------------------------------------------------------------------

		void Complete(RequestId id, const OnlineResponse& response)
		{
			auto it = s_Data->Requests.find(id);
			if (it == s_Data->Requests.end())
				return;

			OnlineCompletion completion = std::move(it->second.Completion);
			s_Data->Requests.erase(it);
			if (response.IsSuccess())
				s_Data->Stats.Succeeded++;
			else
				s_Data->Stats.Failed++;
			completion(response);
		}

		// Fails every request waiting for a session, with `reason` as its TransportError.
		void FailWaiting(const std::string& reason)
		{
			std::vector<RequestId> waiting;
			waiting.swap(s_Data->Waiting);

			OnlineResponse response;
			response.TransportError = reason;
			for (RequestId id : waiting)
				Complete(id, response);
		}

		void ResumeWaiting()
		{
			std::vector<RequestId> waiting;
			waiting.swap(s_Data->Waiting);
			for (RequestId id : waiting)
			{
				if (s_Data->Requests.count(id))
					Dispatch(id);
			}
		}

		void OnAttemptDone(RequestId id, const OnlineResponse& response)
		{
			if (!s_Data)
				return;

			auto it = s_Data->Requests.find(id);
			if (it == s_Data->Requests.end())
				return;

			LogicalRequest& request = it->second;
			request.Attempt = 0;

			const bool rejected = request.Request.Authenticated && response.Arrived() && response.Status == 401;
			if (!rejected || request.Attempts >= 2)
			{
				Complete(id, response);
				return;
			}

			s_Data->Stats.Retried++;

			// Sent with a token that has since been replaced (another request's 401 already
			// recovered the session): retry with the current one, no second recovery.
			if (request.Generation < s_Data->Generation)
			{
				Dispatch(id);
				return;
			}

			s_Data->Waiting.push_back(id);
			if (s_Data->Recovering)
				return;   // another request's 401 started the recovery this one now waits for

			// The contract: token-expired means refresh; anything else (unauthorized) means the
			// token is no good at all, so sign in again.
			const bool expired = ProblemType(response) == kTokenExpired;
			if (expired && !s_Data->RefreshToken.empty())
			{
				GE_CORE_INFO("Online: access token expired ({0} {1}); refreshing, then retrying once",
					request.Request.Method, request.Request.Path);

				s_Data->Recovering = true;
				s_Data->Stats.Refreshes++;

				// Consumed by this call whatever happens next, so it is dropped now. If the network
				// fails mid-call the backend may have spent it anyway, and presenting it again would
				// count as reuse and revoke the session; the next recovery signs in instead.
				JsonValue body = JsonValue::MakeObject();
				body.Members.emplace_back("refresh_token", JsonValue::MakeString(std::move(s_Data->RefreshToken)));
				s_Data->RefreshToken.clear();

				OnlineRequest refresh;
				refresh.Method = "POST";
				refresh.Path = "/v1/auth/refresh";
				refresh.Authenticated = false;
				WriteJson(body, refresh.Body);

				OnlineTransport::Start(refresh, {}, [](const OnlineResponse& result)
				{
					if (!s_Data)
						return;

					std::string error;
					if (result.IsSuccess() && AdoptSession(result, error))
					{
						GE_CORE_INFO("Online: session refreshed");
						ResumeWaiting();
						return;
					}

					s_Data->Recovering = false;
					if (result.Arrived())
					{
						// invalid-refresh-token, whatever the reason: the remedy is to sign in.
						GE_CORE_INFO("Online: refresh refused ({0}); signing in again", DescribeFailure(result));
						BeginSignIn();
						return;
					}

					GE_CORE_WARN("Online: refresh failed ({0})", result.TransportError);
					FailWaiting("could not refresh the session: " + result.TransportError);
				});
				return;
			}

			GE_CORE_INFO("Online: the session was not accepted ({0} {1}: {2}); signing in again, then retrying once",
				request.Request.Method, request.Request.Path, DescribeFailure(response));
			BeginSignIn();
		}

		void Launch(RequestId id)
		{
			LogicalRequest& request = s_Data->Requests.at(id);
			request.Attempts++;
			request.Generation = s_Data->Generation;
			const std::string& bearer = request.Request.Authenticated ? s_Data->AccessToken : std::string{};
			request.Attempt = OnlineTransport::Start(request.Request, bearer,
				[id](const OnlineResponse& response) { OnAttemptDone(id, response); });
		}

		// Sends now, or parks the request until there is a session to send it with.
		void Dispatch(RequestId id)
		{
			const LogicalRequest& request = s_Data->Requests.at(id);
			const bool ready = s_Data->Status == Online::Status::SignedIn && !s_Data->Recovering;
			if (!request.Request.Authenticated || ready)
			{
				Launch(id);
				return;
			}

			s_Data->Waiting.push_back(id);
			if (!s_Data->Recovering)
				BeginSignIn();
		}

		void FetchPlayerName()
		{
			OnlineRequest request;
			request.Path = "/v1/me/profile";
			Online::Send(std::move(request), [](const OnlineResponse& response)
			{
				if (!s_Data || !response.IsSuccess())
					return;   // the name is a nicety; a failure here changes nothing else
				std::optional<JsonValue> profile = ParseJson(response.Body);
				if (const std::string* name = profile ? StringMember(*profile, "display_name") : nullptr)
				{
					s_Data->PlayerName = *name;
					GE_CORE_INFO("Online: playing as '{0}'", s_Data->PlayerName);
				}
			});
		}

		void BeginSignIn()
		{
			if (!s_Data->IdentityError.empty())
			{
				FailWaiting("not signed in: " + s_Data->IdentityError);
				return;
			}

			if (s_Data->SignInFailed && std::chrono::steady_clock::now() - s_Data->LastSignInFailure < kSignInBackoff)
			{
				FailWaiting("not signed in: offline (the last sign-in failed moments ago)");
				return;
			}

			s_Data->Recovering = true;
			s_Data->Status = Online::Status::SigningIn;
			s_Data->Stats.SignIns++;

			JsonValue body = JsonValue::MakeObject();
			body.Members.emplace_back("device_id", JsonValue::MakeString(s_Data->DeviceId));

			OnlineRequest signIn;
			signIn.Method = "POST";
			signIn.Path = "/v1/auth/device";
			signIn.Authenticated = false;
			WriteJson(body, signIn.Body);

			OnlineTransport::Start(signIn, {}, [](const OnlineResponse& result)
			{
				if (!s_Data)
					return;

				std::string error;
				if (result.IsSuccess() && AdoptSession(result, error))
				{
					s_Data->SignInFailed = false;
					GE_CORE_INFO("Online: signed in as account {0} (profile '{1}')", s_Data->AccountId, s_Data->Profile);
					FetchPlayerName();
					ResumeWaiting();
					return;
				}

				const std::string reason = !result.IsSuccess() ? DescribeFailure(result) : "unreadable session: " + error;
				GE_CORE_WARN("Online: sign-in failed ({0}); playing offline", reason);

				s_Data->Status = Online::Status::Offline;
				s_Data->Recovering = false;
				s_Data->AccountId.clear();
				s_Data->AccessToken.clear();
				s_Data->RefreshToken.clear();
				s_Data->PlayerName.clear();
				s_Data->SignInFailed = true;
				s_Data->LastSignInFailure = std::chrono::steady_clock::now();
				FailWaiting("not signed in: " + reason);
			});
		}
	}

	void Online::Init()
	{
		GE_CORE_ASSERT(!s_Data, "Online::Init called twice");
		s_Data = new SessionData();

		// 127.0.0.1, not localhost: on Windows a refused connect to localhost costs ~4 s against
		// ~2 s, because both ::1 and 127.0.0.1 are tried (measured in ONLINE.md, O0).
		s_Data->BackendUrl = ReadFlag("--backend=", "http://127.0.0.1:8080");
		while (!s_Data->BackendUrl.empty() && s_Data->BackendUrl.back() == '/')
			s_Data->BackendUrl.pop_back();
		s_Data->Profile = ReadFlag("--profile=", "default");

		OnlineTransport::Init(s_Data->BackendUrl);

		if (!IsValidProfileName(s_Data->Profile))
			s_Data->IdentityError = "--profile='" + s_Data->Profile + "' is not a profile name (1-32 of A-Z a-z 0-9 - _)";
		else
			s_Data->DeviceId = LoadOrCreateDeviceId(s_Data->Profile, s_Data->IdentityError);

		GE_CORE_INFO("Online initialised (backend {0}, profile '{1}')", s_Data->BackendUrl, s_Data->Profile);
		if (!s_Data->IdentityError.empty())
		{
			GE_CORE_ERROR("Online: {0}; staying offline", s_Data->IdentityError);
			return;
		}

		// Started, not awaited: boot never depends on the backend.
		BeginSignIn();
	}

	void Online::Shutdown()
	{
		if (!s_Data)
			return;

		const Stats stats = GetStats();   // before the transport goes: it holds the dropped-late count
		const size_t abandoned = OnlineTransport::Shutdown();
		GE_CORE_INFO("Online shut down: {0} sent, {1} succeeded, {2} failed, {3} cancelled, {4} retried, "
			"{5} sign-ins, {6} refreshes, {7} dropped late, {8} abandoned in flight", stats.Sent, stats.Succeeded,
			stats.Failed, stats.Cancelled, stats.Retried, stats.SignIns, stats.Refreshes, stats.DroppedLate, abandoned);

		delete s_Data;
		s_Data = nullptr;
	}

	bool Online::IsInitialized()
	{
		return s_Data != nullptr;
	}

	const std::string& Online::GetBackendUrl()
	{
		static const std::string s_None;
		return s_Data ? s_Data->BackendUrl : s_None;
	}

	RequestId Online::Send(OnlineRequest request, OnlineCompletion completion)
	{
		GE_CORE_ASSERT(JobSystem::IsMainThread(), "Online::Send is main-thread only");

		if (!s_Data)
		{
			OnlineTransport::Start(request, {}, std::move(completion));   // fails it, uninitialised
			return 0;
		}

		const RequestId id = s_Data->NextId++;
		LogicalRequest logical;
		logical.Request = std::move(request);
		logical.Completion = std::move(completion);
		s_Data->Requests.emplace(id, std::move(logical));
		s_Data->Stats.Sent++;
		Dispatch(id);
		return id;
	}

	void Online::Cancel(RequestId id)
	{
		if (!s_Data)
			return;

		auto it = s_Data->Requests.find(id);
		if (it == s_Data->Requests.end())
			return;

		if (it->second.Attempt)
			OnlineTransport::Abort(it->second.Attempt);
		s_Data->Waiting.erase(std::remove(s_Data->Waiting.begin(), s_Data->Waiting.end(), id), s_Data->Waiting.end());
		s_Data->Requests.erase(it);
		s_Data->Stats.Cancelled++;
	}

	Online::Status Online::GetStatus()
	{
		return s_Data ? s_Data->Status : Status::Offline;
	}

	const std::string& Online::GetProfile()
	{
		static const std::string s_None;
		return s_Data ? s_Data->Profile : s_None;
	}

	const std::string& Online::GetAccountId()
	{
		static const std::string s_None;
		return s_Data ? s_Data->AccountId : s_None;
	}

	const std::string& Online::GetPlayerName()
	{
		static const std::string s_None;
		return s_Data ? s_Data->PlayerName : s_None;
	}

	// 122 random bits, which is what the backend asks of a device ID ("high-entropy random data")
	// and of an Idempotency-Key (unique per logical request). std::random_device is the OS's CSPRNG
	// on both platforms that build (RtlGenRandom on MSVC, /dev/urandom or RDRAND in libstdc++).
	std::string Online::NewUuid()
	{
		std::random_device random;
		uint8_t bytes[16];
		for (size_t i = 0; i < sizeof(bytes); i += 4)
		{
			const uint32_t value = random();
			std::memcpy(bytes + i, &value, 4);
		}
		bytes[6] = (bytes[6] & 0x0F) | 0x40;   // version 4
		bytes[8] = (bytes[8] & 0x3F) | 0x80;   // RFC 9562 variant

		char text[37];
		std::snprintf(text, sizeof(text),
			"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
			bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
		return text;
	}

	Online::Stats Online::GetStats()
	{
		if (!s_Data)
			return {};
		Stats stats = s_Data->Stats;
		stats.DroppedLate = OnlineTransport::DroppedLate();
		stats.InFlight = static_cast<uint32_t>(s_Data->Requests.size());
		return stats;
	}
}
