#include "gepch.h"
#include "GanymedE/Net/Net.h"

#include "GanymedE/Core/Log.h"
#include "GanymedE/Net/Connection.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <unordered_map>

namespace GanymedE::Net {

	namespace {

		Sim g_Sim;

		bool Chance(std::mt19937& rng, int percent)
		{
			if (percent <= 0)
				return false;
			if (percent >= 100)
				return true;
			std::uniform_int_distribution<int> roll(0, 99);
			return roll(rng) < percent;
		}

		// Holds a datagram until its release time, then the socket sends it. Loss
		// drops it here, so the connection has already counted it as sent: the
		// measured loss is the simulated loss, not a hole in the sequence space
		// the sender pretended not to use.
		class Conditioner
		{
		public:
			explicit Conditioner(Sim sim) : m_Sim(sim), m_Rng(std::random_device{}()) {}

			void Submit(const Address& to, std::vector<uint8_t> bytes)
			{
				if (bytes.empty())
					return;
				if (Chance(m_Rng, m_Sim.LossPercent))
					return;

				const auto now = Connection::Clock::now();
				int jitter = 0;
				if (m_Sim.JitterMs > 0)
				{
					std::uniform_int_distribution<int> extra(0, m_Sim.JitterMs);
					jitter = extra(m_Rng);
				}
				auto due = now + std::chrono::milliseconds(m_Sim.LatencyMs + jitter);
				if (Chance(m_Rng, m_Sim.ReorderPercent))
					due += std::chrono::milliseconds(std::max(50, m_Sim.LatencyMs));

				m_Queue.push_back({ due, to, bytes });
				if (Chance(m_Rng, m_Sim.DuplicatePercent))
					m_Queue.push_back({ due + std::chrono::milliseconds(10), to, bytes });
			}

			void Flush(Socket& socket, Connection::Clock::time_point now)
			{
				std::vector<Item> kept;
				kept.reserve(m_Queue.size());
				for (Item& item : m_Queue)
				{
					if (item.Due <= now)
						socket.Send(item.To, item.Bytes.data(), static_cast<int>(item.Bytes.size()));
					else
						kept.push_back(std::move(item));
				}
				m_Queue.swap(kept);
			}

		private:
			struct Item
			{
				Connection::Clock::time_point Due{};
				Address To{};
				std::vector<uint8_t> Bytes;
			};

			Sim m_Sim;
			std::mt19937 m_Rng;
			std::vector<Item> m_Queue;
		};

		struct Peer
		{
			std::unique_ptr<Connection> Conn;
			uint32_t Probe = 0;
			bool ProbeBad = false;
		};

		struct Engine
		{
			// Recursive: the door holds it across the text handler, and that handler calls
			// NoteAdmitted. Not held across select. The main thread takes it to read acks
			// and to queue a snapshot. The client queue below is main-thread only.
			std::recursive_mutex StateLock;
			std::mutex PublishLock;
			Stats Published;

			struct Incoming
			{
				std::vector<uint8_t> Bytes;
				float RttMs = 0.0f;
				Connection::Clock::time_point At{};
				uint64_t Session = 0;
				uint16_t Sequence = 0;
			};
			std::vector<Incoming> IncomingSnapshots;

			struct CommandIn
			{
				std::string Account;
				std::vector<uint8_t> Bytes;
			};
			std::vector<CommandIn> IncomingCommands;

			bool Serving = false;
			Socket ServerSocket;
			std::unique_ptr<Conditioner> ServerBox;
			std::vector<Peer> Peers;
			std::unordered_map<std::string, Address> Admitted;
			std::function<std::string(const Address&, const std::string&)> OnText;
			uint64_t DroppedSession = 0;
			uint64_t DroppedAddress = 0;
			Connection::Clock::time_point NextLog{};

			bool Client = false;
			enum class Phase { Hello, Session, Live, Dead } ClientPhase = Phase::Dead;
			std::unique_ptr<Socket> ClientSocket;
			std::unique_ptr<Conditioner> ClientBox;
			std::unique_ptr<Connection> ClientConn;
			Address ServerAddress{};
			std::string DevName;
			int HelloAttempts = 0;
			Connection::Clock::time_point HelloSent{};
			bool HelloRefused = false;
			int ReliableTotal = 0;
			bool Spoof = false;
			bool SpoofSent = false;
			bool Flood = false;
			bool FloodSent = false;
			uint32_t EchoExpect = 0;
			bool EchoBad = false;
			bool EchoLogged = false;
			Connection::Clock::time_point ClientLog{};
		};

		Engine& State()
		{
			static Engine engine;
			return engine;
		}

		std::unique_lock<std::recursive_mutex> LockState()
		{
			return std::unique_lock<std::recursive_mutex>(State().StateLock);
		}

