#include "RuntimeMatch.h"

#include "GanymedE/Net/Net.h"
#include "GanymedE/Online/Json.h"
#include "GanymedE/UI/UIEngine.h"

namespace GanymedE {

	namespace {

		// A queue that cannot be placed is retried: the backend may still be signing this
		// profile in, or be briefly down. Ten tries, three seconds apart, then give up.
		constexpr int kQueueTries = 10;
		constexpr float kQueueRetry = 3.0f;
		constexpr float kPoll = 1.0f;
		// JoinMatch already makes three attempts with a fresh token each. Three of those.
		constexpr int kJoinFailures = 3;
		// After the server has gone, the result is the backend's to write. It forwards the
		// server's report at once; a server that died is `server_lost` within about 5 s.
		constexpr float kResultWait = 60.0f;

		std::string Text(const JsonValue* value)
		{
			return value && value->Kind == JsonValue::Type::String ? value->String : std::string();
		}

		std::string Problem(const OnlineResponse& response)
		{
			if (!response.Arrived())
				return response.TransportError;
			std::optional<JsonValue> body = ParseJson(response.Body);
			std::string type = body ? Text(body->Find("type")) : std::string();
			return "status " + std::to_string(response.Status) + (type.empty() ? "" : " " + type);
		}

	}

	RuntimeMatch::RuntimeMatch(std::string mode)
		: m_Mode(std::move(mode))
	{
		GE_INFO("Match: queueing for '{0}' as profile '{1}'", m_Mode, Online::GetProfile());
		Show("Looking for a " + m_Mode + " match...");
	}

	RuntimeMatch::~RuntimeMatch()
	{
		if (m_Request)
			Online::Cancel(m_Request);
	}

	void RuntimeMatch::OnUpdate(float ts)
	{
		m_Wait -= ts;
		m_Elapsed += ts;

		switch (m_State)
		{
			case State::Queue:
				if (!m_Request && m_Wait <= 0.0f)
					SendQueue();
				break;

			case State::Wait:
				if (!m_Request && m_Wait <= 0.0f)
					ReadTicket();
				break;

			case State::Join:
				break;

			case State::Play:
			{
				// The session ends when the server stops talking: it exits once it has reported
				// the result. 5 s of silence is what the client counts as gone (netcode.md).
				const Net::Stats stats = Net::GetStats();
				if (stats.Connected)
				{
					if (!m_SeenConnected)
						Show("");
					m_SeenConnected = true;
				}
				else if (m_SeenConnected)
				{
					GE_INFO("Match: the server has gone; reading the result");
					Show("Match over - reading the result...");
					m_State = State::Result;
					m_Elapsed = 0.0f;
					m_Wait = 0.0f;
				}
				break;
			}

			case State::Result:
				if (m_Elapsed > kResultWait)
				{
					Finish("no result on the ticket " + std::to_string(static_cast<int>(kResultWait)) + " s after the server went",
						"Match over - no result came back");
					break;
				}
				if (!m_Request && m_Wait <= 0.0f)
					ReadTicket();
				break;

			case State::Done:
				break;
		}
	}

	void RuntimeMatch::SendQueue()
	{
		m_QueueTries++;
		JsonValue body = JsonValue::MakeObject();
		body.Members.emplace_back("mode", JsonValue::MakeString(m_Mode));
		OnlineRequest request;
		request.Method = "POST";
		request.Path = "/v1/matchmaking/tickets";
		WriteJson(body, request.Body);

		m_Request = Online::Send(std::move(request), [this](const OnlineResponse& response)
		{
			m_Request = 0;
			if (response.IsSuccess())
			{
				std::optional<JsonValue> ticket = ParseJson(response.Body);
				GE_INFO("Match: queued (ticket {0})", ticket ? Text(ticket->Find("ticket_id")) : "?");
				m_State = State::Wait;
				m_Wait = 0.0f;
				return;
			}
			const std::string problem = Problem(response);
			// Already in a ticket: one that is queued, or a match that has not finished. Follow it.
			if (problem.find("already-queued") != std::string::npos)
			{
				GE_INFO("Match: this profile already has an active ticket; following it");
				m_State = State::Wait;
				m_Wait = 0.0f;
				return;
			}
			if (m_QueueTries >= kQueueTries)
			{
				Finish("could not queue: " + problem, "Could not queue for a match");
				return;
			}
			GE_WARN("Match: queue failed ({0}); trying again in {1:.0f} s", problem, kQueueRetry);
			Show("Cannot reach the matchmaker - trying again...");
			m_Wait = kQueueRetry;
		});
	}

