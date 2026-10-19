# Netcode

The game session that starts after `WELCOME`. A client and a server exchange UDP datagrams on one
socket, each side knows the round trip and the loss, and a small reliable channel carries the few
messages that must arrive in order. The world itself travels as unreliable snapshots: each one
replaces the last, and a client draws replicated entities from the two snapshots around a time a
tenth of a second behind the server's tick. The character a client owns is predicted from the
commands it sent, and corrected when a snapshot disagrees. A script sees `Net.GetStats()`,
`Net.IsServer()`, `Net.IsClient()`, `OnTick`, replicated fields, `Net.Broadcast`, and `Net.Shot`.

| | |
|---|---|
| Sources | `GanymedEngine/source/GanymedE/Net/` — `Socket`, `Connection`, `Net.h`, `Host.cpp`, `BitStream.h`, `Quantize.h`, `Command.h`, `ScriptWire.h`, `World.cpp` |
| Who pumps it | The dedicated process's door thread calls `PumpServer`. The runtime's main thread calls `PumpClient` from `Online::OnUpdate`, once a frame, after the join's handshake poll. The main thread builds snapshots; the door is what calls `sendto` |
| What a script sees | `Net.GetStats()`, `Net.IsServer()`, `Net.IsClient()`, `OnTick(tick, command)`, a `Replicated` table, `Net.Broadcast`, `Net.Shot`, `OnNetEvent`, `OnShot`. Both role queries are false offline. `IsLocallyControlled()` is on the entity |

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
logs `server at {addr} went silent; session ended`. That path does not call a script. A server
script can `Net.Broadcast` when it decides the match is over. Each side sends at
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
| 20 | u8 | Kind. `1` session request, `2` session accept, `3` data, `4` snapshot, `5` commands |

Comparison of sequence numbers is a signed 16-bit subtract, so the wrap is ordinary.

A request appends one length byte and the account id. An accept is the header only. A data packet
appends a count byte and then, that many times, a u16 id, a u16 size, and the bytes. Those are the
reliable messages still waiting for an acknowledgement. A command packet is not one of those: its
body is a count and then the last few `PlayerCommand`s, and `Receive` does not try to deliver it
as a reliable message. It still notes the sequence and the ack.

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

**What may be held is bounded.** A message more than 256 ids past the next expected one is dropped
unheld, and so is any message once 256 are already held. Each one is counted as `reliable refused`
in the stats line. An honest sender cannot hit either limit: a packet carries at most 255 messages,
starting from the sender's oldest unacknowledged one, so nothing it sends is more than 254 ahead.
Without the limit, any peer holding a session could make the receiver store up to 255 messages per
packet, with ids anywhere up to 32,767 ahead, for as long as it kept sending.
`--net-test=reliable-flood` (a client flag) sends four raw data packets of 255 messages each, all
outside the window. Measured on 2026-10-17: the server's stats line read `reliable refused 1020`,
and `reliable:1000` at `loss:20,reorder:50` on both sides still delivered all 1000 in order, with
`reliable refused 0`.

`--net-test=reliable:<count>` (1 to 10000) is a client flag, used after the session accept. It
enqueues `count` payloads of a type byte `1` and a little-endian index. The server, on an in-order
delivery of type `1`, echoes type `2` with the same index, and only when the index is the next one
it expected. The client's success line is the proof the server saw them in order. Type `2` is not
echoed. Nothing sends these unless the flag is set. A fleet server will still echo a type `1` a
client sends: there is no encryption to refuse it, which is the same limit as the rest of the
session.

Type `3` is a server-to-client script event, one per message, packed with the same bits as a
snapshot event. `SendReliable` queues it on every peer from the main thread. The door still does
the `sendto`. A payload over 256 bytes is dropped, which is `Enqueue`'s cap. The server's deliver
path ignores type `3`: a client cannot inject one. The client's deliver path queues it for the
scene update. Bytes `1` and `2` are still only the probe.

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

A snapshot that says tick T, received at time R, estimates `T + (now − R) / FixedTimestep`. At the
moment it arrives, now is R, so the estimate is T. The flight is not in `(now − R)`. That term is
only how long the packet has been sitting on the client. While it was in the air the server kept
simulating, about half a round trip's worth.

The render clock is that estimate minus 100 ms, and the flight is not added there either. Remote
entities are drawn in the past on purpose. Adding the round trip on top of the interpolation delay
would put them further behind, which is the double count this clock has to avoid. The `lead=` field
on the replicate line is still half the measured round trip, in milliseconds, and it is still not
added into the render time.