		std::string Hex(uint64_t value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
			return buffer;
		}

		uint64_t NewSession()
		{
			std::random_device random;
			uint64_t id = (static_cast<uint64_t>(random()) << 32) ^ random();
			return id == 0 ? 1 : id;
		}

		uint32_t ReadU32(const uint8_t* data)
		{
			return static_cast<uint32_t>(data[0])
				| (static_cast<uint32_t>(data[1]) << 8)
				| (static_cast<uint32_t>(data[2]) << 16)
				| (static_cast<uint32_t>(data[3]) << 24);
		}

		void WriteU32(std::vector<uint8_t>& out, uint32_t value)
		{
			out.push_back(static_cast<uint8_t>(value));
			out.push_back(static_cast<uint8_t>(value >> 8));
			out.push_back(static_cast<uint8_t>(value >> 16));
			out.push_back(static_cast<uint8_t>(value >> 24));
		}

		bool IsHello(const uint8_t* data, int length)
		{
			return length >= 6 && std::memcmp(data, "HELLO ", 6) == 0;
		}

		void Publish(Engine& engine, const Stats& stats)
		{
			std::lock_guard<std::mutex> lock(engine.PublishLock);
			engine.Published = stats;
		}

		void LogStats(const std::string& who, const Connection& connection, Connection::Clock::time_point now,
			uint64_t droppedSession, uint64_t droppedAddress)
		{
			GE_CORE_INFO("Net {0}: rtt {1:.0f} ms, loss in {2:.1f}% out {3:.1f}%, {4:.0f} B/s in, {5:.0f} B/s out, resent {6}, dropped session {7}, address {8}, reliable refused {9}",
				who, connection.RttMs(), connection.LossIn(), connection.LossOut(),
				connection.BytesInPerSec(now), connection.BytesOutPerSec(now), connection.Resent(),
				droppedSession, droppedAddress, connection.ReliableRefused());
		}

		Stats FromConnection(const Connection& connection, Connection::Clock::time_point now, bool connected,
			uint64_t droppedSession, uint64_t droppedAddress)
		{
			Stats stats;
			stats.Connected = connected;
			stats.Peers = connected ? 1 : 0;
			stats.RttMs = connection.RttMs();
			stats.LossIn = connection.LossIn();
			stats.LossOut = connection.LossOut();
			stats.BytesInPerSec = connection.BytesInPerSec(now);
			stats.BytesOutPerSec = connection.BytesOutPerSec(now);
			stats.Resent = connection.Resent();
			stats.DroppedSession = droppedSession;
			stats.DroppedAddress = droppedAddress;
			return stats;
		}

		void DeliverServer(Engine& engine, Peer& peer, const uint8_t* data, int length)
		{
			// Byte 3 is a server-to-client script event. A client that sends one is ignored,
			// the same as any payload that is not the probe.
			if (length != 5 || data[0] != 1)
				return;
			const uint32_t index = ReadU32(data + 1);
			if (peer.ProbeBad)
				return;
			if (index != peer.Probe)
			{
				peer.ProbeBad = true;
				GE_CORE_ERROR("Reliable probe from {0} arrived out of order at {1}, expected {2}",
					peer.Conn->Account(), index, peer.Probe);
				return;
			}
			peer.Probe++;
			std::vector<uint8_t> echo;
			echo.push_back(2);
			WriteU32(echo, index);
			peer.Conn->Enqueue(std::move(echo));
		}

		void DeliverClient(Engine& engine, const uint8_t* data, int length)
		{
			if (length >= 1 && data[0] == 3)
			{
				IngestReliable(data, length);
				return;
			}
			if (length != 5 || data[0] != 2 || engine.ReliableTotal <= 0)
				return;
			const uint32_t index = ReadU32(data + 1);
			if (engine.EchoBad || engine.EchoLogged)
				return;
			if (index != engine.EchoExpect)
			{
				engine.EchoBad = true;
				GE_CORE_ERROR("Reliable echo arrived out of order at {0}, expected {1}", index, engine.EchoExpect);
				return;
			}
			engine.EchoExpect++;
			if (engine.EchoExpect == static_cast<uint32_t>(engine.ReliableTotal))
			{
				engine.EchoLogged = true;
				GE_CORE_INFO("Reliable test ok: {0} messages, in order, exactly once", engine.ReliableTotal);
			}
		}

