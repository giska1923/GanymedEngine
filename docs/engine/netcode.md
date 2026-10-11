# Netcode

The game session that starts after `WELCOME`. A client and a server exchange UDP datagrams on one
socket, each side knows the round trip and the loss, and a small reliable channel carries the few
messages that must arrive in order. The world itself travels as unreliable snapshots: each one
replaces the last, and a client draws replicated entities from the two snapshots around a time a
tenth of a second behind the server's tick. Predicting the local player is N4 of
[NETCODE.md](../ToDo/NETCODE.md). A script still sees only `Net.GetStats()`.

| | |
|---|---|
| Sources | `GanymedEngine/source/GanymedE/Net/` — `Socket`, `Connection`, `Net.h`, `Host.cpp`, `BitStream.h`, `Quantize.h`, `World.cpp` |
| Who pumps it | The dedicated process's door thread calls `PumpServer`. The runtime's main thread calls `PumpClient` from `Online::OnUpdate`, once a frame, after the join's handshake poll. The main thread builds snapshots; the door is what calls `sendto` |
| What a script sees | `Net.GetStats()`. Nothing else. `Net.IsServer` and `OnTick` are N4 and N5 |

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
| 20 | u8 | Kind. `1` session request, `2` session accept, `3` data, `4` snapshot |

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

## Ticks

The server numbers its physics steps. `PhysicsSystem` calls `NoteSimulatedStep` once per step, and
only when the scene's role is `Server`, so the client's own physics loop does not advance the
clock. The step is `PhysicsSettings::FixedTimestep`, 1/60 s. A snapshot is every second tick.

That capture sits inside the step loop, after the discrete pose has been written back, and not
once at the end of the frame. A hitch that burns several steps would otherwise publish only the
last tick and leave a hole wider than the client's delay. The blend `SyncTransforms` does with
the leftover accumulator is the local render. The snapshot is the tick.

The client does not add the round trip on top of that. A snapshot that says tick T, received at
time R, estimates the server's current tick as `T + (now − R) / FixedTimestep`. The lead N4 will
simulate ahead by is half the measured round trip. It is logged (`lead=`) and it is not added into
the render time. Adding it again would count the same delay twice: once in how late the snapshot
is, and once more as a bias.

## Snapshots

Kind `4`. The body is not a reliable message. `Receive` still notes the sequence and the ack, and
`BuildSnapshot` records a sent packet with an empty reliable-id list, so the snapshot takes part
in the round trip and the loss figures. It does not carry, and is not carried by, the reliable
channel. A newer snapshot makes an older one useless, which is why it is unreliable.

The main thread builds the packet and submits it to the conditioner. The door thread sends it.
`StateLock` on the server is a recursive mutex: `PumpServer` holds it across the flush and the
receive, and not across `select`, and `NoteAdmitted` runs on the door while that lock is already
held. `SendSnapshot` takes the same lock. The client's incoming snapshot queue is main-thread
only. `PumpClient` pushes, `PresentWorld` drains, and there is no lock on that queue. Lock order,
where both exist, is `StateLock` then the stats publish lock.

Per client the server keeps up to 32 sent snapshots, keyed by the sequence `BuildSnapshot`
assigned. The baseline is the newest of those whose sequence is still acknowledged inside the
128-packet send window. No such snapshot means a full state: every live replicated entity is a
spawn. The values in a delta are absolute. What the delta selects is which entities and which
fields changed, not a numeric difference. An empty delta is still sent, so the client's clock
moves on a scene that is sitting still.

Spawns and destroys travel in the same packet. There is no reliable spawn. A destroy is also
emitted for anything in the newest sent stamp that is no longer in the world, even when the spawn
that created it was never acknowledged. Otherwise a client that applied a spawn whose ack was
lost, and then watched the server destroy the entity, would keep it. Destroys are packed at the
front, so a full datagram does not drop them behind spawns. Destroying an unknown net id on the
client is a no-op. Destroying a prefab collects the subtree and destroys it from the leaves,
because `Scene::DestroyEntity` unparents children rather than destroying them.

If the next record does not fit, the packet still goes out with the records that did, and the
stamp stored for that sequence is the baseline plus those records. The remainder is still a diff
against that partial stamp next time, so a world larger than one datagram drains about one batch
per acknowledgement rather than starting over. One record that cannot fit alone logs a warning
and the packet is the tick header with a count of zero.