Prediction is a different clock. The command for tick T has to be in the server's buffer when the
server starts tick T, and the trip back is the other half of the round trip. The client therefore
simulates `ceil(rtt / FixedTimestep) + 2` ticks ahead of the estimate, at least 1 and at most 32.
From a snapshot that has just arrived, the round-trip part is a full round trip ahead of T, which
is about half a round trip ahead of where the server is now: the tick the command arrives for.
Leading by only half the round trip catches the server's present, and the command is late.

The two extra ticks are the margin, and the round trip alone left none. A command a tick late is
not used: the server repeats the previous one, and at a change of direction that is a correction.
The margin covers what the round trip does not show. The server steps several ticks per frame, so
a command has to arrive before the frame that simulates its tick, not before the tick itself. The
client sends at the end of its own frame. And the RTT is a smoothed average, not the slow packets.
Measured with `lat:75` each way (176 to 177 ms once the poll is in the sample), the lead is 13
ticks, 11 of them the round trip. The predict line logs it as `leadTicks=`. Without the margin, a
60 s walk had about 18.5% of its ticks repeated at 0% loss. With it, none were (2026-10-17).

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
fields changed **since that baseline**, not a numeric difference. An empty delta is still sent, so
the client's clock moves on a scene that is sitting still.

**The client decodes each snapshot onto exactly the baseline the header names**, never onto
whatever it applied last. It keeps the full state after each of the last 64 snapshots it received,
keyed by transport sequence, whether it applied them or not, because any of them can be named as
the next baseline. The new state is that baseline with the snapshot's records stamped on, through
`StampAfter`, the same function the server uses for what the client holds after a snapshot, so both
sides compute the same state from the same records. The scene then follows the full state:
entities that left it are destroyed, entities that joined it are spawned, every entity gets a
sample of its whole state at the snapshot's tick, and script fields are written when they differ
from what the scene was last given. This is Quake 3's delta rule.

The latest applied state is not a substitute for the baseline. A field missing from a delta means
"as in the baseline", and the two differ whenever a value has moved and come back within a round
trip. Before this rule, the client filled missing fields from its latest sample. A character
reversing direction passes back through the positions of the ticks before the reversal. About ten
ticks after it, its position equalled the baselined one exactly, so the delta omitted it, and the
client read the position from two ticks earlier. Prediction then corrected 133 mm to a wrong
position, and 133 mm back on the next snapshot, at every reversal. The same would happen to any
field toggled twice within a round trip. A snapshot whose baseline the client does not hold is
dropped and counted as `nobase`. Every baseline is a snapshot the client acknowledged, and the
server picks among its last 32 sent, so none has been seen.

Spawns and destroys travel in the same packet. There is no reliable spawn. **The client may hold an
entity the acked baseline does not list:** it applied a spawn whose ack the server has not seen.
If that entity is then destroyed, a diff against the baseline never mentions it. So per client the
server keeps the set of net ids a spawn record has gone out for. Any of them that is gone from the
world gets a destroy in every snapshot, until a snapshot that carried its destroy is acknowledged.
Only then does it leave the set. An earlier version emitted that destroy only while the entity was
in the newest sent stamp. When the one snapshot carrying it was lost, the next stamp no longer
listed the entity, and nothing said it again. `churn` (below) left 10 to 47 entities on a client
that way. Destroys are packed at the front, so a full datagram does not drop them behind spawns.

**The client applies snapshots in tick order, and does not apply one that is not newer** than the
last it applied (`stale=` on the replicate line). It still decodes and keeps that snapshot's state,
since the server may name it as a baseline. UDP reorders and duplicates. Applying an old snapshot
after a newer one moved the tick estimate backwards and put older state on the scene. It could also
spawn an entity that a newer snapshot had already destroyed, and that entity stayed. Not applying
the old snapshot loses nothing: the newer one's state already includes whatever the older one
changed. Quake 3 discards out-of-order snapshots the same way. The ordering, and the kept states,
are per session: a snapshot on a new session starts a new timeline. Destroying an unknown net id on the
client is a no-op. Destroying a prefab collects the subtree and destroys it from the leaves,
because `Scene::DestroyEntity` unparents children rather than destroying them.

If the next record does not fit, the packet still goes out with the records that did, and the
stamp stored for that sequence is the baseline plus those records. The remainder is still a diff
against that partial stamp next time, so a world larger than one datagram drains about one batch
per acknowledgement rather than starting over. One record that cannot fit alone logs a warning
and the packet is the tick header with a count of zero.

