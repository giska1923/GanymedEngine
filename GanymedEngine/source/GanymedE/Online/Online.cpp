#include "gepch.h"
#include "GanymedE/Online/OnlineTransport.h"

#include "GanymedE/Core/JobSystem.h"

// The only engine TU that includes IXWebSocket, and it must stay that way: IXNetSystem.h pulls
// in winsock2.h and redefines EWOULDBLOCK, EAGAIN, EINVAL and friends to their WSA values for
// the rest of whatever TU includes it. See docs/engine/build-and-tooling.md, IXWebSocket.
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXUdpSocket.h>
#include <ixwebsocket/IXWebSocket.h>

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

			// The push socket. Its events carry the generation they were raised under, and the
			// main thread drops any whose generation is not the current one.
			std::unique_ptr<ix::WebSocket> Socket;
			uint64_t SocketGeneration = 0;
			SocketEvents Events;

			// UDP exchanges in flight, polled by PollExchanges.
			struct PendingExchange
			{
				std::unique_ptr<ix::UdpSocket> Socket;
				std::string Address;
				std::chrono::steady_clock::time_point Deadline;
				std::function<void(bool, const std::string&)> Completion;
			};
			std::unordered_map<ExchangeId, PendingExchange> Exchanges;
			ExchangeId NextExchangeId = 1;
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
			out.Headers.assign(response->headers.begin(), response->headers.end());
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

		// Network thread -> main thread, for the socket raised under `generation` only.
		template<typename Fn>
		void PostSocketEvent(uint64_t generation, Fn&& fn)
		{
			JobSystem::SubmitToMainThread([generation, fn = std::forward<Fn>(fn)]()
			{
				if (s_Data && s_Data->SocketGeneration == generation)
					fn(s_Data->Events);
			});
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

		CloseSocket();
		s_Data->Exchanges.clear();   // abandoned: their completions never run

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

	void OpenSocket(const std::string& path, const std::string& bearer, SocketEvents events)
	{
		GE_CORE_ASSERT(JobSystem::IsMainThread(), "OnlineTransport::OpenSocket is main-thread only");
		if (!s_Data)
			return;

		CloseSocket();
		const uint64_t generation = ++s_Data->SocketGeneration;
		s_Data->Events = std::move(events);

		std::string url = s_Data->BaseUrl;
		if (url.rfind("https://", 0) == 0)
			url.replace(0, 5, "wss");
		else if (url.rfind("http://", 0) == 0)
			url.replace(0, 4, "ws");

		auto socket = std::make_unique<ix::WebSocket>();
		socket->setUrl(url + path);
		ix::WebSocketHttpHeaders headers;
		headers["Authorization"] = "Bearer " + bearer;
		socket->setExtraHeaders(headers);
		// The session reconnects by close code (4001 must not reconnect at all), so IXWebSocket's
		// own loop is off. That also avoids a stop() hang that upstream fixed after v12.0.1 (#609),
		// which needs automatic reconnection to race.
		socket->disableAutomaticReconnection();
		// Built without zlib, so compression must never be negotiated: frames the server
		// compressed could not be read.
		socket->disablePerMessageDeflate();
		socket->setHandshakeTimeout(5);   // IXWebSocket's default is 60 s

		// Network thread. Copies what it needs and posts it; nothing here touches s_Data.
		socket->setOnMessageCallback([generation](const ix::WebSocketMessagePtr& message)
		{
			switch (message->type)
			{
				case ix::WebSocketMessageType::Open:
					PostSocketEvent(generation, [](SocketEvents& e) { if (e.OnOpen) e.OnOpen(); });
					break;
				case ix::WebSocketMessageType::Message:
					if (!message->binary)
					{
						PostSocketEvent(generation, [text = message->str](SocketEvents& e)
						{
							if (e.OnText) e.OnText(text);
						});
					}
					break;
				case ix::WebSocketMessageType::Close:
					PostSocketEvent(generation, [code = int(message->closeInfo.code), reason = message->closeInfo.reason,
						remote = message->closeInfo.remote](SocketEvents& e)
					{
						if (e.OnClose) e.OnClose(code, reason, remote);
					});
					break;
				case ix::WebSocketMessageType::Error:
					PostSocketEvent(generation, [status = message->errorInfo.http_status, reason = message->errorInfo.reason](SocketEvents& e)
					{
						if (e.OnFailed) e.OnFailed(status, reason);
					});
					break;
				default:
					break;   // ping/pong are answered by IXWebSocket itself; fragments are reassembled
			}
		});

		socket->start();
		s_Data->Socket = std::move(socket);
	}

	ExchangeId Exchange(const std::string& address, const std::string& payload, std::chrono::milliseconds timeout,
		std::function<void(bool ok, const std::string& text)> completion)
	{
		GE_CORE_ASSERT(JobSystem::IsMainThread(), "OnlineTransport::Exchange is main-thread only");

		auto fail = [&completion](std::string why)
		{
			JobSystem::SubmitToMainThread([completion = std::move(completion), why]() { completion(false, why); });
			return ExchangeId{ 0 };
		};
		if (!s_Data)
			return fail("online is not initialised");

		const size_t colon = address.rfind(':');
		const int port = colon == std::string::npos ? 0 : std::atoi(address.c_str() + colon + 1);
		if (colon == std::string::npos || port <= 0 || port > 65535)
			return fail("'" + address + "' is not host:port");

		// Non-blocking (UdpSocket::init sets it), which is what lets one poll a frame do the waiting.
		auto socket = std::make_unique<ix::UdpSocket>();
		std::string error;
		if (!socket->init(address.substr(0, colon), port, error))
			return fail("cannot reach " + address + ": " + error);
		if (socket->sendto(payload) < 0)
			return fail("cannot send to " + address);

		const ExchangeId id = s_Data->NextExchangeId++;
		auto& pending = s_Data->Exchanges[id];
		pending.Socket = std::move(socket);
		pending.Address = address;
		pending.Deadline = std::chrono::steady_clock::now() + timeout;
		pending.Completion = std::move(completion);
		return id;
	}

	void AbortExchange(ExchangeId id)
	{
		if (s_Data)
			s_Data->Exchanges.erase(id);
	}

	void PollExchanges()
	{
		if (!s_Data || s_Data->Exchanges.empty())
			return;

		const auto now = std::chrono::steady_clock::now();
		std::vector<std::pair<std::function<void(bool, const std::string&)>, std::pair<bool, std::string>>> done;
		for (auto it = s_Data->Exchanges.begin(); it != s_Data->Exchanges.end();)
		{
			auto& pending = it->second;
			char buffer[512];
			const auto received = pending.Socket->recvfrom(buffer, sizeof(buffer));
			bool finished = true;
			std::pair<bool, std::string> outcome;
			if (received >= 0)
				outcome = { true, std::string(buffer, static_cast<size_t>(received)) };
			else if (!ix::UdpSocket::isWaitNeeded())
				// Windows reports an ICMP port-unreachable for an earlier send as a failed receive
				// (WSAECONNRESET): nothing is listening on that port. Elsewhere, any receive error.
				outcome = { false, "nothing is listening at " + pending.Address };
			else if (now >= pending.Deadline)
				outcome = { false, "no answer from " + pending.Address };
			else
				finished = false;

			if (!finished)
			{
				++it;
				continue;
			}
			done.emplace_back(std::move(pending.Completion), std::move(outcome));
			it = s_Data->Exchanges.erase(it);
		}

		// Called after the loop, so a completion that starts another exchange sees a stable map.
		for (auto& [completion, outcome] : done)
			completion(outcome.first, outcome.second);
	}

	void CloseSocket()
	{
		if (!s_Data || !s_Data->Socket)
			return;

		// Bumped first, so nothing this socket raises - including the close below - reaches the
		// caller's events.
		s_Data->SocketGeneration++;
		s_Data->Socket->stop();   // sends a close frame and joins the socket's thread
		s_Data->Socket.reset();
		s_Data->Events = {};
	}
}
