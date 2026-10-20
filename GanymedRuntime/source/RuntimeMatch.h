#pragma once

#include <GanymedE.h>

#include "GanymedE/Online/Online.h"

namespace GanymedE {

	// `--queue=<mode>`: a player's whole match with no menu. Queues this profile through the
	// backend, follows its ticket to `ready`, joins the server (Online::JoinMatch, which hands its
	// socket to the session), and once the server has gone reads the ticket for the result.
	//
	// The scene is already a client scene when this starts (RuntimeLayer builds it as one, as for
	// --connect), so joining only attaches the session; nothing is reloaded mid-match. The ticket is
	// the state of record and is polled once a second. The pushes are nudges (docs/engine/online.md)
	// and a script may be listening to them; this does not take the push handler.
	//
	// What it is doing is also on screen, in the HUD's `#match-status` element if it has one
	// (UIEngine::SetElementText): the queue, the join, and at the end the outcome. It is cleared
	// while the match is being played.
	class RuntimeMatch
	{
	public:
		explicit RuntimeMatch(std::string mode);
		~RuntimeMatch();

		// Once a frame, after Online's completions for the frame have run.
		void OnUpdate(float ts);

		// Finished, one way or another. The outcome is in the log.
		bool Done() const { return m_State == State::Done; }

	private:
		enum class State { Queue, Wait, Join, Play, Result, Done };

		void SendQueue();
		void ReadTicket();
		void OnTicket(const OnlineResponse& response);
		void Finish(const std::string& why, const std::string& shown);
		void Show(const std::string& text);

		std::string m_Mode;
		State m_State = State::Queue;
		RequestId m_Request = 0;      // the one request in flight, 0 for none
		float m_Wait = 0.0f;          // seconds until the next request
		float m_Elapsed = 0.0f;       // in the current state, for the timeouts
		int m_QueueTries = 0;
		int m_JoinFailures = 0;
		bool m_SeenConnected = false;
		std::string m_LastState;
		std::string m_Shown;
		bool m_WarnedNoStatus = false;
	};

}
