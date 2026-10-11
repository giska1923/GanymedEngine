# Netcode

The game session that starts after `WELCOME`. A client and a server exchange UDP datagrams on one
socket, each side knows the round trip and the loss, and a small reliable channel carries the few
messages that must arrive in order. There is no game state in the packets yet. Snapshots and
prediction are N3 and N4 of [NETCODE.md](../ToDo/NETCODE.md).

| | |
|---|---|
| Sources | `GanymedEngine/source/GanymedE/Net/` — `Socket`, `Connection`, `Net.h`, `Host.cpp` |
| Who pumps it | The dedicated process's door thread calls `PumpServer`. The runtime's main thread calls `PumpClient` from `Online::OnUpdate`, once a frame, after the join's handshake poll |
| What a script sees | `Net.GetStats()`. Nothing else. `OnTick` and replication are later |

## How engines usually do this, and where Ganymed diverges

The transport under a shooter-style session is almost always unreliable UDP plus a thin reliability
layer, not TCP. TCP's head-of-line blocking turns one lost packet into a stall of everything behind
it, and a game would rather drop a stale transform than wait for it. Quake 3 numbered snapshots and
acknowledged them per client. Source did the same on a fixed tick. netcode.io (and the yojimbo
library around it) puts the session keys, the connect token and the encryption in one protocol so
an off-path host cannot inject packets and an on-path one cannot read them.

Ganymed's split is the one [ONLINE.md](../ToDo/ONLINE.md) already made. The token handshake is the
backend's contract (`HELLO` / `WELCOME` / `DENIED`, text, one datagram each way) and stays that way.
The bytes after `WELCOME` are engine-to-engine. They are not in GanymedServer's `docs/api/`. They
are not encrypted. A random session id stops a host that has not seen the handshake from guessing
its way in. An attacker who can see the packets can forge them. That limit is recorded in
[NETCODE.md](../ToDo/NETCODE.md)'s design tensions; closing it means keys inside the token, which
would change the backend's contract.

The other divergence is where the socket lives. JoinMatch used to open a socket, read one reply,
and close it. The server knows a client by the address it sent from, so the next socket would be a
new port and a stranger. The same socket now continues into the session.

## The socket

`Net::Socket` is non-blocking IPv4 UDP. Winsock and BSD sockets are behind it. The header does not
include either, because `Online.cpp` is the one translation unit that includes IXWebSocket, and
that header's UDP socket cannot bind or report the sender. `Socket.cpp` is excluded from the
precompiled header so `winsock2.h` is the first Windows header in the file. It calls `WSAStartup`
and never `WSACleanup`: IXWebSocket's `initNetSystem` holds a count of its own, and a cleanup here
would drop it and break the agent's HTTP.

`bind`, `sendto`, `recvfrom`, and `select`. There is no `SO_RCVTIMEO`. The door thread passes 2 ms
to `select`, which is long enough to sleep and short enough that shutdown and the round-trip sample
are not waiting on a 200 ms receive. Teardown joins that thread and only then closes. Closing under
`select` races the descriptor, and on Linux the number can be reused before the call returns.

On Windows a UDP socket whose earlier send drew an ICMP port-unreachable fails its next receive
with `WSAECONNRESET`. A server socket turns that off with `SIO_UDP_CONNRESET` (defined locally as
`_WSAIOW(IOC_VENDOR, 12)` when the SDK header omits it) and treats a reset that still arrives as
"try again". One dead client must not stop admission. A client socket leaves the error enabled, so
JoinMatch can still tell "nothing is listening" from a second of silence. Linux does not report
ICMP to an unconnected UDP socket, so the same dead port is silence there either way.

Addresses are IPv4 only, parsed with `inet_pton`. No DNS. `127.0.0.1:port` is loopback; anything
else is not, including a name that would resolve there.

## From WELCOME to a session

The text replies are not the game protocol. A datagram whose header parses as a game packet is a
game packet. `HELLO`, or any other datagram whose first byte is printable and which is not a game
packet, goes to the admission handler. The handler's reply (`WELCOME` or `DENIED`) is what gets
logged, not the hello.

