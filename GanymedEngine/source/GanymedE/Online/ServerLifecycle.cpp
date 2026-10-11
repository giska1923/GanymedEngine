#include "gepch.h"
#include "GanymedE/Online/ServerLifecycle.h"

#include "GanymedE/Online/ConnectToken.h"
#include "GanymedE/Online/GamePort.h"
#include "GanymedE/Online/Json.h"
#include "GanymedE/Online/OnlineTransport.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GanymedE {

	namespace {

		std::string Quote(const std::string& text)
		{
			std::string out = "\"";
			for (char c : text)
			{
				if (c == '"' || c == '\\')
					out.push_back('\\');
				out.push_back(c);
			}
			out.push_back('"');
			return out;
		}

		std::string Trim(std::string text)
		{
			while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' || text.back() == '\t'))
				text.pop_back();
			size_t start = 0;
			while (start < text.size() && (text[start] == ' ' || text[start] == '\n' || text[start] == '\r' || text[start] == '\t'))
				start++;
			return text.substr(start);
		}

		int64_t NowUnix()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
		}

	}

	struct ServerLifecycle::State
	{
		GamePort Port;
		std::thread Health;
		std::thread Door;

		std::mutex StopMutex;
		std::condition_variable StopCv;
		std::atomic<bool> Stop{ false };

		std::mutex DoorMutex;
		enum class Phase { Waiting, Open, Over } Phase = Phase::Waiting;
		std::vector<std::string> Players;
		std::unordered_map<std::string, int64_t> SeenNonce;   // nonce -> expiry, unix seconds
		std::unordered_set<std::string> Admitted;

		bool Transport = false;
		bool TornDown = false;
		bool Acknowledged = false;
	};

	ServerLifecycle::~ServerLifecycle()
	{
		TearDown();
	}

	bool ServerLifecycle::IsStopped() const
	{
		return m_State && m_State->Stop.load();
	}

	void ServerLifecycle::RequestStop()
	{
		if (!m_State)
			return;
		m_State->Stop = true;
		m_State->StopCv.notify_all();
	}

	bool ServerLifecycle::Start(const ServerLaunch& launch, std::string& error)
	{
		m_Launch = launch;
		m_State = new State();

		// Before the first socket, so WSAStartup has run and a later Shutdown is paired.
		OnlineTransport::Init(std::string());
		m_State->Transport = true;

		if (!m_State->Port.Bind(launch.Port, error))
		{
			TearDown();
			return false;
		}

		GE_CORE_INFO("Game port {0} bound ({1})", launch.Port, launch.Advertise);

		m_State->Health = std::thread([this]() { HealthLoop(); });
		m_State->Door = std::thread([this]() { DoorLoop(); });
		return true;
	}

	void ServerLifecycle::HealthLoop()
	{
		// Immediately, then every 2 s. The agent's kill line is 6 s of silence.
		for (;;)
		{
			int status = 0;
			std::string body;
			std::string error;
			const bool arrived = Post("/health", "", 5, status, body, error);
			if (m_State->Stop)
				return;

			if (!arrived || status < 200 || status >= 300)
			{
				GE_CORE_WARN("Health refused ({0}): stopping", arrived ? std::to_string(status) : error);
				RequestStop();
				return;
			}

			std::unique_lock<std::mutex> lock(m_State->StopMutex);
			if (m_State->StopCv.wait_for(lock, std::chrono::seconds(2), [this]() { return m_State->Stop.load(); }))
				return;
		}
	}

	bool ServerLifecycle::Post(const std::string& path, const std::string& body, int transferTimeoutSeconds,
		int& status, std::string& responseBody, std::string& error)
	{
		const std::string url = m_Launch.Agent + "/v1/servers/" + m_Launch.ServerId + path;
		const OnlineTransport::BlockingResponse response =
			OnlineTransport::PostBlocking(url, body, transferTimeoutSeconds);
		if (!response.Arrived())
		{
			error = response.TransportError;
			return false;
		}
		status = response.Status;
		responseBody = response.Body;
		return true;
	}

	bool ServerLifecycle::WaitForAllocation(std::string& error)
	{
		GE_CORE_INFO("Waiting for an allocation");

		for (;;)
		{
			if (m_State->Stop)
			{
				error = "stopped while waiting for an allocation";
				TearDown();
				return false;
			}

			int status = 0;
			std::string body;
			std::string postError;
			const bool arrived = Post("/ready", "", 25, status, body, postError);
			if (m_State->Stop)
			{
				error = "stopped while waiting for an allocation";
				TearDown();
				return false;
			}

			if (!arrived)
			{
				GE_CORE_WARN("ready failed ({0}); retrying", postError);
				std::unique_lock<std::mutex> lock(m_State->StopMutex);
				m_State->StopCv.wait_for(lock, std::chrono::seconds(1), [this]() { return m_State->Stop.load(); });
				continue;
			}

			if (status == 204)
				continue;

			if (status != 200)
			{
				error = "agent refused ready: " + std::to_string(status);
				TearDown();
				return false;
			}

			std::string parseError;
			const std::optional<JsonValue> json = ParseJson(body, &parseError);
			const JsonValue* match = json ? json->Find("match_id") : nullptr;
			const JsonValue* players = json ? json->Find("players") : nullptr;
			if (!json || !match || match->Kind != JsonValue::Type::String || !players || players->Kind != JsonValue::Type::Array)
			{
				GE_CORE_WARN("ready returned a body that is not an allocation ({0}); retrying", parseError);
				std::unique_lock<std::mutex> lock(m_State->StopMutex);
				m_State->StopCv.wait_for(lock, std::chrono::seconds(1), [this]() { return m_State->Stop.load(); });
				continue;
			}

			m_MatchId = match->String;
			std::vector<std::string> expected;
			for (const JsonValue& player : players->Items)
			{
				if (player.Kind != JsonValue::Type::String)
				{
					error = "allocation has a player that is not a string";
					TearDown();
					return false;
				}
				expected.push_back(player.String);
			}

			GE_CORE_INFO("Allocated match {0} for {1} players", m_MatchId, expected.size());

			const std::string ack = "{\"match_id\":" + Quote(m_MatchId) + "}";
			int ackStatus = 0;
			std::string ackBody;
			std::string ackError;
			const bool ackArrived = Post("/allocated", ack, 5, ackStatus, ackBody, ackError);
			if (!ackArrived || ackStatus < 200 || ackStatus >= 300)
			{
				error = ackArrived ? "acknowledgement refused: " + std::to_string(ackStatus) : ackError;
				int ignored = 0;
				std::string ignoredBody;
				std::string ignoredError;
				Post("/shutdown", "", 5, ignored, ignoredBody, ignoredError);
				TearDown();
				return false;
			}

			{
				std::lock_guard<std::mutex> lock(m_State->DoorMutex);
				m_State->Players = std::move(expected);
				m_State->SeenNonce.clear();
				m_State->Admitted.clear();
				m_State->Phase = State::Phase::Open;
				m_State->Acknowledged = true;
			}
			return true;
		}
	}

	void ServerLifecycle::GiveUp()
	{
		if (!m_State || m_State->TornDown)
			return;

		int status = 0;
		std::string body;
		std::string error;
		Post("/shutdown", "", 5, status, body, error);
		TearDown();
	}

	void ServerLifecycle::ReportResult()
	{
		const std::string body = "{\"outcome\":" + Quote(m_Launch.Outcome) + "}";
		int backoffMs = 500;
		for (int attempt = 1; attempt <= 5; attempt++)
		{
			int status = 0;
			std::string response;
			std::string error;
			const bool arrived = Post("/result", body, 10, status, response, error);
			if (arrived && status == 200)
			{
				GE_CORE_INFO("Result reported ({0}), attempt {1}", m_Launch.Outcome, attempt);
				return;
			}
			if (arrived && status < 500)
			{
				GE_CORE_ERROR("Result refused ({0}); not retrying", status);
				return;
			}

			GE_CORE_WARN("Result report failed (attempt {0}): {1}", attempt,
				arrived ? std::to_string(status) : error);
			if (attempt == 5)
				break;

			std::unique_lock<std::mutex> lock(m_State->StopMutex);
			m_State->StopCv.wait_for(lock, std::chrono::milliseconds(backoffMs), [this]() { return false; });
			backoffMs *= 2;
		}
		GE_CORE_ERROR("Gave up reporting the result");
	}

	void ServerLifecycle::Finish()
	{
		if (!m_State || m_State->TornDown)
			return;

		size_t admitted = 0;
		{
			std::lock_guard<std::mutex> lock(m_State->DoorMutex);
			m_State->Phase = State::Phase::Over;
			admitted = m_State->Admitted.size();
		}
		GE_CORE_INFO("Match over, {0} players admitted", admitted);

		ReportResult();

		int status = 0;
		std::string body;
		std::string error;
		Post("/shutdown", "", 5, status, body, error);
		TearDown();
	}

	void ServerLifecycle::TearDown()
	{
		if (!m_State)
			return;
		if (m_State->TornDown)
			return;
		m_State->TornDown = true;

		RequestStop();
		if (m_State->Health.joinable())
			m_State->Health.join();

		// Stop is already set, so the door leaves recvfrom within its 200 ms timeout and the
		// loop exits. Join before close: closing under a blocked recvfrom races m_Socket, and
		// on Linux the descriptor number can be reused by another socket before the call returns.
		if (m_State->Door.joinable())
			m_State->Door.join();
		m_State->Port.Close();

		if (m_State->Transport)
		{
			OnlineTransport::Shutdown();
			m_State->Transport = false;
		}

		delete m_State;
		m_State = nullptr;
	}

	void ServerLifecycle::DoorLoop()
	{
		while (!m_State->Stop)
		{
			std::string text;
			GamePeer from;
			std::string error;
			switch (m_State->Port.Receive(text, from, error))
			{
				case GamePort::Recv::Packet:
				{
					const std::string reply = Answer(text);
					GE_CORE_INFO("Join from {0}: {1}", from.Text, reply);
					m_State->Port.Send(from, reply);
					break;
				}
				case GamePort::Recv::Timeout:
					break;
				case GamePort::Recv::Closed:
					return;
				case GamePort::Recv::Error:
					GE_CORE_WARN("Game port read failed: {0}", error);
					break;
			}
		}
	}

	std::string ServerLifecycle::Answer(const std::string& datagram)
	{
		const std::string text = Trim(datagram);
		constexpr const char* kHello = "HELLO ";
		if (text.rfind(kHello, 0) != 0)
			return "DENIED expected HELLO <token>";

		const std::string token = text.substr(std::char_traits<char>::length(kHello));

		std::lock_guard<std::mutex> lock(m_State->DoorMutex);
		if (m_State->Phase == State::Phase::Waiting)
			return "DENIED not allocated";
		if (m_State->Phase == State::Phase::Over)
			return "DENIED match over";

		const ConnectResult checked = VerifyConnectToken(m_Launch.PublicKey, token,
			m_Launch.Advertise, m_MatchId, NowUnix());
		if (!checked.Ok)
			return std::string("DENIED ") + checked.Reason;

		bool expected = false;
		for (const std::string& player : m_State->Players)
		{
			if (player == checked.Claims.AccountId)
			{
				expected = true;
				break;
			}
		}
		if (!expected)
			return "DENIED not a player in this match";

		const int64_t now = NowUnix();
		for (auto it = m_State->SeenNonce.begin(); it != m_State->SeenNonce.end(); )
		{
			if (now > it->second)
				it = m_State->SeenNonce.erase(it);
			else
				++it;
		}

		if (m_State->SeenNonce.find(checked.Claims.Nonce) != m_State->SeenNonce.end())
			return "DENIED token already used";

		m_State->SeenNonce.emplace(checked.Claims.Nonce, checked.Claims.ExpiresAt + 5);
		m_State->Admitted.insert(checked.Claims.AccountId);
		return "WELCOME " + checked.Claims.AccountId;
	}

}
