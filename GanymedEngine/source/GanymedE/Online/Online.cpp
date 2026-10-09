#include "gepch.h"
#include "GanymedE/Online/Online.h"

#include "GanymedE/Core/JobSystem.h"
#include "GanymedE/main/Application.h"

// The only engine TU that includes IXWebSocket, and it must stay that way: IXNetSystem.h pulls
// in winsock2.h and redefines EWOULDBLOCK, EAGAIN, EINVAL and friends to their WSA values for
// the rest of whatever TU includes it. See docs/engine/build-and-tooling.md, IXWebSocket.
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>

#include <cstring>

namespace GanymedE {

	namespace {

		// A pool rather than one client, because one ix::HttpClient in async mode is one thread
		// running one request at a time off a queue (HttpClient::run): a single slow request
		// holds up everything behind it. Four covers the engine's real concurrency (a sign-in, a
		// leaderboard read, a ticket poll) without a thread per request.
		constexpr size_t kClientCount = 4;

		struct PendingRequest
		{
			ix::HttpRequestArgsPtr Args;   // shared with the network thread; Cancel sets Args->cancel
			size_t Client = 0;
			OnlineCompletion Completion;
			bool Cancelled = false;
		};

		struct OnlineData
		{
			std::string BackendUrl;
			std::vector<std::unique_ptr<ix::HttpClient>> Clients;
			std::array<uint32_t, kClientCount> Load{};   // requests queued or running, per client

			// Main thread only. The network threads never touch it: a completion is copied off the
			// network thread and looked up here once it reaches the main thread.
			std::unordered_map<RequestId, PendingRequest> Pending;
			RequestId NextId = 1;
			Online::Stats Stats;
		};

		OnlineData* s_Data = nullptr;

		std::string ReadBackendUrl()
		{
			// 127.0.0.1, not localhost: on Windows a refused connect to localhost costs ~4 s
			// against ~2 s, because both ::1 and 127.0.0.1 are tried (measured in ONLINE.md, O0).
			std::string url = "http://127.0.0.1:8080";

			const ApplicationCommandLineArgs& args = Application::GetCommandLineArgs();
			const char* kFlag = "--backend=";
			const std::size_t flagLength = std::strlen(kFlag);
			for (int i = 1; i < args.Count; i++)
			{
				if (args.Args[i] && std::strncmp(args.Args[i], kFlag, flagLength) == 0)
					url = args.Args[i] + flagLength;   // last one wins, as --renderer=
			}

			while (!url.empty() && url.back() == '/')
				url.pop_back();
			return url;
		}

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
		void Deliver(RequestId id, const OnlineResponse& response)
		{
			GE_PROFILE_FUNCTION();

			if (!s_Data)
				return;   // queued before Shutdown, drained by JobSystem::Shutdown after it

			auto it = s_Data->Pending.find(id);
			if (it == s_Data->Pending.end())
				return;

			PendingRequest pending = std::move(it->second);
			s_Data->Pending.erase(it);
			s_Data->Load[pending.Client]--;
			s_Data->Stats.InFlight--;

			if (pending.Cancelled)
			{
				s_Data->Stats.DroppedLate++;
				return;
			}

			if (response.IsSuccess())
				s_Data->Stats.Succeeded++;
			else
				s_Data->Stats.Failed++;

			// Erased before the call, so a completion that sends or cancels sees a consistent map.
			pending.Completion(response);
		}

		void FailWithoutSending(OnlineCompletion completion, std::string reason)
		{
			OnlineResponse response;
			response.TransportError = std::move(reason);
			JobSystem::SubmitToMainThread([completion = std::move(completion), response]()
			{
				completion(response);
			});
		}
	}

	void Online::Init()
	{
		GE_CORE_ASSERT(!s_Data, "Online::Init called twice");

		// WSAStartup on Windows, nothing elsewhere. Before the first client: an async client
		// starts its thread in its constructor.
		ix::initNetSystem();

		s_Data = new OnlineData();
		s_Data->BackendUrl = ReadBackendUrl();
		for (size_t i = 0; i < kClientCount; i++)
			s_Data->Clients.push_back(std::make_unique<ix::HttpClient>(/*async=*/true));

		GE_CORE_INFO("Online initialised (backend {0}, {1} clients)", s_Data->BackendUrl, kClientCount);
	}

	void Online::Shutdown()
	{
		if (!s_Data)
			return;

		// Joins the network threads, and does not wait out a transfer to do it: ix::HttpClient's
		// destructor sets _stop, which the request's cancellation check reads during the connect
		// AND the transfer (the check captures its timeout object by reference, so it survives
		// the switch from connect to transfer timeout). Measured: closing the runtime with a
		// 10-second request in flight exits in 0.6-0.7 s, the same as with nothing in flight.
		// Requests still queued behind it are never started.
		s_Data->Clients.clear();

		const Stats& stats = s_Data->Stats;
		GE_CORE_INFO("Online shut down: {0} sent, {1} succeeded, {2} failed, {3} cancelled, "
			"{4} dropped late, {5} abandoned in flight", stats.Sent, stats.Succeeded, stats.Failed,
			stats.Cancelled, stats.DroppedLate, s_Data->Pending.size());

		delete s_Data;
		s_Data = nullptr;

		ix::uninitNetSystem();
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
			FailWithoutSending(std::move(completion), "online is not initialised");
			return 0;
		}

		size_t client = 0;
		for (size_t i = 1; i < kClientCount; i++)
		{
			if (s_Data->Load[i] < s_Data->Load[client])
				client = i;
		}

		const RequestId id = s_Data->NextId++;
		const std::string url = s_Data->BackendUrl + request.Path;

		ix::HttpRequestArgsPtr args = s_Data->Clients[client]->createRequest(url, request.Method);
		args->body = std::move(request.Body);
		args->connectTimeout = request.ConnectTimeoutSeconds;
		args->transferTimeout = request.TransferTimeoutSeconds;
		// A redirect would carry the request's headers (from O2 on, a bearer token) to wherever the
		// Location points. The backend never redirects, so one is an error, not a hop.
		args->followRedirects = false;
		args->compress = false;   // built without zlib
		args->extraHeaders["Accept"] = "application/json";
		if (!args->body.empty())
			args->extraHeaders["Content-Type"] = "application/json";
		for (auto& [name, value] : request.Headers)
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

		PendingRequest pending;
		pending.Args = std::move(args);
		pending.Client = client;
		pending.Completion = std::move(completion);
		s_Data->Pending.emplace(id, std::move(pending));
		s_Data->Load[client]++;
		s_Data->Stats.Sent++;
		s_Data->Stats.InFlight++;
		return id;
	}

	void Online::Cancel(RequestId id)
	{
		if (!s_Data)
			return;

		auto it = s_Data->Pending.find(id);
		if (it == s_Data->Pending.end() || it->second.Cancelled)
			return;

		// Kept in the map until the network thread reports back, so the client's load stays
		// right and the late response is counted rather than delivered.
		it->second.Cancelled = true;
		it->second.Args->cancel = true;
		it->second.Completion = nullptr;   // release whatever it captured now, not on arrival
		s_Data->Stats.Cancelled++;
	}

	Online::Stats Online::GetStats()
	{
		return s_Data ? s_Data->Stats : Stats{};
	}
}