The body budget is 1200 − 21 = 1179 bytes. The header is a u32 tick, a u16 baseline sequence
(`0` means this packet is a full state), a u16 owned net id (`0` means this client controls
nothing), and a u16 count. That is 10 bytes before the records. Each record is a u16 net id and a 2-bit
op: `1` spawn, `2` destroy, `3` update. A spawn adds a u64 UUID and a path: a 7-bit length, at
most 80, then the bytes. A destroy stops there. Anything else carries a 7-bit mask — position,
yaw, euler, velocity, ground, aim, script fields — and the fields the mask names. Every snapshot
then ends with a 4-bit event count, even when the count is zero. The same `Bits` function
writes, measures, and reads, least significant bit first inside the byte, so the three cannot
disagree about a width. The codec is the compile-time list in `World.cpp`, not `entt::meta`.
[scene.md](scene.md) forbids reflection on the replication pass.

What that list sends, for an entity with `ReplicatedComponent`:

| Field | When | Encoding |
|---|---|---|
| Local position | Always | Millimetres, biased into 24 bits. The span is ±8192 m. Nothing in the tree publishes a tighter map bound. Past it, the value clamps |
| Yaw | The entity has a `CharacterControllerComponent` | uint16 over one full turn |
| Euler XYZ | It does not | Each axis uint16 over [−π, π], wrapped before the pack |
| Linear velocity and ground | It is a character | Signed 16 bits, millimetres per second, clamped to ±32 m/s. Ground is 2 bits: `0` OnGround, `1` OnSteepGround, `2` NotSupported, `3` InAir. Read from Jolt. Gameplay does not write it. Reconciliation does, and only then |
| Aim pitch and yaw | `AimOffsetComponent` is present | The same signed-angle pack as an Euler axis |
| Script fields | The script's class table declares `Replicated` | Bit 64 of the mask, then the block in the next section. A spawn carries every declared field. A later snapshot carries the ones that changed |

Scale is not replicated. The world transform is not: the client already
has the parent. The server assigns a 16-bit net id, starting at 1 and skipping 0, stored on the
component with `Trait::Runtime` so the file does not keep it. Scene entities match by UUID. An
entity the client does not have yet is `CreateEntityWithUUID` with the server's UUID, or
`Instantiate` with that UUID pinned as the root when the spawn names a prefab. The prefab a spawn
names is the root's `PrefabInstanceComponent` source, as a path under `assets/`, so anything
`Scene.Spawn` made from a prefab whose root carries `ReplicatedComponent` replicates as that prefab.
The test module's spawns register their path explicitly, which comes to the same thing. The client refuses
a path that is empty, longer than 80, contains `..`, or uses a character outside
`A–Z`, `a–z`, `0–9`, `_`, `.`, `/` and `-`.

`GanymedDedicated --net-test=` is `move`, `spawn`, `prefab`, `rest`, `churn`, `fields`, or a predict
mode: `predict`, `predict-loss`, `predict-desync`, `predict-wall`, `predict-step`. Anything else
exits 2, including under `--dev`. The runtime accepts those predict names too, plus `predict-fire`,
`fields`, `offline`, `reliable:<count>`, `reliable-flood` and `spoof`. A name it does not know is a logged
connect failure, not exit 2. The two parsers are not the same list. The modes are how N3, N4 and
the N5 engine hooks were measured. They are not a game mode.

**The modes live in `Net/NetTest.cpp`, not in the replication code.** `World.cpp` calls a handful
of `Net::Test::` hooks (start, the per-frame advance, before and after the scripts tick, after a
prediction frame, when a client spawns a prefab), and each is a no-op when no mode is set. The
movers, arenas, synthetic sticks, the 500-entity phase, the desync teleport and both
`Application::Close` calls are there. A test that needs a pawn uses the same production calls the
game will: `Net::SpawnReplicatedPrefab` and `Net::AssignPawn`. The transport's own probes
(`reliable:<count>`, `reliable-flood`, `spoof`) stay in `Host.cpp`, beside the channel they test.

`churn` spawns one replicated entity per frame and destroys each three ticks later, for 20 s. A life
that short fits inside one reorder hold, so a snapshot that spawned an entity can arrive after the
one that destroyed it. The server logs `Churn done` once it holds none; the client must end with
`ghosts=0`. Measured on 2026-10-17 with `reorder:30` on the server, before both fixes above: the
server held 0, and the client kept 111, for good. With the ordering fix only, 10. With both, 0, at
`reorder:30` and again at `reorder:30,loss:10,dup:10,lat:30`, with 225 and 281 stale snapshots
dropped.

