#include "gepch.h"
#include "GanymedE/Online/OnlineTransport.h"

#include "GanymedE/Core/JobSystem.h"

// The only engine TU that includes IXWebSocket, and it must stay that way: IXNetSystem.h pulls
// in winsock2.h and redefines EWOULDBLOCK, EAGAIN, EINVAL and friends to their WSA values for
// the rest of whatever TU includes it. See docs/engine/build-and-tooling.md, IXWebSocket.
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>

namespace GanymedE::OnlineTransport {

	namespace {

		// A pool rather than one client, because one ix::HttpClient in async mode is one thread
		// running one request at a time off a queue (HttpClient::run): a single slow request
		// holds up everything behind it. Four covers the engine's real concurrency (a sign-in, a
		// leaderboard read, a ticket poll) without a thread per request.
		constexpr size_t kClientCount = 4;

		struct PendingAttempt
		{
			ix::HttpRequestArgsPtr Args;   // shared with the network thread; Abort sets Args->cancel
			size_t Client = 0;
			std::function<void(const OnlineResponse&)> Completion;
			bool Aborted = false;
		};

		struct TransportData
		{
			std::string BaseUrl;
			std::vector<std::unique_ptr<ix::HttpClient>> Clients;
			std::array<uint32_t, kClientCount> Load{};   // attempts queued or running, per client

			// Main thread only. The network threads never touch it: a completion is copied off the
			// network thread and looked up here once it reaches the main thread.
			std::unordered_map<AttemptId, PendingAttempt> Pending;
			AttemptId NextId = 1;
			uint64_t DroppedLate = 0;
		};

		TransportData* s_Data = nullptr;

		// The engine's wording, not IXWebSocket's: its messages embed strerror, and Windows maps a
		// refused connect to "No error".
		std::string DescribeTransportError(ix::HttpErrorCode code, const std::string& url)
		{
			switch (code)
			{
				case ix::HttpErrorCode::CannotConnect:      return "cannot connect to " + url;
				case ix::HttpErrorCode::Timeout:            return "timed out";
				case ix::HttpErrorCode::UrlMalformed:       return "malformed URL " + url;
				case ix::HttpErrorCode::Cancelled:          return "cancelled";
				case ix::HttpErrorCode::SendError:          return "connection lost while sending";
				case ix::HttpErrorCode::TooManyRedirects:
				case ix::HttpErrorCode::MissingLocation:    return "unexpected redirect";
				default:                                    return "connection lost while receiving";
			}
		}

		// Network thread: takes the URL by value rather than reading s_Data.
		OnlineResponse Translate(const ix::HttpResponsePtr& response, const std::string& url)
		{
			OnlineResponse out;
			if (response->errorCode != ix::HttpErrorCode::Ok)
			{
				out.TransportError = DescribeTransportError(response->errorCode, url);
				return out;
			}

			out.Status = response->statusCode;
			out.Body = response->body;
			auto contentType = response->headers.find("Content-Type");   // the map is case-insensitive
			if (contentType != response->headers.end())
				out.ContentType = contentType->second;
			return out;
		}

		// Main thread. Where a response becomes a completion call, or is dropped.
		void Deliver(AttemptId id, const OnlineResponse& response)
		{
			GE_PROFILE_FUNCTION();

			if (!s_Data)
				return;   // queued before Shutdown, drained by JobSystem::Shutdown after it

			auto it = s_Data->Pending.find(id);
			if (it == s_Data->Pending.end())
				return;

			PendingAttempt pending = std::move(it->second);
			s_Data->Pending.erase(it);
			s_Data->Load[pending.Client]--;

			if (pending.Aborted)
			{
				s_Data->DroppedLate++;
				return;
			}

			// Erased before the call, so a completion that starts or aborts attempts sees a
			// consistent map.
			pending.Completion(response);
		}

		void FailWithoutSending(std::function<void(const OnlineResponse&)> completion, std::string reason)
		{
			OnlineResponse response;
			response.TransportError = std::move(reason);
			JobSystem::SubmitToMainThread([completion = std::move(completion), response]()
			{
				completion(response);
			});
		}
	}

