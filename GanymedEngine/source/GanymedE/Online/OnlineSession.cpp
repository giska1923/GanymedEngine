#include "gepch.h"
#include "GanymedE/Online/Online.h"

#include "GanymedE/Core/JobSystem.h"
#include "GanymedE/Online/Json.h"
#include "GanymedE/Online/OnlineTransport.h"
#include "GanymedE/Utils/PlatformUtils.h"
#include "GanymedE/main/Application.h"

#include <cctype>
#include <chrono>
#include <cmath>
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

			// JoinMatch operations: one id for the whole ticket-read / HELLO / retry sequence, from the
			// same id space as requests, so Cancel works on either.
			struct JoinOperation
			{
				OnlineCompletion Completion;
				int Attempt = 0;
				RequestId TicketRead = 0;                    // the GET in flight, or 0
				OnlineTransport::ExchangeId Hello = 0;       // the datagram awaiting a reply, or 0
			};
			std::unordered_map<RequestId, JoinOperation> Joins;
			std::vector<RequestId> Waiting;   // authenticated requests waiting for a session
			RequestId NextId = 1;
			Online::Stats Stats;

			// The push socket (realtime.md). Opened after every successful sign-in, and reopened by
			// close code; ReconnectAt is checked by Online::OnUpdate.
			Online::PushStatus Push = Online::PushStatus::Disconnected;
			OnlinePushHandler PushHandler;
			int ReconnectAttempts = 0;        // since the last successful open; drives the backoff
			int Upgrade401s = 0;              // refused upgrades in a row (see OnFailed)
			bool Replaced = false;            // a 4001 within the last minute (see OnClose)
			std::chrono::steady_clock::time_point ReplacedAt{};
			bool ReconnectPending = false;
			std::chrono::steady_clock::time_point ReconnectAt{};
			std::mt19937 Jitter{ std::random_device{}() };
		};

		SessionData* s_Data = nullptr;

		void Dispatch(RequestId id);
		void ConnectPush();
		void StartJoinAttempt(RequestId id);
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
					ConnectPush();   // no-op if it is already open, connecting, or was replaced
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

		// ---- The push socket -------------------------------------------------------------------

		void DeliverPush(const OnlinePush& push, bool counted)
		{
			if (counted)
				s_Data->Stats.Pushes++;
			const size_t queued = s_Data->PushHandler ? s_Data->PushHandler(push) : 0;
			if (queued == 0 && counted)
			{
				// Not a warning: an unknown type, or a known one nobody listens to in this scene, is
				// normal - the contract adds types without a version bump.
				s_Data->Stats.PushesUndelivered++;
				GE_CORE_TRACE("Online: push '{0}' ({1}) had no subscriber; dropped", push.Type, push.Id);
			}
		}

		// Exponential backoff with jitter: 0.25 s doubling to a 30 s cap, each delay drawn from its
		// upper half. Jitter is not optional: when a backend restarts, every client loses its socket
		// in the same instant, and reconnecting on a fixed schedule brings them all back in the same
		// instant too. `immediate` is for 1001 (the replica is going; another one is up).
		void ScheduleReconnect(const std::string& why, bool immediate)
		{
			double delay;
			if (immediate)
				delay = std::uniform_real_distribution<double>(0.0, 0.25)(s_Data->Jitter);
			else
			{
				const double ceiling = std::min(30.0, 0.25 * std::pow(2.0, s_Data->ReconnectAttempts));
				delay = std::uniform_real_distribution<double>(ceiling / 2.0, ceiling)(s_Data->Jitter);
				s_Data->ReconnectAttempts++;
			}

			s_Data->ReconnectPending = true;
			s_Data->ReconnectAt = std::chrono::steady_clock::now()
				+ std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(delay));
			GE_CORE_INFO("Online: push channel {0}; reconnecting in {1:.2f} s", why, delay);
		}

		// Reconnects once the session is good. An authenticated call recovers it the way every request
		// does (refresh, or sign in again), so the socket never needs its own token logic.
		void RecoverThenConnect(const std::string& why)
		{
			OnlineRequest probe;
			probe.Path = "/v1/me";
			Online::Send(std::move(probe), [why](const OnlineResponse& response)
			{
				if (!s_Data)
					return;
				if (response.IsSuccess())
					ConnectPush();
				else
					ScheduleReconnect(why + "; the session could not be recovered (" + DescribeFailure(response) + ")", false);
			});
		}

		void ConnectPush()
		{
			using PS = Online::PushStatus;
			if (!s_Data || s_Data->Push != PS::Disconnected || s_Data->Status != Online::Status::SignedIn)
				return;

			s_Data->ReconnectPending = false;
			s_Data->Push = PS::Connecting;

			OnlineTransport::SocketEvents events;
			events.OnOpen = []()
			{
				s_Data->Push = PS::Connected;
				s_Data->ReconnectAttempts = 0;
				s_Data->Upgrade401s = 0;
				s_Data->Stats.PushConnects++;
				GE_CORE_INFO("Online: push channel connected");

				// Pushes sent while the socket was down are gone, never replayed: whoever shows state
				// re-fetches it now (realtime.md, "the one rule").
				OnlinePush connected;
				connected.Type = "connected";
				DeliverPush(connected, /*counted=*/false);
			};

			events.OnText = [](const std::string& text)
			{
				std::string error;
				std::optional<JsonValue> envelope = ParseJson(text, &error);
				const JsonValue* type = envelope ? envelope->Find("type") : nullptr;
				if (!type || type->Kind != JsonValue::Type::String)
				{
					GE_CORE_WARN("Online: ignored a push that is not a {{type, id, payload}} envelope ({0})",
						envelope ? "no type" : error);
					return;
				}

				OnlinePush push;
				push.Type = type->String;
				if (const JsonValue* id = envelope->Find("id"); id && id->Kind == JsonValue::Type::String)
					push.Id = id->String;
				if (const JsonValue* payload = envelope->Find("payload"))
					push.Payload = *payload;
				// match.ready carries the connect token. It is a credential, so it stays in here:
				// JoinMatch reads a fresh one from the ticket, and no handler ever sees this one.
				auto& members = push.Payload.Members;
				members.erase(std::remove_if(members.begin(), members.end(),
					[](const auto& m) { return m.first == "connect_token"; }), members.end());
				DeliverPush(push, /*counted=*/true);
			};

			events.OnClose = [](int code, const std::string& reason, bool remote)
			{
				s_Data->Push = PS::Disconnected;
				switch (code)
				{
					case 4001:
					{
						// Reconnecting on every 4001 would replace the newer session, which would replace
						// this one, forever. Never reconnecting is not right either: a newer connection
						// is not always a live session. One was seen that was an upgrade request replayed
						// after its client had given up (most likely by Docker Desktop's port forwarder,
						// while a replica restarted), and it cost a live player its push channel - and,
						// 35 s later, its party seat. So: one more try after ~5 s, inside the 30 s
						// presence grace; a second 4001 within a minute is final. Two real sessions swap
						// once and settle.
						const auto now = std::chrono::steady_clock::now();
						if (s_Data->Replaced && now - s_Data->ReplacedAt < std::chrono::minutes(1))
						{
							s_Data->Push = PS::Replaced;
							GE_CORE_WARN("Online: push channel replaced again by a newer connection for this account "
								"(another session of profile '{0}' is active); not reconnecting", s_Data->Profile);
							return;
						}
						s_Data->Replaced = true;
						s_Data->ReplacedAt = now;
						s_Data->ReconnectPending = true;
						s_Data->ReconnectAt = now + std::chrono::milliseconds(
							std::uniform_int_distribution<int>(4500, 5500)(s_Data->Jitter));
						GE_CORE_WARN("Online: push channel replaced by a newer connection for this account; "
							"trying once more in ~5 s in case that was not a live session");
						return;
					}
					case 1001:
						ScheduleReconnect("closed 1001 (that replica is shutting down)", /*immediate=*/true);
						return;
					case 1008:
						GE_CORE_ERROR("Online: push channel closed 1008 (policy violation: the engine sent a data "
							"message, or fell too far behind). This is an engine bug");
						ScheduleReconnect("closed 1008", false);
						return;
					default:
						ScheduleReconnect(code == 1006 || code == 0 ? std::string("dropped")
							: "closed " + std::to_string(code) + (reason.empty() ? "" : " (" + reason + ")")
								+ (remote ? "" : " locally"), false);
				}
			};

			events.OnFailed = [](int status, const std::string& reason)
			{
				s_Data->Push = PS::Disconnected;
				// The upgrade's 401 body never reaches us (IXWebSocket reports only the status), so
				// token-expired and unauthorized look the same here. One authenticated call tells them
				// apart and fixes either; a second 401 in a row means it was not the token, and backs off.
				if (status == 401 && s_Data->Upgrade401s++ == 0)
				{
					GE_CORE_INFO("Online: push upgrade refused (401); recovering the session, then reconnecting");
					RecoverThenConnect("upgrade refused (401)");
					return;
				}
				ScheduleReconnect(status ? "upgrade refused (HTTP " + std::to_string(status) + ")"
					: "could not connect (" + reason + ")", false);
			};

			OnlineTransport::OpenSocket("/v1/realtime", s_Data->AccessToken, std::move(events));
		}
	}

	namespace {

		constexpr int kJoinAttempts = 3;
		constexpr std::chrono::milliseconds kJoinWait{ 1000 };

		void FinishJoin(RequestId id, const OnlineResponse& response)
		{
			auto it = s_Data->Joins.find(id);
			if (it == s_Data->Joins.end())
				return;
			OnlineCompletion completion = std::move(it->second.Completion);
			s_Data->Joins.erase(it);
			if (response.IsSuccess())
				s_Data->Stats.Joined++;
			completion(response);
		}

		void FailJoin(RequestId id, std::string reason)
		{
			OnlineResponse response;
			response.TransportError = std::move(reason);
			FinishJoin(id, response);
		}

		void OnHelloReply(RequestId id, bool answered, const std::string& text, const std::string& address)
		{
			auto it = s_Data->Joins.find(id);
			if (it == s_Data->Joins.end())
				return;
			it->second.Hello = 0;

			if (!answered)
			{
				if (it->second.Attempt < kJoinAttempts)
				{
					GE_CORE_INFO("Online: join attempt {0} got {1}; trying again with a fresh token",
						it->second.Attempt, text);
					StartJoinAttempt(id);
				}
				else
					FailJoin(id, text + " after " + std::to_string(kJoinAttempts) + " attempts");
				return;
			}

			// The reply's reason is for logs, never parsed beyond its first word (server-lifecycle.md).
			if (text.rfind("WELCOME ", 0) == 0)
			{
				GE_CORE_INFO("Online: joined the game server at {0} (attempt {1})", address, it->second.Attempt);
				JsonValue body = JsonValue::MakeObject();
				body.Members.emplace_back("account_id", JsonValue::MakeString(text.substr(8)));
				OnlineResponse response;
				response.Status = 200;
				WriteJson(body, response.Body);
				FinishJoin(id, response);
				return;
			}

			const std::string reason = text.rfind("DENIED ", 0) == 0 ? text.substr(7) : "an unexpected reply";
			GE_CORE_WARN("Online: the game server at {0} refused the join: {1}", address, reason);
			FailJoin(id, "denied: " + reason);
		}

		void OnJoinTicket(RequestId id, const OnlineResponse& response)
		{
			auto it = s_Data->Joins.find(id);
			if (it == s_Data->Joins.end())
				return;
			it->second.TicketRead = 0;

			if (!response.IsSuccess())
			{
				FinishJoin(id, response);   // the read's own failure, as any request reports it
				return;
			}

			std::optional<JsonValue> body = ParseJson(response.Body);
			const JsonValue* ticket = body ? body->Find("ticket") : nullptr;
			const JsonValue* state = ticket ? ticket->Find("state") : nullptr;
			const JsonValue* server = ticket ? ticket->Find("server") : nullptr;
			const JsonValue* address = server ? server->Find("address") : nullptr;
			const JsonValue* token = server ? server->Find("connect_token") : nullptr;
			if (!ticket || ticket->Kind == JsonValue::Type::Null)
			{
				FailJoin(id, "no ticket: queue first");
				return;
			}
			if (!state || state->Kind != JsonValue::Type::String || state->String != "ready"
				|| !address || address->Kind != JsonValue::Type::String || !token || token->Kind != JsonValue::Type::String)
			{
				FailJoin(id, "no ready match to join (the ticket is " + (state && state->Kind == JsonValue::Type::String
					? state->String : std::string("unreadable")) + ")");
				return;
			}

			it->second.Attempt++;
			s_Data->Stats.JoinAttempts++;
			const std::string where = address->String;
			it->second.Hello = OnlineTransport::Exchange(where, "HELLO " + token->String, kJoinWait,
				[id, where](bool answered, const std::string& text)
				{
					if (s_Data)
						OnHelloReply(id, answered, text, where);
				});
		}

		void StartJoinAttempt(RequestId id)
		{
			OnlineRequest read;
			read.Path = "/v1/matchmaking/ticket";
			const RequestId ticketRead = Online::Send(std::move(read), [id](const OnlineResponse& response)
			{
				if (s_Data)
					OnJoinTicket(id, response);
			});
			auto it = s_Data->Joins.find(id);
			if (it != s_Data->Joins.end())
				it->second.TicketRead = ticketRead;
		}
	}

	RequestId Online::JoinMatch(OnlineCompletion completion)
	{
		GE_CORE_ASSERT(JobSystem::IsMainThread(), "Online::JoinMatch is main-thread only");
		if (!s_Data)
		{
			OnlineTransport::Start({}, {}, std::move(completion));   // fails it, uninitialised
			return 0;
		}

		const RequestId id = s_Data->NextId++;
		s_Data->Joins[id].Completion = std::move(completion);
		s_Data->Stats.Joins++;
		StartJoinAttempt(id);
		return id;
	}

	void Online::OnUpdate()
	{
		if (!s_Data)
			return;

		// Waiting for a game server's reply is a non-blocking receive here, once a frame.
		OnlineTransport::PollExchanges();

		if (!s_Data->ReconnectPending || std::chrono::steady_clock::now() < s_Data->ReconnectAt)
			return;

		s_Data->ReconnectPending = false;
		if (s_Data->Status == Status::SignedIn && !s_Data->Recovering)
			ConnectPush();
		else
			RecoverThenConnect("waiting for a session");   // signs in on its behalf, with its backoff
	}

	Online::PushStatus Online::GetPushStatus()
	{
		return s_Data ? s_Data->Push : PushStatus::Disconnected;
	}

	void Online::SetPushHandler(OnlinePushHandler handler)
	{
		if (s_Data)
			s_Data->PushHandler = std::move(handler);
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
			"{5} sign-ins, {6} refreshes, {7} dropped late, {8} abandoned in flight; push: {9} connects, {10} messages, "
			"{11} with no subscriber; joins: {12} ({13} HELLOs), {14} joined", stats.Sent, stats.Succeeded, stats.Failed,
			stats.Cancelled, stats.Retried, stats.SignIns, stats.Refreshes, stats.DroppedLate, abandoned,
			stats.PushConnects, stats.Pushes, stats.PushesUndelivered, stats.Joins, stats.JoinAttempts, stats.Joined);

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

		if (auto join = s_Data->Joins.find(id); join != s_Data->Joins.end())
		{
			const RequestId ticketRead = join->second.TicketRead;
			OnlineTransport::AbortExchange(join->second.Hello);
			s_Data->Joins.erase(join);
			Cancel(ticketRead);   // a no-op for 0; counted as a cancelled request if it was pending
			return;
		}

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
