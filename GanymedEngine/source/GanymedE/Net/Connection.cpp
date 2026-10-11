#include "gepch.h"
#include "GanymedE/Net/Connection.h"

#include <cstring>

namespace GanymedE::Net {

	namespace {

		void WriteU16(std::vector<uint8_t>& out, uint16_t value)
		{
			out.push_back(static_cast<uint8_t>(value));
			out.push_back(static_cast<uint8_t>(value >> 8));
		}

		void WriteU32(std::vector<uint8_t>& out, uint32_t value)
		{
			out.push_back(static_cast<uint8_t>(value));
			out.push_back(static_cast<uint8_t>(value >> 8));
			out.push_back(static_cast<uint8_t>(value >> 16));
			out.push_back(static_cast<uint8_t>(value >> 24));
		}

		void WriteU64(std::vector<uint8_t>& out, uint64_t value)
		{
			for (int i = 0; i < 8; i++)
				out.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}

		bool ReadU16(const uint8_t* data, int length, int& cursor, uint16_t& out)
		{
			if (cursor + 2 > length)
				return false;
			out = static_cast<uint16_t>(data[cursor] | (data[cursor + 1] << 8));
			cursor += 2;
			return true;
		}

		bool ReadU32(const uint8_t* data, int length, int& cursor, uint32_t& out)
		{
			if (cursor + 4 > length)
				return false;
			out = static_cast<uint32_t>(data[cursor])
				| (static_cast<uint32_t>(data[cursor + 1]) << 8)
				| (static_cast<uint32_t>(data[cursor + 2]) << 16)
				| (static_cast<uint32_t>(data[cursor + 3]) << 24);
			cursor += 4;
			return true;
		}

		bool ReadU64(const uint8_t* data, int length, int& cursor, uint64_t& out)
		{
			if (cursor + 8 > length)
				return false;
			out = 0;
			for (int i = 0; i < 8; i++)
				out |= static_cast<uint64_t>(data[cursor + i]) << (8 * i);
			cursor += 8;
			return true;
		}

		// Positive when `a` is newer than `b`, across the uint16 wrap.
		int SeqDiff(uint16_t a, uint16_t b)
		{
			return static_cast<int16_t>(a - b);
		}

	}

	PacketView ReadPacket(const uint8_t* data, int length)
	{
		PacketView view;
		if (!data || length < kHeaderBytes)
			return view;

		int cursor = 0;
		uint32_t protocol = 0;
		if (!ReadU32(data, length, cursor, protocol) || protocol != kProtocolId)
			return view;
		if (!ReadU64(data, length, cursor, view.Session))
			return view;
		if (!ReadU16(data, length, cursor, view.Sequence) || !ReadU16(data, length, cursor, view.Ack))
			return view;
		if (!ReadU32(data, length, cursor, view.AckBits))
			return view;
		view.Kind = data[cursor++];

		if (view.Kind == kKindRequest)
		{
			if (cursor >= length)
				return view;
			const int accountLen = data[cursor++];
			if (accountLen <= 0 || cursor + accountLen > length)
				return view;
			view.Account.assign(reinterpret_cast<const char*>(data + cursor), static_cast<size_t>(accountLen));
		}

		view.Ok = view.Kind == kKindRequest || view.Kind == kKindAccept
			|| view.Kind == kKindData || view.Kind == kKindSnapshot || view.Kind == kKindCommand;
		return view;
	}

	Connection::Connection(uint64_t session, Address remote, std::string account)
		: m_Session(session), m_Remote(remote), m_Account(std::move(account))
	{
		const auto now = Clock::now();
		m_LastSend = now;
		m_LastRecv = now;
	}

	void Connection::Enqueue(std::vector<uint8_t> payload)
	{
		if (payload.empty() || payload.size() > 256)
			return;
		Outgoing message;
		message.Id = m_NextReliable++;
		if (m_NextReliable == 0)
			m_NextReliable = 1;
		message.Bytes = std::move(payload);
		m_Outgoing.push_back(std::move(message));
	}

	bool Connection::Build(std::vector<uint8_t>& out, uint8_t kind, const std::string* account, Clock::time_point now, bool force)
	{
		const bool due = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastSend).count() >= kKeepaliveMs;
		const bool hasReliable = kind == kKindData && !m_Outgoing.empty();
		// A session request is resent on the keepalive, not every pump. Data goes out
		// as soon as a reliable message is waiting. An ack goes out on the pump that
		// noticed the peer's packet, which is what keeps the RTT sample inside a frame
		// of the conditioner's delay. The keepalive is the floor when nothing arrives.
		if (!force && !due && !hasReliable && !m_AckPending)
			return false;

		if (++m_LocalSeq == 0)
			m_LocalSeq = 1;