The body budget is 1200 − 21 = 1179 bytes. The header is a u32 tick, a u16 baseline sequence
(`0` means this packet is a full state), and a u16 count. Each record is a u16 net id and a 2-bit
op: `1` spawn, `2` destroy, `3` update. A spawn adds a u64 UUID and a path: a 7-bit length, at
most 80, then the bytes. A destroy stops there. Anything else carries a 6-bit mask — position,
yaw, euler, velocity, ground, aim — and the fields the mask names. The same `Bits` function
writes, measures, and reads, least significant bit first inside the byte, so the three cannot
disagree about a width. The codec is the compile-time list in `World.cpp`, not `entt::meta`.
[scene.md](scene.md) forbids reflection on the replication pass.

What that list sends, for an entity with `ReplicatedComponent`:

| Field | When | Encoding |
|---|---|---|
| Local position | Always | Millimetres, biased into 24 bits. The span is ±8192 m. Nothing in the tree publishes a tighter map bound. Past it, the value clamps |
| Yaw | The entity has a `CharacterControllerComponent` | uint16 over one full turn |
| Euler XYZ | It does not | Each axis uint16 over [−π, π], wrapped before the pack |
| Linear velocity and ground | It is a character | Signed 16 bits, millimetres per second, clamped to ±32 m/s. Ground is 2 bits: `0` OnGround, `1` OnSteepGround, `2` NotSupported, `3` InAir. Read from Jolt. Nothing writes it back |
| Aim pitch and yaw | `AimOffsetComponent` is present | The same signed-angle pack as an Euler axis |

Scale is not replicated. Script fields are not. The world transform is not: the client already
has the parent. The server assigns a 16-bit net id, starting at 1 and skipping 0, stored on the
component with `Trait::Runtime` so the file does not keep it. Scene entities match by UUID. An
entity the client does not have yet is `CreateEntityWithUUID` with the server's UUID, or
`Instantiate` with that UUID pinned as the root when the spawn names a prefab. The client refuses
a path that is empty, longer than 80, contains `..`, or uses a character outside
`A–Z`, `a–z`, `0–9`, `_`, `.`, `/` and `-`.

`GanymedDedicated --net-test=` is `move`, `spawn`, `prefab`, or `rest`. Anything else exits 2,
including under `--dev`. The runtime's `--net-test` is still only `reliable:<count>` and `spoof`.
The four server tests are what [NETCODE.md](../ToDo/NETCODE.md)'s N3 checks ran. They are not a game mode.

## Interpolation

`GanymedRuntime --connect` constructs the scene as `SceneRole::Client` before `OnRuntimeStart`.
Client registers the same systems as Standalone. What changes is `GetRole()`.

`PresentWorld` runs at the start of `RuntimeLayer::OnUpdate`, before `OnUpdateRuntime`. A snapshot
spawn is `CreateEntity`, and that asserts once a system update has set the updating flag. The
drain is the packets `PumpClient` queued earlier in the same frame.

Replicated entities on a client are kinematic. `AdoptReplicatedMotion` turns a dynamic body into
a kinematic one before the step, and both `SyncTransforms` and the character step skip a
replicated entity, so the physics writeback does not overwrite the blend. A kinematic body
already copies the transform in during `Step`. The ground state is not written into
`CharacterVirtual`. There is no setter.

The render tick is the estimate minus `0.100 / FixedTimestep`, which is 6 ticks at 60 Hz. The
entity is the blend of the two buffer samples around that tick. The buffer keeps 32 samples per
entity. When the render tick is past the newest snapshot, the frame is dry: the pose extrapolates
the last segment by at most one snapshot interval (2 ticks) and then holds, and the frame is
counted. Counting starts after one second of summed frame time, so the wait for the first
snapshot is not a dry run. An entity that has stopped moving keeps its last sample. Holding that
sample while the render tick is still behind the newest packet is not dry. An entity is
extrapolated only when the frame is dry and its own newest sample is within 2 ticks of that
packet.

Once a second, while a test is running:

```
Replicate server peer={account} tick={n} entities={n} registry={n} snap={bytes} min={bytes} Bps={payload bytes in the last 1s}
Replicate client tick={floor of the render tick} est={.1f} lead={rtt/2} snap={body bytes} depth={max samples} dry={percent} step={max ratio} entities={n} registry={n} ghosts={n} frames={n}
```

`step` is the largest ratio, since the warmup, of how far an entity was drawn this frame to how
far the snapshot segment under the render tick says it should have moved across the ticks the
render clock advanced. A continuous blend sits near 1. A teleport is many times that. `dry` is
the percentage of warmed frames that ran past the newest snapshot. `Bps` counts payload bytes at
the moment the snapshot is queued, not on the wire, so the conditioner's loss is not in it.

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