An idle snapshot — no records, event count zero — is 11 bytes. That is the 10-byte header plus the
4-bit count, which rounds up to one more byte. Two idle `NetEcho` entities measured 11 bytes and
330 B/s at 30 Hz on 2026-10-11. The N3 rest figure was taken before the owned id and this trailer,
and it stays in the N3 verification record.

## Script fields and events

Unreal replicates actor properties through a property list and `NetSerialize`, and marks each RPC
reliable or unreliable on the function. Source sends unreliable user messages, and reliable ones on
a different channel. Ganymed already has a snapshot delta and a 256-byte reliable channel, so a
field rides the snapshot — it has to converge after a loss — and an event takes the channel its
caller asked for. The declaration is a Lua table, read once into a C++ schema. `entt::meta` is not
on this pass. [scene.md](scene.md) forbids a by-value `meta_any` at 30 Hz.

`Replicated` sits on the table the script returns. The types are `bool`, `u8`, `tick` (a u32) and
`str16`. At most 16 fields. Names are sorted, and the index on the wire is that position. Each
field is a 4-bit index, a 2-bit type (`0` bool, `1` u8, `2` tick, `3` str16) and the payload: one
bit, 8 bits, 32 bits, or a 5-bit length and at most 16 bytes. A count of changed fields, 5 bits,
precedes them. The baseline merges by index, so a later diff does not resend a field that held.
Nil on the server reads as the zero of the type.

The client does not write these during `ApplyPacket`. That runs before `LuaScriptSystem`
instantiates a prefab spawned in the same frame. `DeliverScriptTraffic` writes them after
`Instantiate` and before `OnUpdate`. The owned pawn is included. Health is not predicted.

`Net.Broadcast(name, args)` queues an unreliable event on the server. A third argument `true`
sends it as reliable type `3` instead. `args` is nil or a flat table of at most 4 string keys,
each a bool, a number, a string or a `Vec3`. A nested table is skipped. Numbers are f32. The name
is at most 16 bytes, a key 12, a string 16. The event is delivered to that same entity on each
client as `OnNetEvent(name, args)`.

Unreliable events are the snapshot trailer: a 4-bit count, then each event. They are not in the
baseline and they are not retried. A lost snapshot loses them. At most 8. If they do not fit in
what the records left of the 1179-byte body, they are dropped. Records win.

`Net.Shot(entity, muzzle, direction, speed)` is an unreliable trailer event of a different kind:
six floats, a speed, and the server tick, plus the shooter's net id. The client calls
`OnShot(muzzle, direction, speed, tick)` unless that entity is locally controlled. The owner's
cosmetic is spawned from its own `OnTick` when `cmd.Fire` is set, and the echo would draw a
second one. The cosmetic cannot damage anything. The server owns the hit. A round is not an
entity. Replicating each one was rejected: the bandwidth is the whole point of the event.

A reliable event whose entity has not spawned is held for about a second (60 frames) and then
dropped, with a log. An unreliable one for an unknown entity is dropped at once. A call to
`Broadcast` or `Shot` on a client or in a standalone scene logs once and returns. Both role
queries are false there, and there is no second client to tell.