		out.clear();
		WriteU32(out, kProtocolId);
		WriteU64(out, m_Session);
		WriteU16(out, m_LocalSeq);
		WriteU16(out, m_HaveRecv ? m_RecvAck : 0);
		WriteU32(out, m_HaveRecv ? m_RecvBits : 0);
		out.push_back(kind);

		Sent sent;
		sent.Sequence = m_LocalSeq;
		sent.At = now;

		if (kind == kKindRequest)
		{
			const std::string empty;
			const std::string& name = account ? *account : empty;
			const int length = static_cast<int>(name.size());
			out.push_back(static_cast<uint8_t>(length > 255 ? 255 : length));
			out.insert(out.end(), name.begin(), name.begin() + (length > 255 ? 255 : length));
		}
		else if (kind == kKindData)
		{
			const size_t countAt = out.size();
			out.push_back(0);
			int count = 0;
			for (Outgoing& message : m_Outgoing)
			{
				if (message.Bytes.empty())
					continue;
				const int entry = 4 + static_cast<int>(message.Bytes.size());
				if (static_cast<int>(out.size()) + entry > kMaxDatagram)
					break;
				WriteU16(out, message.Id);
				WriteU16(out, static_cast<uint16_t>(message.Bytes.size()));
				out.insert(out.end(), message.Bytes.begin(), message.Bytes.end());
				if (message.SentOnce)
					sent.Resent = true;
				message.SentOnce = true;
				sent.ReliableIds.push_back(message.Id);
				count++;
			}
			out[countAt] = static_cast<uint8_t>(count > 255 ? 255 : count);
		}