The client's first game packet is a session request. It carries the account id `WELCOME` named, and
session id 0. The server accepts it only from the address that was just admitted, and answers with
a session accept: a random non-zero 64-bit id. Later packets carry that id. A packet from the
connection's address with a different id increments `droppedSession`. A packet from any other
address increments `droppedAddress`, even when the id is right. Lookup is by address.

A player admitted twice keeps one session. A fresh token re-admits an account the server already
has (`connect-token.md`, rule 8). `NoteAdmitted` points that account's connection at the newest
address and keeps the id. The new client learns the id from a new session accept. `--dev` admits
`HELLO dev:<name>` with no token — name is 1–32 of `A–Z`, `a–z`, `0–9`, `-` and `_` — and then the
same request.

JoinMatch holds one socket for every attempt. O5a opened a new one per try, so a `WELCOME` lost on
attempt 1 left the server holding a port the client had already closed. The first attempt binds an
ephemeral port. Retries reuse it. On `WELCOME` the socket is moved into `BeginClient` before the
join is erased. `OnlineTransport::Exchange` borrows the pointer, sends at once, and does not close
it. Cancel drops the borrow and then destroys the join, which closes the socket.

That handshake send is not put through the conditioner. Its retry is one second, and a simulated
delay belongs to the session, not to a wait that would otherwise time out on the delay itself.
The server's text reply does go through its conditioner, so the one-second wait includes the
server's delay. A delay under that second still leaves room for the reply.
`--dev` / `--connect` is the other hello: it is conditioned, because that path has no ticket and
the test is the network. Everything after the handover is conditioned, on both sides, from the
`Sim` that was installed before `Listen` and before `OnRuntimeStart`.

Silence of 5 s ends the connection. The server logs `Player {account} left (timeout)`. The client
logs `server at {addr} went silent; session ended`. Telling a script is N5. Each side sends at
least every 100 ms, and also on the pump that noticed the peer's packet, so the acknowledgement
does not wait out the keepalive. On a quiet link that is about one packet per round trip, and
never slower than the 100 ms floor. When the round trip is near that floor the two overlap and the
rate sits a little above ten a second.

## The packet

Little-endian, 21 bytes, then the body. A datagram is capped at 1200 bytes so it stays under a
common internet MTU once the IP and UDP headers are on it.

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | Protocol id `0x47453201`. The first byte is `0x01`, which is how a game packet is told from `HELLO` |
| 4 | u64 | Session. `0` on the request, then the id from the accept |
| 12 | u16 | Sequence. `0` means nothing has been sent. Numbers start at 1 and skip 0 again when they wrap |
| 14 | u16 | Ack. The newest sequence received. `0` means nothing has been received |
| 16 | u32 | Ack bits. Bit `i` set means sequence `ack - 1 - i` was received. 32 packets of history |
| 20 | u8 | Kind. `1` session request, `2` session accept, `3` data |

Comparison of sequence numbers is a signed 16-bit subtract, so the wrap is ordinary.

A request appends one length byte and the account id. An accept is the header only. A data packet
appends a count byte and then, that many times, a u16 id, a u16 size, and the bytes. Those are the
reliable messages still waiting for an acknowledgement.

## Acks, round trip, loss

The send time of a packet is the moment `Build` hands it to the conditioner, not the moment it
reaches the socket. Both sides' simulated delays are therefore inside the sample. The sample is
taken only when the peer's `ack` field newly names one of our packets. The bitfield marks the
older ones as received and does not move the estimate, because those packets may have been
acknowledged late. The first sample is copied. After that, `rtt += (sample - rtt) / 8`.

Loss out is cumulative over the whole session, not a sliding window of the last 32. A sent packet
that falls more than 32 sequences behind the newest ack without being acknowledged is lost. A
thirty-second run needs the whole session in the denominator; a short window would be a coin flip.
Loss in is gaps in the received sequence. A packet that arrives late, still inside the 32-bit
window, fills its gap and is not counted as loss. Reordering is not loss.

Bytes per second are a one-second sliding sum of datagram sizes, each way. `resent` counts packets
that carried at least one reliable message we had already sent, not each message.

## The reliable channel

For the few messages that must arrive, and arrive once, in order: a player joined, the match
ended. Nearly everything else supersedes itself and will not use this.