`--net-test=fields` spawns `assets/prefabs/NetEcho.gprefab` for each account. The script is
`assets/scripts/NetEcho.lua`. The measured run is in
[NETCODE.md](../ToDo/NETCODE.md#n5--the-game).

## Commands

Kind `5`. Unreliable, like a snapshot: a newer packet carries the commands the older one had, so a
loss does not need a resend. The body is a count byte, 1 to 4, then that many commands, oldest
first. Each command is 13 bytes: u32 tick, i16 move X, i16 move Z, u16 yaw, u16 pitch, u8 buttons.
The sticks are `clamp(v, -1, 1) * 32767`. Yaw is the full-turn uint16. Pitch is the signed-angle
uint16, the same pack as an Euler axis. Buttons are bit 0 jump, bit 1 fire, bit 2 melee, bit 3
interact, and bits 4–7 the equip slot, 0–15.

**What a command holds.** The stick is the keyboard: `W` is move Z −1 (forward), `S` +1, `A` move X
−1, `D` +1, normalised so a diagonal is not faster. Yaw and pitch are the view: the mouse delta,
0.002 radians a pixel, summed once a frame, pitch clamped to ±1.4. **Look accumulates only while
the cursor is captured** (`Cursor.Locked`), so a released cursor can move over a menu without
turning the player. The mouse buttons are held to the same rule, and one more: after the cursor
is captured they count only once they have been seen up, so the click that captured it is not a
shot. Space is jump, the left button fire, the right button melee, `E` interact, and
the number keys the equip slot. The script decides what the stick means: a pawn reads it in its own
frame, by its yaw.

The client builds one of these at the end of every predicted tick, from the last four ticks it has
stored, and `SendCommands` submits it on the main thread. The door sends it. The server's door
queues the body after `Receive`, and `BeginStep` drains the queue under the same lock before it
runs that tick's scripts. A command older than 128 ticks is dropped on the way in. The map keeps
at most 128, and applied ticks older than 8 are dropped once the tick has been consumed.

The server looks up the command for the tick it is about to simulate. A miss repeats the last
command it did apply, and counts the repeat. A constant walk survives that: the repeated command
is the same walk. A direction change does not, which is what the loss check measured. Offline,
nothing is sent. `BeginStep` samples `Input` once per step and hands that command to every script.
The runtime's predict modes ignore the keyboard and hold a synthetic stick, so a headless client
can walk without one. The server does not invent a stick. It applies the command that arrived for
that tick.

`OnTick` sees the floats and the bools, not the packed bytes. The object is only valid for the
call. [scripting.md](scripting.md#the-tick) is the hook.

## Interpolation

`GanymedRuntime --connect` constructs the scene as `SceneRole::Client` before `OnRuntimeStart`.
Client registers the same systems as Standalone. What changes is `GetRole()`.

`PresentWorld` runs at the start of `RuntimeLayer::OnUpdate`, before `OnUpdateRuntime`. A snapshot
spawn is `CreateEntity`, and that asserts once a system update has set the updating flag. The
drain is the packets `PumpClient` queued earlier in the same frame.

Replicated entities on a client are kinematic, apart from the one this client owns.
`AdoptReplicatedMotion` turns a dynamic body into a kinematic one before the step.
`SyncTransforms` and the character step skip a replicated entity the client does not control, so
the physics writeback does not overwrite the blend. A kinematic body already copies the transform
in during `Step`. The owned character is stepped by prediction, and `SyncTransforms` writes it at
alpha 1. Its smoothing is the visual-error decay, not a blend against the previous pose. The
ground state is read for the snapshot. The only write is the reconciliation restore, through the
derived helper in [physics.md](physics.md#replaying-a-character).

The render tick is the estimate minus `0.100 / FixedTimestep`, which is 6 ticks at 60 Hz. The
entity is the blend of the two buffer samples around that tick. The buffer keeps 32 samples per
entity. When the render tick is past the newest snapshot, the frame is dry: the pose extrapolates
the last segment by at most one snapshot interval (2 ticks) and then holds, and the frame is
counted. Counting starts after one second of summed frame time, so the wait for the first
snapshot is not a dry run. Every entity gets a sample from every applied snapshot, moving or not,
so an idle entity's buffer fills to 32 like a moving one (`depth`). Holding that
sample while the render tick is still behind the newest packet is not dry. An entity is
extrapolated only when the frame is dry and its own newest sample is within 2 ticks of that
packet.

Once a second, while a test is running:

```
Replicate server peer={account} tick={n} entities={n} registry={n} snap={bytes} min={bytes} Bps={payload bytes in the last 1s}
Replicate client tick={floor of the render tick} est={.1f} lead={rtt/2} snap={body bytes} depth={max samples} dry={percent} step={max ratio} entities={n} registry={n} ghosts={n} frames={n} stale={n} nobase={n}
```

`step` is the largest ratio, since the warmup, of how far an entity was drawn this frame to how
far the snapshot segment under the render tick says it should have moved across the ticks the
render clock advanced. A continuous blend sits near 1. A teleport is many times that. `dry` is
the percentage of warmed frames that ran past the newest snapshot. `stale` counts snapshots
dropped because they were no newer than one already applied. `Bps` counts payload bytes at
the moment the snapshot is queued, not on the wire, so the conditioner's loss is not in it.

## Prediction

The client and the server share one tick number. The snapshot's tick is the command's
acknowledgement: it is the tick the server had simulated when it captured that pose. The owned net
id in the header is how the client learns which entity that is. `IsLocallyControlled()` is true
for every entity offline, false for every entity on the server, and true on a client only for that
one. The first snapshot of it is written onto the body before prediction starts. Otherwise the
capsule is created at the prefab origin and every later comparison is a metre off.

`PredictLocal` runs once per frame, after `SyncBodies` and before the accumulator. While the
client's sim tick is behind `floor(estimate) + lead`, and at most eight steps in one frame, it
samples the command, calls `OnTick` on the owned entity, steps that character, stores the command
and the resulting position, velocity and ground, and sends the packet. The history is 64 ticks,
about a second. The accumulator's character step skips every client replicated character, including
this one, so it is not stepped twice. Other scripts are not ticked on the client. On the server
and offline, `OnTick` runs inside the accumulator, before `Step`, once per physics step.

On each snapshot the client takes the newest sample of its character, which carries the full
decoded position, velocity and ground at that snapshot's tick, and compares it with the history
entry for the same tick. The tolerance is 2 cm of position, 0.1 m/s of
velocity, and the ground enum exactly. 0.1 m/s is under one tick of gravity (9.81/60 = 0.1635),
so a ground miss still fails it. Inside the tolerance the tick is consumed and nothing is counted.
Outside it, the character is restored to the server's position, linear velocity and ground — not a
Jolt `SaveState` blob — the history slot is rewritten to that pose, and the stored commands from
the next tick through the sim tick are replayed. Input is not sampled again. The sim tick does not
advance. A correction is counted only after the first second of prediction (`SettleFrom`); the
settle before that is logged with `settle=1` and left out of the total. The test sticks start
walking at that same tick.

The visual error is where the mesh was before the restore, minus where the character is after the
replay. It is added to the transform after the final `SyncTransforms` and multiplied by
`exp(-frameDt / 0.1)`, about 100 ms. The next prediction reads Jolt, not the offset, so the decay
cannot feed back into the step.

`OnTick` during that replay is the same call with the stored command. It has to be safe to run
again. Movement belongs there. `OnUpdate` stays once per frame and is not replayed, which is where
presentation goes when N5 splits the game scripts. `assets/scripts/Player.lua` has no `OnTick` and
is unchanged.

The checks live on `--net-test`. `predict` holds forward. `predict-loss` reverses every 30 ticks.
`predict-desync` teleports the pawn +3 m on X at 180 ticks after spawn, on an even tick so the
snapshot includes it. `predict-wall` walks diagonally into a wall. `predict-step` walks at a 0.2 m
step. `predict-fire` is the runtime's alone: it stands, turns a full circle every 12 s, and holds
the trigger 2 s in every 3, against whatever scene the server runs. The floor, the wall and the step are static boxes created the same way on both sides. They
are not replicated; collision does not care that the UUIDs differ. The floor covers the 240 m a
4 m/s walk covers in 60 s. A 20 m pad ends that walk in the air. The pawn is
`assets/prefabs/NetWalker.gprefab`, spawned by the server when the account appears. The client
receives it as a normal prefab spawn. `offline` builds the same floor in a standalone runtime,
spawns the walker, and strips `ReplicatedComponent` so the ordinary character step runs it.
Headless is what keeps the client at 60 Hz. A windowed debug runtime on the demo scene does not.

Once a second, and once when the client is done:

```
Predict server tick={n} x={.3f} y={.3f} z={.3f} repeats={n} walkRepeats={n}
Predict client tick={n} corrections={n} maxMm={.0f} replay={n} leadTicks={n}
Predict correct tick={n} sizeMm={.0f} replay={n} settle={0|1}
Predict done corrections={n} maxMm={.0f} replay={n}
```

`repeats` counts every tick that did not have its own command. `walkRepeats` starts when the
first non-zero stick is applied. `replay` is how many newer commands were re-simulated, which is
the lead and not the round trip.

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
Net {who}: rtt {ms} ms, loss in {%}% out {%}%, {B/s} in, {B/s} out, resent {n}, dropped session {n}, address {n}, reliable refused {n}
```

`Net.GetStats()` returns `connected`, `peers`, `rtt`, `lossIn`, `lossOut`, `bytesInPerSec`,
`bytesOutPerSec`, `resent`, `droppedSession`, `droppedAddress`. On a client that is its one
session. On a server the round trip and the loss are means, and the byte rates and `resent` are
sums. With no session every number is zero and `connected` is false. The counters are `uint64`
in C++ and `int64` in Lua, the same cast as a UUID: sol2 throws on a `uint64` above `INT64_MAX`.
The door thread writes a snapshot under a mutex. The frame only reads the snapshot.

`Net.IsServer()` and `Net.IsClient()` read the scene role. Both are false when the scene is
standalone, which is offline play and the editor. `OnTick` is the script hook in the next section.