		std::vector<uint8_t> BareData(uint64_t session)
		{
			std::vector<uint8_t> out;
			auto u16 = [&](uint16_t value)
			{
				out.push_back(static_cast<uint8_t>(value));
				out.push_back(static_cast<uint8_t>(value >> 8));
			};
			auto u32 = [&](uint32_t value)
			{
				out.push_back(static_cast<uint8_t>(value));
				out.push_back(static_cast<uint8_t>(value >> 8));
				out.push_back(static_cast<uint8_t>(value >> 16));
				out.push_back(static_cast<uint8_t>(value >> 24));
			};
			u32(kProtocolId);
			for (int i = 0; i < 8; i++)
				out.push_back(static_cast<uint8_t>(session >> (8 * i)));
			u16(1);
			u16(0);
			u32(0);
			out.push_back(kKindData);
			out.push_back(0);
			return out;
		}

		void SendSpoof(Engine& engine)
		{
			if (!engine.ClientConn || !engine.ClientSocket)
				return;

			// Not through the connection and not through the conditioner: a dropped
			// probe would be the loss sim, and the check is that the server counted
			// the two kinds of packet it refuses.
			const std::vector<uint8_t> wrong = BareData(engine.ClientConn->Session() + 1);
			engine.ClientSocket->Send(engine.ServerAddress, wrong.data(), static_cast<int>(wrong.size()));

			Socket other;
			std::string error;
			if (!other.OpenClient(error))
			{
				GE_CORE_WARN("Spoof probe could not open a second socket ({0})", error);
				return;
			}
			const std::vector<uint8_t> otherPort = BareData(engine.ClientConn->Session());
			other.Send(engine.ServerAddress, otherPort.data(), static_cast<int>(otherPort.size()));
			GE_CORE_INFO("Spoof probes sent: one wrong session, one from another port");
		}

		// `reliable-flood`: four data packets of 255 reliable messages each, every id far
		// past anything the server expects. A server that held them would grow without limit;
		// it must refuse them all (`reliable refused` in its stats line) and hold none.
		void SendFlood(Engine& engine)
		{
			if (!engine.ClientConn || !engine.ClientSocket)
				return;
			for (int packet = 0; packet < 4; packet++)
			{
				std::vector<uint8_t> out = BareData(engine.ClientConn->Session());
				out.back() = 255;   // the reliable count BareData left at 0
				for (int i = 0; i < 255; i++)
				{
					const uint16_t id = static_cast<uint16_t>(10000 + packet * 255 + i);
					out.push_back(static_cast<uint8_t>(id));
					out.push_back(static_cast<uint8_t>(id >> 8));
					out.push_back(1);
					out.push_back(0);
					out.push_back(1);
				}
				engine.ClientSocket->Send(engine.ServerAddress, out.data(), static_cast<int>(out.size()));
			}
			GE_CORE_INFO("Flood probe sent: 1020 reliable messages outside the window");
		}

		void StartProbes(Engine& engine)
		{
			if (engine.ReliableTotal > 0 && engine.ClientConn)
			{
				for (int i = 0; i < engine.ReliableTotal; i++)
				{
					std::vector<uint8_t> payload;
					payload.push_back(1);
					WriteU32(payload, static_cast<uint32_t>(i));
					engine.ClientConn->Enqueue(std::move(payload));
				}
			}
			if (engine.Spoof && !engine.SpoofSent)
			{
				engine.SpoofSent = true;
				SendSpoof(engine);
			}
			if (engine.Flood && !engine.FloodSent)
			{
				engine.FloodSent = true;
				SendFlood(engine);
			}
		}

		void HandleServerPacket(Engine& engine, const uint8_t* data, int length, const Address& from)
		{
			const PacketView view = ReadPacket(data, length);
			if (!view.Ok)
				return;

			const auto now = Connection::Clock::now();
			if (view.Kind == kKindRequest)
			{
				const auto admitted = engine.Admitted.find(view.Account);
				if (admitted == engine.Admitted.end() || admitted->second != from)
				{
					GE_CORE_WARN("Session request from {0} for '{1}' does not match an admission",
						from.Text(), view.Account);
					return;
				}

				Peer* peer = nullptr;
				for (Peer& existing : engine.Peers)
				{
					if (existing.Conn->Account() == view.Account)
						peer = &existing;
				}
				if (!peer)
				{
					Peer created;
					created.Conn = std::make_unique<Connection>(NewSession(), from, view.Account);
					engine.Peers.push_back(std::move(created));
					peer = &engine.Peers.back();
					GE_CORE_INFO("Session {0} for {1} at {2}", Hex(peer->Conn->Session()), view.Account, from.Text());
				}
				else if (peer->Conn->Remote() != from)
				{
					peer->Conn->SetRemote(from);
				}

				peer->Conn->Receive(data, length, now, {});
				std::vector<uint8_t> accept;
				if (peer->Conn->Build(accept, kKindAccept, nullptr, now, true))
					engine.ServerBox->Submit(from, std::move(accept));
				return;
			}

			Peer* peer = nullptr;
			for (Peer& existing : engine.Peers)
			{
				if (existing.Conn->Remote() == from)
					peer = &existing;
			}
			if (!peer)
			{
				engine.DroppedAddress++;
				return;
			}
			if (view.Session != peer->Conn->Session())
			{
				engine.DroppedSession++;
				return;
			}

			peer->Conn->Receive(data, length, now, [&](const uint8_t* payload, int payloadLength)
			{
				DeliverServer(engine, *peer, payload, payloadLength);
			});
			if (view.Kind == kKindCommand && length > kHeaderBytes)
			{
				Engine::CommandIn incoming;
				incoming.Account = peer->Conn->Account();
				incoming.Bytes.assign(data + kHeaderBytes, data + length);
				engine.IncomingCommands.push_back(std::move(incoming));
			}
		}