	void Init(const std::string& baseUrl)
	{
		GE_CORE_ASSERT(!s_Data, "OnlineTransport::Init called twice");

		// WSAStartup on Windows, nothing elsewhere. Before the first client: an async client
		// starts its thread in its constructor.
		ix::initNetSystem();

		s_Data = new TransportData();
		s_Data->BaseUrl = baseUrl;
		for (size_t i = 0; i < kClientCount; i++)
			s_Data->Clients.push_back(std::make_unique<ix::HttpClient>(/*async=*/true));
	}

	size_t Shutdown()
	{
		if (!s_Data)
			return 0;

		// Joins the network threads, and does not wait out a transfer to do it: ix::HttpClient's
		// destructor sets _stop, which the request's cancellation check reads during the connect
		// AND the transfer (the check captures its timeout object by reference, so it survives
		// the switch from connect to transfer timeout). Measured: closing the runtime with a
		// 10-second request in flight exits in 0.6-0.7 s, the same as with nothing in flight.
		// Requests still queued behind it are never started.
		s_Data->Clients.clear();

		const size_t abandoned = s_Data->Pending.size();
		delete s_Data;
		s_Data = nullptr;

		ix::uninitNetSystem();
		return abandoned;
	}

	bool IsInitialized()
	{
		return s_Data != nullptr;
	}

	AttemptId Start(const OnlineRequest& request, const std::string& bearer,
		std::function<void(const OnlineResponse&)> completion)
	{
		GE_CORE_ASSERT(JobSystem::IsMainThread(), "OnlineTransport::Start is main-thread only");

		if (!s_Data)
		{
			FailWithoutSending(std::move(completion), "online is not initialised");
			return 0;
		}

		size_t client = 0;
		for (size_t i = 1; i < kClientCount; i++)
		{
			if (s_Data->Load[i] < s_Data->Load[client])
				client = i;
		}

		const AttemptId id = s_Data->NextId++;
		const std::string url = s_Data->BaseUrl + request.Path;

		ix::HttpRequestArgsPtr args = s_Data->Clients[client]->createRequest(url, request.Method);
		args->body = request.Body;
		args->connectTimeout = request.ConnectTimeoutSeconds;
		args->transferTimeout = request.TransferTimeoutSeconds;
		// A redirect would carry the request's headers - a bearer token - to wherever the Location
		// points. The backend never redirects, so one is an error, not a hop.
		args->followRedirects = false;
		args->compress = false;   // built without zlib
		args->extraHeaders["Accept"] = "application/json";
		if (!args->body.empty())
			args->extraHeaders["Content-Type"] = "application/json";
		if (!bearer.empty())
			args->extraHeaders["Authorization"] = "Bearer " + bearer;
		for (const auto& [name, value] : request.Headers)
			args->extraHeaders[name] = value;

		// Network thread. Copies the response and leaves; nothing here touches s_Data.
		const bool queued = s_Data->Clients[client]->performRequest(args,
			[id, url](const ix::HttpResponsePtr& response)
			{
				JobSystem::SubmitToMainThread([id, translated = Translate(response, url)]()
				{
					Deliver(id, translated);
				});
			});

		if (!queued)
		{
			FailWithoutSending(std::move(completion), "could not queue the request");
			return 0;
		}

		PendingAttempt pending;
		pending.Args = std::move(args);
		pending.Client = client;
		pending.Completion = std::move(completion);
		s_Data->Pending.emplace(id, std::move(pending));
		s_Data->Load[client]++;
		return id;
	}

	void Abort(AttemptId id)
	{
		if (!s_Data)
			return;

		auto it = s_Data->Pending.find(id);
		if (it == s_Data->Pending.end() || it->second.Aborted)
			return;

		// Kept in the map until the network thread reports back, so the client's load stays
		// right and the late response is counted rather than delivered.
		it->second.Aborted = true;
		it->second.Args->cancel = true;
		it->second.Completion = nullptr;   // release whatever it captured now, not on arrival
	}

	uint64_t DroppedLate()
	{
		return s_Data ? s_Data->DroppedLate : 0;
	}

	uint32_t InFlight()
	{
		return s_Data ? static_cast<uint32_t>(s_Data->Pending.size()) : 0;
	}
}