`Enqueue` rejects an empty payload and anything over 256 bytes. Ids start at 1 and skip 0, same as
sequences. Each data packet carries as many unacknowledged messages as fit in the 1200-byte cap,
oldest first. A message stays queued until some packet that contained it is acknowledged, then it
is dropped. The receiver delivers id `N` only when it has delivered `N - 1`. Anything newer is
held. Anything older is ignored. A duplicate therefore does not run twice, and a hole does not
deliver the messages behind it.

`--net-test=reliable:<count>` (1 to 10000) is a client flag, used after the session accept. It
enqueues `count` payloads of a type byte `1` and a little-endian index. The server, on an in-order
delivery of type `1`, echoes type `2` with the same index, and only when the index is the next one
it expected. The client's success line is the proof the server saw them in order. Type `2` is not
echoed. Nothing sends these unless the flag is set. A fleet server will still echo a type `1` a
client sends: there is no encryption to refuse it, which is the same limit as the rest of the
session.

`--net-test=spoof` sends two datagrams that do not go through the connection and do not go through
the conditioner, so a drop is the server's check and not the loss simulator. One has the session
id plus one, from the real socket. One has the real id, from a second ephemeral socket. The
server's next stats line counts them as a session drop and an address drop.

## The conditioner

`--net-sim=lat:ms,jit:ms,loss:percent,dup:percent,reorder:percent` on both executables. Empty means
off. `lat` and `jit` are milliseconds, at most 10000. The other three are percents, 0–100. A bad
value on the dedicated process exits 2 with the other flags, before `Application` exists. On the
runtime it is logged and the process does not connect.

The queue sits on the send path. Loss is decided after the connection has already counted the
packet as sent, so the measured loss is the simulated loss. Jitter is uniform on `[0, jit]` and
added to `lat`. Reorder holds that fraction for an extra `max(50 ms, lat)`, long enough for a
later packet to pass. Duplicate queues a second copy 10 ms later. The roll is `mt19937` seeded
from `random_device`, uniform on `0 .. 99`.

`SetSim` has to happen before `Listen`, because the server's conditioner copies the value then,
and before `OnRuntimeStart`, because a script that calls `JoinMatch` from `OnCreate` has to see it.
The runtime does both at the start of `OnAttach`. The dedicated process does it before `Start`.

## `--dev` and `--connect`

`GanymedDedicated --dev` does not talk to an agent. No health thread, no `ready`, no result, and
the match clock does not run: a loss measurement needs longer than `dedicated.yaml`'s ten seconds,
and killing the process is the stop. It binds `127.0.0.1` only. The port defaults to 7777.
`--advertise` is forced to `127.0.0.1:<port>`; anything else exits 2. The scene still loads as
`SceneRole::Server`. The door is the same thread as the fleet path. [dedicated.md](../dedicated/dedicated.md)
has the flag.

`GanymedRuntime --connect=<host:port>` sends `HELLO dev:<name>` after boot, three attempts of one
second, then the session request. `--name` defaults to `p` plus the process id, so two runtimes on
one machine are two sessions. A bad `--net-sim` skips the connect. The runtime's own
[page](../runtime/runtime.md) lists the flags.

One process is a server or a client. `Listen` and `ConnectDev` do not both run.

## Stats

Once a second, per live connection:

```
Net {who}: rtt {ms} ms, loss in {%}% out {%}%, {B/s} in, {B/s} out, resent {n}, dropped session {n}, address {n}
```

`Net.GetStats()` returns `connected`, `peers`, `rtt`, `lossIn`, `lossOut`, `bytesInPerSec`,
`bytesOutPerSec`, `resent`, `droppedSession`, `droppedAddress`. On a client that is its one
session. On a server the round trip and the loss are means, and the byte rates and `resent` are
sums. With no session every number is zero and `connected` is false. The counters are `uint64`
in C++ and `int64` in Lua, the same cast as a UUID: sol2 throws on a `uint64` above `INT64_MAX`.
The door thread writes a snapshot under a mutex. The frame only reads the snapshot.

`Net.IsServer` and `OnTick` are not this. They are N4 and N5.