		if (sent.Resent)
			m_Resent++;
		m_AckPending = false;
		m_Sent.push_back(std::move(sent));
		if (m_Sent.size() > 128)
			m_Sent.erase(m_Sent.begin());
		m_LastSend = now;
		AddBytes(m_BytesOut, now, static_cast<int>(out.size()));
		return true;
	}

	uint16_t Connection::BuildSnapshot(std::vector<uint8_t>& out, const uint8_t* payload, int payloadLength, Clock::time_point now, uint8_t kind)
	{
		if (!payload || payloadLength < 0 || kHeaderBytes + payloadLength > kMaxDatagram)
			return 0;

		if (++m_LocalSeq == 0)
			m_LocalSeq = 1;

		out.clear();
		WriteU32(out, kProtocolId);
		WriteU64(out, m_Session);
		WriteU16(out, m_LocalSeq);
		WriteU16(out, m_HaveRecv ? m_RecvAck : 0);
		WriteU32(out, m_HaveRecv ? m_RecvBits : 0);
		out.push_back(kind);
		out.insert(out.end(), payload, payload + payloadLength);

		Sent sent;
		sent.Sequence = m_LocalSeq;
		sent.At = now;
		m_AckPending = false;
		m_Sent.push_back(std::move(sent));
		if (m_Sent.size() > 128)
			m_Sent.erase(m_Sent.begin());
		m_LastSend = now;
		AddBytes(m_BytesOut, now, static_cast<int>(out.size()));
		return m_LocalSeq;
	}

	bool Connection::IsAcked(uint16_t sequence) const
	{
		if (sequence == 0)
			return false;
		for (const Sent& sent : m_Sent)
		{
			if (sent.Sequence == sequence && sent.Acked)
				return true;
		}
		return false;
	}

	void Connection::Receive(const uint8_t* data, int length, Clock::time_point now,
		const std::function<void(const uint8_t*, int)>& deliver)
	{
		const PacketView view = ReadPacket(data, length);
		if (!view.Ok)
			return;

		m_LastRecv = now;
		m_AckPending = true;
		AddBytes(m_BytesIn, now, length);
		NoteSequence(view.Sequence);
		NoteAcks(view.Ack, view.AckBits, now);

		if (view.Kind != kKindData || length < kHeaderBytes + 1)
			return;

		int cursor = kHeaderBytes;
		const int count = data[cursor++];
		for (int i = 0; i < count; i++)
		{
			uint16_t id = 0;
			uint16_t size = 0;
			if (!ReadU16(data, length, cursor, id) || !ReadU16(data, length, cursor, size))
				return;
			if (cursor + size > length)
				return;
			const uint8_t* bytes = data + cursor;
			cursor += size;

			if (id == m_ExpectReliable)
			{
				if (deliver)
					deliver(bytes, size);
				m_ExpectReliable++;
				if (m_ExpectReliable == 0)
					m_ExpectReliable = 1;
				bool advanced = true;
				while (advanced)
				{
					advanced = false;
					for (auto it = m_Held.begin(); it != m_Held.end(); ++it)
					{
						if (it->first == m_ExpectReliable)
						{
							if (deliver)
								deliver(it->second.data(), static_cast<int>(it->second.size()));
							m_ExpectReliable++;
							if (m_ExpectReliable == 0)
								m_ExpectReliable = 1;
							m_Held.erase(it);
							advanced = true;
							break;
						}
					}
				}
			}
			else if (SeqDiff(id, m_ExpectReliable) > 0)
			{
				bool known = false;
				for (const auto& held : m_Held)
				{
					if (held.first == id)
						known = true;
				}
				if (!known)
					m_Held.emplace_back(id, std::vector<uint8_t>(bytes, bytes + size));
			}
		}
	}

	bool Connection::TimedOut(Clock::time_point now) const
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastRecv).count() >= kTimeoutMs;
	}

	float Connection::LossIn() const
	{
		if (m_RecvExpected == 0)
			return 0.0f;
		return 100.0f * static_cast<float>(m_RecvExpected - m_RecvGot) / static_cast<float>(m_RecvExpected);
	}

	float Connection::LossOut() const
	{
		if (m_Resolved == 0)
			return 0.0f;
		return 100.0f * static_cast<float>(m_Lost) / static_cast<float>(m_Resolved);
	}

	float Connection::BytesInPerSec(Clock::time_point now) const { return Rate(m_BytesIn, now); }
	float Connection::BytesOutPerSec(Clock::time_point now) const { return Rate(m_BytesOut, now); }

	void Connection::NoteSequence(uint16_t sequence)
	{
		if (sequence == 0)
			return;

		if (!m_HaveRecv)
		{
			m_RecvAck = sequence;
			m_RecvBits = 0;
			m_HaveRecv = true;
			m_RecvExpected = 1;
			m_RecvGot = 1;
			return;
		}

		const int diff = SeqDiff(sequence, m_RecvAck);
		if (diff > 0)
		{
			m_RecvExpected += static_cast<uint32_t>(diff);
			m_RecvGot += 1;
			if (diff >= 32)
				m_RecvBits = 0;
			else
				m_RecvBits = (m_RecvBits << diff) | (1u << (diff - 1));
			m_RecvAck = sequence;
		}
		else if (diff < 0 && diff >= -32)
		{
			// A packet that arrived late fills a gap the jump ahead already charged
			// to loss. Reordering is not loss.
			const uint32_t bit = 1u << (-diff - 1);
			if ((m_RecvBits & bit) == 0)
			{
				m_RecvBits |= bit;
				m_RecvGot++;
			}
		}
	}

	void Connection::NoteAcks(uint16_t ack, uint32_t bits, Clock::time_point now)
	{
		if (ack == 0)
			return;

		auto mark = [&](uint16_t sequence, bool sampleRtt)
		{
			for (Sent& sent : m_Sent)
			{
				if (sent.Sequence != sequence || sent.Acked)
					continue;
				sent.Acked = true;
				sent.Resolved = true;
				m_Resolved++;
				if (sampleRtt)
				{
					const float sample = std::chrono::duration<float, std::milli>(now - sent.At).count();
					if (!m_HaveRtt)
					{
						m_RttMs = sample;
						m_HaveRtt = true;
					}
					else
						m_RttMs += (sample - m_RttMs) / 8.0f;
				}
				for (uint16_t id : sent.ReliableIds)
				{
					for (auto it = m_Outgoing.begin(); it != m_Outgoing.end(); ++it)
					{
						if (it->Id == id)
						{
							m_Outgoing.erase(it);
							break;
						}
					}
				}
				break;
			}
		};

		mark(ack, true);
		for (int i = 0; i < 32; i++)
		{
			if (bits & (1u << i))
				mark(static_cast<uint16_t>(ack - 1 - i), false);
		}
		Resolve(ack);
	}

	void Connection::Resolve(uint16_t newestAck)
	{
		for (Sent& sent : m_Sent)
		{
			if (sent.Resolved || sent.Sequence == 0)
				continue;
			if (SeqDiff(newestAck, sent.Sequence) > 32)
			{
				sent.Resolved = true;
				m_Resolved++;
				if (!sent.Acked)
					m_Lost++;
			}
		}
	}

	void Connection::AddBytes(std::vector<std::pair<Clock::time_point, int>>& samples, Clock::time_point now, int bytes)
	{
		samples.emplace_back(now, bytes);
		const auto cutoff = now - std::chrono::seconds(1);
		while (!samples.empty() && samples.front().first < cutoff)
			samples.erase(samples.begin());
	}

	float Connection::Rate(const std::vector<std::pair<Clock::time_point, int>>& samples, Clock::time_point now) const
	{
		const auto cutoff = now - std::chrono::seconds(1);
		int total = 0;
		for (const auto& sample : samples)
		{
			if (sample.first >= cutoff)
				total += sample.second;
		}
		return static_cast<float>(total);
	}

}