	void RuntimeMatch::ReadTicket()
	{
		OnlineRequest request;
		request.Path = "/v1/matchmaking/ticket";
		m_Request = Online::Send(std::move(request), [this](const OnlineResponse& response)
		{
			m_Request = 0;
			m_Wait = kPoll;
			OnTicket(response);
		});
	}

	void RuntimeMatch::OnTicket(const OnlineResponse& response)
	{
		if (!response.IsSuccess())
		{
			GE_WARN("Match: reading the ticket failed ({0})", Problem(response));
			return;
		}
		std::optional<JsonValue> body = ParseJson(response.Body);
		const JsonValue* ticket = body ? body->Find("ticket") : nullptr;
		if (!ticket || ticket->Kind != JsonValue::Type::Object)
		{
			if (m_State == State::Result)
				return;
			GE_WARN("Match: no ticket; queueing again");
			m_State = State::Queue;
			return;
		}

		const std::string state = Text(ticket->Find("state"));
		if (state != m_LastState)
		{
			m_LastState = state;
			GE_INFO("Match: ticket {0}", state);
			if (m_State == State::Wait)
			{
				if (state == "queued")
					Show("In the queue - waiting for players...");
				else if (state == "matched" || state == "allocating")
					Show("Match found - starting a server...");
				else if (state == "ready")
					Show("Joining the match...");
			}
		}

		if (state == "finished")
		{
			const JsonValue* result = ticket->Find("result");
			const JsonValue* change = result ? result->Find("rating_change") : nullptr;
			const int64_t delta = change && change->Kind == JsonValue::Type::Integer ? change->Integer : 0;
			const std::string outcome = result ? Text(result->Find("outcome")) : "?";
			const std::string rating = std::string("rating ") + (delta >= 0 ? "+" : "") + std::to_string(delta);
			std::string shown = outcome == "victory" ? "VICTORY" : outcome == "defeat" ? "DEFEAT" : outcome;
			Finish("finished: " + outcome + ", " + rating, shown + " - " + rating);
			return;
		}
		if (state == "failed" || state == "cancelled")
		{
			const std::string reason = Text(ticket->Find("failure_reason"));
			Finish(state + (reason.empty() ? "" : ": " + reason),
				state == "cancelled" ? "Matchmaking cancelled" : "No match" + (reason.empty() ? "" : " (" + reason + ")"));
			return;
		}
		if (state != "ready" || m_State != State::Wait)
			return;

		// Ready: join. JoinMatch reads the ticket again itself, for a fresh token per attempt.
		m_State = State::Join;
		m_Request = Online::JoinMatch([this](const OnlineResponse& joined)
		{
			m_Request = 0;
			if (joined.IsSuccess())
			{
				std::optional<JsonValue> who = ParseJson(joined.Body);
				GE_INFO("Match: joined as {0}", who ? Text(who->Find("account_id")) : "?");
				Show("Joined - waiting for the server...");
				m_State = State::Play;
				m_Elapsed = 0.0f;
				return;
			}
			m_JoinFailures++;
			if (m_JoinFailures >= kJoinFailures)
			{
				Finish("could not join: " + joined.TransportError, "Could not join the match");
				return;
			}
			GE_WARN("Match: join failed ({0}); reading the ticket again", joined.TransportError);
			m_State = State::Wait;
			m_Wait = kPoll;
		});
	}

	void RuntimeMatch::Finish(const std::string& why, const std::string& shown)
	{
		GE_INFO("Match: {0}", why);
		Show(shown);
		m_State = State::Done;
	}

	void RuntimeMatch::Show(const std::string& text)
	{
		if (text == m_Shown)
			return;
		m_Shown = text;
		// Headless there is no UI at all, and nothing to warn about.
		if (!UIEngine::SetElementText("match-status", text) && UIEngine::IsInitialized() && !m_WarnedNoStatus)
		{
			m_WarnedNoStatus = true;
			GE_WARN("Match: the HUD has no #match-status element; the match status is in the log only");
		}
	}

}
