#pragma once

#include "GanymedE/Net/Socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace GanymedE::Net {

	// Every game datagram after the text handshake. Little-endian, so a Windows
	// server and a Linux one agree without a byte swap at the call site.
	//
	//   u32 protocol   0x47453201
	//   u64 session    0 on the client's first packet, then the id the server assigned
	//   u16 sequence   wraps; 0 means "I have sent nothing"
	//   u16 ack        highest sequence received; 0 means "I have received nothing"
	//   u32 ackBits    bit i set => (ack - 1 - i) was received
	//   u8  kind       1 session request, 2 session accept, 3 data, 4 snapshot
	//
	// A session request carries the account id (one length byte, then the bytes).
	// A data packet carries the reliable messages still waiting for an ack.
	constexpr uint32_t kProtocolId = 0x47453201u;
	constexpr uint8_t kKindRequest = 1;
	constexpr uint8_t kKindAccept = 2;
	constexpr uint8_t kKindData = 3;
	constexpr uint8_t kKindSnapshot = 4;
	constexpr int kHeaderBytes = 21;
	constexpr int kMaxDatagram = 1200;
	constexpr int kKeepaliveMs = 100;
	constexpr int kTimeoutMs = 5000;

	struct PacketView
	{
		uint64_t Session = 0;
		uint16_t Sequence = 0;
		uint16_t Ack = 0;
		uint32_t AckBits = 0;
		uint8_t Kind = 0;
		std::string Account;
		bool Ok = false;
	};

	PacketView ReadPacket(const uint8_t* data, int length);

	// One peer. The same type runs on the client and on the server: each side has its
	// own sequence space, and each side's ack bitfield is about the other's.
	class Connection
	{
	public:
		using Clock = std::chrono::steady_clock;

		Connection(uint64_t session, Address remote, std::string account);

		uint64_t Session() const { return m_Session; }
		void SetSession(uint64_t session) { m_Session = session; }
		const Address& Remote() const { return m_Remote; }
		void SetRemote(const Address& remote) { m_Remote = remote; }
		const std::string& Account() const { return m_Account; }

		void Enqueue(std::vector<uint8_t> payload);

		// False when there is nothing to say and the keepalive interval has not elapsed.
		// `account` is written only for a session request.
		bool Build(std::vector<uint8_t>& out, uint8_t kind, const std::string* account, Clock::time_point now, bool force);

		// Unreliable snapshot. Always consumes a sequence. The payload is the body after
		// the 21-byte header and must already fit in the 1200-byte cap. Returns the
		// sequence, or 0 when the payload does not fit.
		uint16_t BuildSnapshot(std::vector<uint8_t>& out, const uint8_t* payload, int payloadLength, Clock::time_point now);

		// True when `sequence` is still in the send window and the peer has acknowledged it.
		bool IsAcked(uint16_t sequence) const;

		// Records the peer's sequence and which of our packets they have seen, then
		// delivers reliable payloads that are now in order. Malformed reliable bytes
		// are skipped; the header still counts.
		void Receive(const uint8_t* data, int length, Clock::time_point now,
			const std::function<void(const uint8_t*, int)>& deliver);

		bool TimedOut(Clock::time_point now) const;
		void MarkAlive(Clock::time_point now) { m_LastRecv = now; }

		float RttMs() const { return m_RttMs; }
		float LossIn() const;
		float LossOut() const;
		float BytesInPerSec(Clock::time_point now) const;
		float BytesOutPerSec(Clock::time_point now) const;
		uint64_t Resent() const { return m_Resent; }

	private:
		struct Sent
		{
			uint16_t Sequence = 0;
			Clock::time_point At{};
			bool Acked = false;
			bool Resolved = false;
			bool Resent = false;
			std::vector<uint16_t> ReliableIds;
		};

		struct Outgoing
		{
			uint16_t Id = 0;
			std::vector<uint8_t> Bytes;
			bool SentOnce = false;
		};

		void NoteSequence(uint16_t sequence);
		void NoteAcks(uint16_t ack, uint32_t bits, Clock::time_point now);
		void Resolve(uint16_t newestAck);
		void AddBytes(std::vector<std::pair<Clock::time_point, int>>& samples, Clock::time_point now, int bytes);
		float Rate(const std::vector<std::pair<Clock::time_point, int>>& samples, Clock::time_point now) const;

		uint64_t m_Session = 0;
		Address m_Remote{};
		std::string m_Account;

		uint16_t m_LocalSeq = 0;
		uint16_t m_RecvAck = 0;
		uint32_t m_RecvBits = 0;
		bool m_HaveRecv = false;
		// Set when a packet arrives, cleared when one goes out. The ack rides the next
		// pump instead of waiting out the keepalive, so a 50 ms delay each way measures
		// near 100 ms rather than 100 ms plus half the keepalive. The peer only sends
		// again when it hears this, so the idle rate is one packet per round trip,
		// and never slower than the keepalive.
		bool m_AckPending = false;

		std::vector<Sent> m_Sent;
		std::vector<Outgoing> m_Outgoing;
		uint16_t m_NextReliable = 1;
		uint16_t m_ExpectReliable = 1;
		std::vector<std::pair<uint16_t, std::vector<uint8_t>>> m_Held;

		Clock::time_point m_LastSend{};
		Clock::time_point m_LastRecv{};
		bool m_HaveRtt = false;
		float m_RttMs = 0.0f;

		uint32_t m_Resolved = 0;
		uint32_t m_Lost = 0;
		uint32_t m_RecvExpected = 0;
		uint32_t m_RecvGot = 0;
		uint64_t m_Resent = 0;

		std::vector<std::pair<Clock::time_point, int>> m_BytesIn;
		std::vector<std::pair<Clock::time_point, int>> m_BytesOut;
	};

}