		void TickServer(Engine& engine, Connection::Clock::time_point now)
		{
			std::vector<std::string> left;
			for (Peer& peer : engine.Peers)
			{
				std::vector<uint8_t> packet;
				if (peer.Conn->Build(packet, kKindData, nullptr, now, false))
					engine.ServerBox->Submit(peer.Conn->Remote(), std::move(packet));
				if (peer.Conn->TimedOut(now))
					left.push_back(peer.Conn->Account());
			}
			for (const std::string& account : left)
			{
				GE_CORE_INFO("Player {0} left (timeout)", account);
				engine.Peers.erase(std::remove_if(engine.Peers.begin(), engine.Peers.end(),
					[&](const Peer& peer) { return peer.Conn->Account() == account; }), engine.Peers.end());
			}

			if (now >= engine.NextLog)
			{
				engine.NextLog = now + std::chrono::seconds(1);
				for (const Peer& peer : engine.Peers)
					LogStats(peer.Conn->Account(), *peer.Conn, now, engine.DroppedSession, engine.DroppedAddress);
				if (engine.Peers.empty() && (engine.DroppedSession || engine.DroppedAddress))
				{
					GE_CORE_INFO("Net: dropped session {0}, address {1}", engine.DroppedSession, engine.DroppedAddress);
				}
			}

			float rtt = 0.0f;
			float lossIn = 0.0f;
			float lossOut = 0.0f;
			float bytesIn = 0.0f;
			float bytesOut = 0.0f;
			uint64_t resent = 0;
			int counted = 0;
			for (const Peer& peer : engine.Peers)
			{
				rtt += peer.Conn->RttMs();
				lossIn += peer.Conn->LossIn();
				lossOut += peer.Conn->LossOut();
				bytesIn += peer.Conn->BytesInPerSec(now);
				bytesOut += peer.Conn->BytesOutPerSec(now);
				resent += peer.Conn->Resent();
				counted++;
			}
			Stats stats;
			stats.Connected = counted > 0;
			stats.Peers = counted;
			if (counted > 0)
			{
				stats.RttMs = rtt / static_cast<float>(counted);
				stats.LossIn = lossIn / static_cast<float>(counted);
				stats.LossOut = lossOut / static_cast<float>(counted);
			}
			stats.BytesInPerSec = bytesIn;
			stats.BytesOutPerSec = bytesOut;
			stats.Resent = resent;
			stats.DroppedSession = engine.DroppedSession;
			stats.DroppedAddress = engine.DroppedAddress;
			Publish(engine, stats);
		}

		void FailClient(Engine& engine, const std::string& why)
		{
			GE_CORE_WARN("Net: {0}", why);
			engine.ClientPhase = Engine::Phase::Dead;
			engine.Client = false;
			engine.ClientConn.reset();
			engine.ClientSocket.reset();
			engine.ClientBox.reset();
			Stats stats;
			Publish(engine, stats);
		}

		void PumpClientHello(Engine& engine, Connection::Clock::time_point now)
		{
			uint8_t buffer[2048];
			for (;;)
			{
				int length = 0;
				Address from;
				std::string error;
				const Socket::Recv result = engine.ClientSocket->Receive(buffer, sizeof(buffer), length, from, error);
				if (result == Socket::Recv::WouldBlock || result == Socket::Recv::Closed)
					break;
				if (result == Socket::Recv::Refused)
				{
					engine.HelloRefused = true;
					break;
				}
				if (result == Socket::Recv::Error)
				{
					GE_CORE_WARN("Net receive failed: {0}", error);
					break;
				}
				if (length <= 0)
					continue;

				std::string text(reinterpret_cast<const char*>(buffer), static_cast<size_t>(length));
				while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
					text.pop_back();
				if (text.rfind("WELCOME ", 0) == 0)
				{
					const std::string account = text.substr(8);
					engine.ClientConn = std::make_unique<Connection>(0, engine.ServerAddress, account);
					engine.ClientConn->MarkAlive(now);
					std::vector<uint8_t> request;
					engine.ClientConn->Build(request, kKindRequest, &account, now, true);
					engine.ClientBox->Submit(engine.ServerAddress, std::move(request));
					engine.ClientPhase = Engine::Phase::Session;
					GE_CORE_INFO("Dev join admitted as {0}", account);
					return;
				}
				if (text.rfind("DENIED ", 0) == 0)
				{
					FailClient(engine, "the server refused the join: " + text.substr(7));
					return;
				}
			}

			if (engine.HelloRefused)
			{
				FailClient(engine, "nothing is listening at " + engine.ServerAddress.Text());
				return;
			}
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - engine.HelloSent).count() < 1000)
				return;
			if (engine.HelloAttempts >= 3)
			{
				FailClient(engine, "no answer from " + engine.ServerAddress.Text() + " after 3 attempts");
				return;
			}
			engine.HelloAttempts++;
			engine.HelloSent = now;
			const std::string hello = "HELLO dev:" + engine.DevName;
			engine.ClientBox->Submit(engine.ServerAddress, std::vector<uint8_t>(hello.begin(), hello.end()));
			GE_CORE_INFO("Dev join attempt {0} got no answer; trying again", engine.HelloAttempts - 1);
		}

		void PumpClientSession(Engine& engine, Connection::Clock::time_point now)
		{
			uint8_t buffer[2048];
			for (;;)
			{
				int length = 0;
				Address from;
				std::string error;
				const Socket::Recv result = engine.ClientSocket->Receive(buffer, sizeof(buffer), length, from, error);
				if (result == Socket::Recv::WouldBlock || result == Socket::Recv::Closed)
					break;
				if (result != Socket::Recv::Packet)
					break;

				const PacketView view = ReadPacket(buffer, length);
				if (!view.Ok || view.Kind != kKindAccept || view.Session == 0)
					continue;
				engine.ClientConn->SetSession(view.Session);
				engine.ClientConn->Receive(buffer, length, now, {});
				engine.ClientPhase = Engine::Phase::Live;
				engine.ClientLog = now + std::chrono::seconds(1);
				GE_CORE_INFO("Session {0} with {1}", Hex(view.Session), engine.ServerAddress.Text());
				StartProbes(engine);
				return;
			}

			if (engine.ClientConn->TimedOut(now))
			{
				FailClient(engine, "server at " + engine.ServerAddress.Text() + " went silent during the session; session ended");
				return;
			}

			std::vector<uint8_t> request;
			const std::string account = engine.ClientConn->Account();
			if (engine.ClientConn->Build(request, kKindRequest, &account, now, false))
				engine.ClientBox->Submit(engine.ServerAddress, std::move(request));
		}

		void PumpClientLive(Engine& engine, Connection::Clock::time_point now)
		{
			uint8_t buffer[2048];
			for (;;)
			{
				int length = 0;
				Address from;
				std::string error;
				const Socket::Recv result = engine.ClientSocket->Receive(buffer, sizeof(buffer), length, from, error);
				if (result == Socket::Recv::WouldBlock || result == Socket::Recv::Closed)
					break;
				if (result != Socket::Recv::Packet)
					break;
				const PacketView view = ReadPacket(buffer, length);
				if (!view.Ok || view.Session != engine.ClientConn->Session())
					continue;
				engine.ClientConn->Receive(buffer, length, now, [&](const uint8_t* payload, int payloadLength)
				{
					DeliverClient(engine, payload, payloadLength);
				});
				if (view.Kind == kKindSnapshot && length > kHeaderBytes)
				{
					Engine::Incoming incoming;
					incoming.Bytes.assign(buffer + kHeaderBytes, buffer + length);
					incoming.At = now;
					incoming.RttMs = engine.ClientConn->RttMs();
					incoming.Session = engine.ClientConn->Session();
					incoming.Sequence = view.Sequence;
					engine.IncomingSnapshots.push_back(std::move(incoming));
				}
			}

			if (engine.ClientConn->TimedOut(now))
			{
				FailClient(engine, "server at " + engine.ServerAddress.Text() + " went silent; session ended");
				return;
			}

			std::vector<uint8_t> packet;
			if (engine.ClientConn->Build(packet, kKindData, nullptr, now, false))
				engine.ClientBox->Submit(engine.ServerAddress, std::move(packet));

			Publish(engine, FromConnection(*engine.ClientConn, now, true, 0, 0));
			if (now >= engine.ClientLog)
			{
				engine.ClientLog = now + std::chrono::seconds(1);
				LogStats(engine.ClientConn->Account(), *engine.ClientConn, now, 0, 0);
			}
		}

		bool ParseTest(const std::string& test, Engine& engine, std::string& error)
		{
			engine.ReliableTotal = 0;
			engine.Spoof = false;
			engine.Flood = false;
			if (test.empty())
				return true;
			if (test == "reliable-flood")
			{
				engine.Flood = true;
				return true;
			}
			if (test == "spoof")
			{
				engine.Spoof = true;
				return true;
			}
			const std::string prefix = "reliable:";
			if (test.rfind(prefix, 0) == 0)
			{
				const std::string count = test.substr(prefix.size());
				char* end = nullptr;
				const long value = std::strtol(count.c_str(), &end, 10);
				if (end == count.c_str() || *end != '\0' || value <= 0 || value > 10000)
				{
					error = "--net-test=reliable:<count> needs a count from 1 to 10000";
					return false;
				}
				engine.ReliableTotal = static_cast<int>(value);
				return true;
			}
			if (test == "offline" || test == "predict" || test == "predict-loss"
				|| test == "predict-desync" || test == "predict-wall" || test == "predict-step"
				|| test == "fields")
				return true;
			error = "--net-test is 'spoof', 'reliable:<count>', 'reliable-flood', a predict mode, or 'fields'";
			return false;
		}

	}

	bool ParseSim(const std::string& text, Sim& out, std::string& error)
	{
		out = {};
		if (text.empty())
			return true;

		size_t cursor = 0;
		while (cursor < text.size())
		{
			const size_t comma = text.find(',', cursor);
			const std::string piece = text.substr(cursor, comma == std::string::npos ? std::string::npos : comma - cursor);
			cursor = comma == std::string::npos ? text.size() : comma + 1;
			if (piece.empty())
				continue;

			const size_t colon = piece.find(':');
			if (colon == std::string::npos || colon == 0 || colon + 1 >= piece.size())
			{
				error = "--net-sim entry '" + piece + "' is not key:value";
				return false;
			}
			const std::string key = piece.substr(0, colon);
			const std::string valueText = piece.substr(colon + 1);
			char* end = nullptr;
			const long value = std::strtol(valueText.c_str(), &end, 10);
			if (end == valueText.c_str() || *end != '\0' || value < 0)
			{
				error = "--net-sim " + key + " is not a non-negative integer";
				return false;
			}

			if (key == "lat")
				out.LatencyMs = static_cast<int>(value);
			else if (key == "jit")
				out.JitterMs = static_cast<int>(value);
			else if (key == "loss")
				out.LossPercent = static_cast<int>(value);
			else if (key == "dup")
				out.DuplicatePercent = static_cast<int>(value);
			else if (key == "reorder")
				out.ReorderPercent = static_cast<int>(value);
			else
			{
				error = "--net-sim has no '" + key + "' (lat, jit, loss, dup, reorder)";
				return false;
			}
			if ((key == "loss" || key == "dup" || key == "reorder") && value > 100)
			{
				error = "--net-sim " + key + " is a percent";
				return false;
			}
			if ((key == "lat" || key == "jit") && value > 10000)
			{
				error = "--net-sim " + key + " is over 10 s";
				return false;
			}
		}
		return true;
	}

	void SetSim(const Sim& sim) { g_Sim = sim; }
	const Sim& GetSim() { return g_Sim; }

	bool Listen(const Address& bind, std::string& error)
	{
		auto guard = LockState();
		Engine& engine = State();
		if (engine.Serving)
		{
			error = "a server socket is already open";
			return false;
		}
		if (!engine.ServerSocket.OpenServer(bind, error))
			return false;
		engine.ServerBox = std::make_unique<Conditioner>(g_Sim);
		engine.Serving = true;
		engine.NextLog = Connection::Clock::now() + std::chrono::seconds(1);
		return true;
	}

	void SetTextHandler(std::function<std::string(const Address&, const std::string&)> handler)
	{
		auto guard = LockState();
		State().OnText = std::move(handler);
	}

	void NoteAdmitted(const std::string& account, const Address& from)
	{
		auto guard = LockState();
		Engine& engine = State();
		engine.Admitted[account] = from;
		for (Peer& peer : engine.Peers)
		{
			if (peer.Conn->Account() == account)
				peer.Conn->SetRemote(from);
		}
	}

	void PumpServer(int waitMs)
	{
		Engine& engine = State();
		{
			auto guard = LockState();
			if (!engine.Serving)
				return;

			const auto now = Connection::Clock::now();
			engine.ServerBox->Flush(engine.ServerSocket, now);
			TickServer(engine, now);
		}

		// The main thread queues snapshots during this wait. CloseServer runs only after
		// this thread has been joined, so the socket is still ours here.
		if (waitMs > 0)
			engine.ServerSocket.Wait(waitMs);

		auto guard = LockState();
		if (!engine.Serving)
			return;

		uint8_t buffer[2048];
		for (;;)
		{
			int length = 0;
			Address from;
			std::string error;
			const Socket::Recv result = engine.ServerSocket.Receive(buffer, sizeof(buffer), length, from, error);
			if (result == Socket::Recv::WouldBlock || result == Socket::Recv::Closed)
				break;
			if (result == Socket::Recv::Refused)
				continue;
			if (result == Socket::Recv::Error)
			{
				GE_CORE_WARN("Game port read failed: {0}", error);
				break;
			}
			if (length <= 0)
				continue;

			const bool text = IsHello(buffer, length) || (buffer[0] >= 32 && ReadPacket(buffer, length).Ok == false);
			if (!text && ReadPacket(buffer, length).Ok)
			{
				HandleServerPacket(engine, buffer, length, from);
				continue;
			}
			if (!text || !engine.OnText)
				continue;

			const std::string datagram(reinterpret_cast<const char*>(buffer), static_cast<size_t>(length));
			const std::string reply = engine.OnText(from, datagram);
			if (reply.empty())
				continue;
			GE_CORE_INFO("Join from {0}: {1}", from.Text(), reply.rfind("WELCOME ", 0) == 0 ? reply : reply);
			engine.ServerBox->Submit(from, std::vector<uint8_t>(reply.begin(), reply.end()));
		}

		engine.ServerBox->Flush(engine.ServerSocket, Connection::Clock::now());
	}

	void CloseServer()
	{
		auto guard = LockState();
		Engine& engine = State();
		engine.Serving = false;
		engine.Peers.clear();
		engine.Admitted.clear();
		engine.ServerBox.reset();
		engine.ServerSocket.Close();
		engine.OnText = {};
		Stats stats;
		Publish(engine, stats);
	}

	bool ConnectDev(const std::string& hostPort, const std::string& name, const std::string& test, std::string& error)
	{
		Engine& engine = State();
		if (engine.Client)
		{
			error = "a client session is already open";
			return false;
		}
		if (!ParseAddress(hostPort, engine.ServerAddress, error))
			return false;
		if (!ParseTest(test, engine, error))
			return false;

		engine.ClientSocket = std::make_unique<Socket>();
		if (!engine.ClientSocket->OpenClient(error))
		{
			engine.ClientSocket.reset();
			return false;
		}

		engine.DevName = name;
		engine.ClientBox = std::make_unique<Conditioner>(g_Sim);
		engine.HelloAttempts = 1;
		engine.HelloSent = Connection::Clock::now();
		engine.HelloRefused = false;
		engine.EchoExpect = 0;
		engine.EchoBad = false;
		engine.EchoLogged = false;
		engine.SpoofSent = false;
		const std::string hello = "HELLO dev:" + name;
		engine.ClientBox->Submit(engine.ServerAddress, std::vector<uint8_t>(hello.begin(), hello.end()));
		engine.ClientBox->Flush(*engine.ClientSocket, engine.HelloSent);
		engine.ClientPhase = Engine::Phase::Hello;
		engine.Client = true;
		GE_CORE_INFO("Dev join to {0} as {1}", hostPort, name);
		return true;
	}

	bool BeginClient(std::unique_ptr<Socket> socket, const std::string& hostPort, const std::string& account, std::string& error)
	{
		Engine& engine = State();
		if (engine.Client)
		{
			error = "a client session is already open";
			return false;
		}
		if (!socket)
		{
			error = "join handed over no socket";
			return false;
		}
		if (!ParseAddress(hostPort, engine.ServerAddress, error))
			return false;

		engine.ClientSocket = std::move(socket);
		engine.ClientBox = std::make_unique<Conditioner>(g_Sim);
		engine.ClientConn = std::make_unique<Connection>(0, engine.ServerAddress, account);
		const auto now = Connection::Clock::now();
		engine.ClientConn->MarkAlive(now);
		std::vector<uint8_t> request;
		engine.ClientConn->Build(request, kKindRequest, &account, now, true);
		engine.ClientBox->Submit(engine.ServerAddress, std::move(request));
		engine.ClientBox->Flush(*engine.ClientSocket, now);
		engine.ClientPhase = Engine::Phase::Session;
		engine.Client = true;
		engine.ReliableTotal = 0;
		engine.Spoof = false;
		GE_CORE_INFO("Joined {0} as {1}; the socket stays open", hostPort, account);
		return true;
	}

	void PumpClient()
	{
		Engine& engine = State();
		if (!engine.Client || !engine.ClientSocket || !engine.ClientBox)
			return;

		const auto now = Connection::Clock::now();
		engine.ClientBox->Flush(*engine.ClientSocket, now);
		switch (engine.ClientPhase)
		{
			case Engine::Phase::Hello:   PumpClientHello(engine, now); break;
			case Engine::Phase::Session: PumpClientSession(engine, now); break;
			case Engine::Phase::Live:    PumpClientLive(engine, now); break;
			case Engine::Phase::Dead:    break;
		}
		if (engine.Client && engine.ClientBox && engine.ClientSocket)
			engine.ClientBox->Flush(*engine.ClientSocket, Connection::Clock::now());
	}

	void ShutdownClient()
	{
		Engine& engine = State();
		if (!engine.Client && !engine.ClientSocket)
			return;
		engine.Client = false;
		engine.ClientPhase = Engine::Phase::Dead;
		engine.ClientConn.reset();
		engine.ClientBox.reset();
		engine.ClientSocket.reset();
		Stats stats;
		Publish(engine, stats);
	}

	Stats GetStats()
	{
		Engine& engine = State();
		std::lock_guard<std::mutex> lock(engine.PublishLock);
		return engine.Published;
	}

	std::vector<std::string> ServerAccounts()
	{
		auto guard = LockState();
		Engine& engine = State();
		std::vector<std::string> accounts;
		accounts.reserve(engine.Peers.size());
		for (const Peer& peer : engine.Peers)
			accounts.push_back(peer.Conn->Account());
		return accounts;
	}

	bool SnapshotAcked(const std::string& account, uint16_t sequence)
	{
		auto guard = LockState();
		Engine& engine = State();
		for (const Peer& peer : engine.Peers)
		{
			if (peer.Conn->Account() == account)
				return peer.Conn->IsAcked(sequence);
		}
		return false;
	}

	void SendReliable(const uint8_t* data, int length)
	{
		if (!data || length <= 0 || length > 256)
			return;
		auto guard = LockState();
		Engine& engine = State();
		if (!engine.Serving)
			return;
		for (Peer& peer : engine.Peers)
		{
			if (!peer.Conn)
				continue;
			peer.Conn->Enqueue(std::vector<uint8_t>(data, data + length));
		}
	}

	uint16_t SendSnapshot(const std::string& account, const uint8_t* data, int length)
	{
		auto guard = LockState();
		Engine& engine = State();
		if (!engine.Serving || !engine.ServerBox || !data || length <= 0)
			return 0;
		for (Peer& peer : engine.Peers)
		{
			if (peer.Conn->Account() != account)
				continue;
			std::vector<uint8_t> packet;
			const auto now = Connection::Clock::now();
			const uint16_t sequence = peer.Conn->BuildSnapshot(packet, data, length, now);
			if (sequence == 0)
				return 0;
			engine.ServerBox->Submit(peer.Conn->Remote(), std::move(packet));
			return sequence;
		}
		return 0;
	}

	std::vector<SnapshotDatagram> DrainSnapshots()
	{
		Engine& engine = State();
		std::vector<SnapshotDatagram> out;
		out.reserve(engine.IncomingSnapshots.size());
		for (Engine::Incoming& incoming : engine.IncomingSnapshots)
		{
			SnapshotDatagram datagram;
			datagram.Bytes = std::move(incoming.Bytes);
			datagram.RttMs = incoming.RttMs;
			datagram.At = incoming.At;
			datagram.Session = incoming.Session;
			datagram.Sequence = incoming.Sequence;
			out.push_back(std::move(datagram));
		}
		engine.IncomingSnapshots.clear();
		return out;
	}

	void SendCommands(const uint8_t* data, int length)
	{
		auto guard = LockState();
		Engine& engine = State();
		if (!engine.Client || !engine.ClientConn || !engine.ClientBox || !data || length <= 0)
			return;
		if (engine.ClientPhase != Engine::Phase::Live)
			return;
		std::vector<uint8_t> packet;
		const auto now = Connection::Clock::now();
		if (engine.ClientConn->BuildSnapshot(packet, data, length, now, kKindCommand) == 0)
			return;
		engine.ClientBox->Submit(engine.ServerAddress, std::move(packet));
	}

	std::vector<CommandDatagram> DrainCommands()
	{
		auto guard = LockState();
		Engine& engine = State();
		std::vector<CommandDatagram> out;
		out.reserve(engine.IncomingCommands.size());
		for (Engine::CommandIn& incoming : engine.IncomingCommands)
		{
			CommandDatagram datagram;
			datagram.Account = std::move(incoming.Account);
			datagram.Bytes = std::move(incoming.Bytes);
			out.push_back(std::move(datagram));
		}
		engine.IncomingCommands.clear();
		return out;
	}

}
