# Milestone — Netcode (a dedicated server, and clients that play on it)

**Status: N0 done (2026-10-10); N1–N5 planned, nothing built.** It follows [ONLINE.md](ONLINE.md), whose last
phase, O5b, becomes N1 here. It depends on `online-o4` (O4 and O5a) reaching `master`. N1 is
written against the backend's `api-v0.6` (results through the agent, `DENIED` before allocation),
which is built.

The goal is the Proving Ground played co-op by two to four people. Each person runs
`GanymedRuntime`, the backend matches them, and the match runs on one `GanymedDedicated` that the
backend's fleet allocated. The server is authoritative: it runs the simulation, and the clients send
input and show what it sends back. The match ends with a victory or a defeat that the server
reports, and the players' ratings change.

This is a learning milestone like the others. It is also the largest one this engine has had, and
most of its cost is not the networking: it is making the engine run without a window, and making a
game written as one player's per-frame Lua into a simulation that can run on a server. The sizes
below say so.

---

## How production engines do it, and where Ganymed diverges

There are three families. They are worth separating, because the choice between them shapes
everything after it.

- **Deterministic lockstep.** Every machine runs the whole simulation from the same inputs, and only
  inputs travel. RTSs use it (Age of Empires; StarCraft), and so does fighting-game *rollback*
  (GGPO), which predicts the remote inputs and re-simulates the whole game when a guess was wrong.
  Bandwidth is tiny and independent of world size. In exchange, the simulation must be bit-identical
  everywhere, and one divergence is a desync that nobody can repair. A late joiner needs the whole
  state.
- **Snapshot interpolation with client-side prediction.** The server alone simulates. It sends
  snapshots of the state, and clients render other entities a little in the past, interpolating
  between two snapshots. The client simulates only what it controls, ahead of the server, from its
  own inputs, and corrects itself when the server disagrees. This is the shooter lineage: Quake 3
  (delta-compressed snapshots over unreliable UDP, acknowledged per client), Source (a fixed tick,
  interpolation 100 ms behind by default, prediction, and lag compensation that rewinds hitboxes),
  and Overwatch (Tim Ford's GDC 2017 talk: an ECS, command frames, and prediction of the local hero's
  abilities).
- **State replication by property, with RPCs.** Unreal's model, from the same lineage but organised
  around objects: an actor marks properties as replicated, the server sends what changed, filtered
  by relevancy and priority, and gameplay calls RPCs on the server or on clients. Prediction is per
  feature: `CharacterMovementComponent` predicts and corrects movement, and the Gameplay Ability
  System predicts abilities.

Two more references matter, because they are the readable versions of the above. Glenn Fiedler's
*Gaffer On Games* articles (snapshot interpolation, state synchronisation, deterministic lockstep,
and reliability over UDP with sequence numbers and an ack bitfield) and his libraries (`netcode`,
`reliable`, `yojimbo`) are the clearest description of the transport this plan builds. Rocket League
is the counter-example: it predicts every physics object, not only the local car, because a ball
that is interpolated in the past cannot be hit where it is drawn.

**Ganymed takes the second family**, with Unreal's vocabulary where it helps (relevancy, replicated
properties) and none of its machinery:

- **The backend already chose the topology.** It allocates a dedicated server per match and admits
  players with a connect token. Lockstep needs no server, and listen servers need no fleet.
- **Lockstep does not fit the engine.** Jolt has a cross-platform-deterministic build mode, at a
  cost, but the game's simulation also runs in Lua, uses `math.random`, and steps scripts on a
  variable frame time. Making all of that bit-identical across Windows and Linux is a project of its
  own, and the reward (low bandwidth for thousands of units) is not what a 4-player shooter needs.
- **Rollback of the whole world does not fit either.** It means re-simulating Jolt, every script and
  every animation several times a frame. Prediction here is limited to the one thing a player
  controls directly: their character's movement.

## What it deliberately is not

- **No lag compensation.** It exists so that a hitscan shot fired at an interpolated enemy hits where
  the shooter saw it. The Proving Ground has no hitscan (its Decision 2): rounds are simulated
  projectiles, and the server simulates them. In co-op against AI, an enemy that is 100 ms behind on
  screen is a tolerable cost. A PvP mode would reopen this.
- **No prediction beyond movement.** Firing, melee and pickups happen when the server says so. The
  local player sees their own shot at once anyway, because the cosmetic projectile is spawned locally
  (N5). Predicting damage, deaths or pickups needs rollback of gameplay state, and this plan does not
  take that on.
- **No encryption or packet authentication after the handshake.** The connect token proves who may
  join; the packets after `WELCOME` carry a random session ID and nothing stronger. netcode.io
  encrypts every packet with keys carried inside the connect token, and this contract's token has
  none. It is acceptable on localhost and for a learning project. It is recorded under
  [Design tensions](#design-tensions-recorded).
- **No interest management or bandwidth budgeting**, beyond delta compression and quantization.
  Four players and a few dozen enemies on one map fit without it. The measurements in N3 say whether
  that is true.
- **No host migration, reconnection to a running match, or spectators.** A client that drops is
  gone. The backend's ticket already ends the player's match when the server reports the result.
- **No editor multiplayer play-in-editor.** The development loop is the dedicated server plus
  runtime clients on one machine (N2's `--dev` mode). Unreal's "play as N clients" in one editor
  process is a large feature and is not needed to learn any of this.

---

## Decisions, with reasoning

1. **Simulation tick: 60 Hz, the physics step.** `PhysicsSettings` already steps Jolt at 1/60 s
   with an accumulator ([physics.md](../engine/physics.md#the-step)), so the network tick is that
   step, numbered. A tick number is the shared clock: inputs are tagged with the tick they apply to,
   snapshots with the tick they describe. A lower tick (30 Hz) halves the server's cost, but makes
   prediction coarser and needs a second step rate. Not worth it at this scale.

2. **Snapshots at 30 Hz, inputs at 60 Hz in packets of up to 4 commands.** A client sends every
   tick's command, plus the previous three, in every packet. A lost packet then costs nothing as long
   as the next one arrives: the classic Quake/Source redundancy, and cheaper than a reliability
   layer for inputs. Snapshots every second tick keep bandwidth down. Interpolation then needs to
   run about two snapshot intervals behind (≈ 67 ms) plus jitter, so the starting interpolation delay
   is **100 ms**, Source's default, and it is measured rather than tuned by feel.

3. **Transport: our own thin protocol over UDP, no new dependency.** The alternatives:
   - **ENet**: a small, mature C library with reliable and unreliable channels. A reasonable
     production choice. But this protocol is almost all *unreliable* (snapshots and inputs, each
     superseding the last), and the reliability it needs is the ack-bitfield kind, which ENet hides.
   - **Valve's GameNetworkingSockets**: excellent and heavy (protobuf, a crypto library). Too much
     for what is used.
   - **yojimbo / netcode.io**: the closest in spirit, but netcode.io's connect token is its own
     format with encryption keys inside, and the backend's token contract is already fixed.

   Writing it is also most of what there is to learn. The packet header is Fiedler's: protocol ID,
   session ID, sequence, the newest acknowledged sequence, and a 32-bit ack bitfield. That gives
   per-packet acknowledgement, RTT and loss for free, and lets the server delta-encode against the
   last snapshot each client acknowledged.

   **The socket is new code too.** IXWebSocket's `ix::UdpSocket` (which O5a's `JoinMatch` uses) is
   client-only: it has no `bind`, `recvfrom` does not return the sender's address, and it is IPv4
   only. A server needs all three. A small wrapper over Winsock and BSD sockets (bind, non-blocking
   send/receive with addresses, about 150 lines) lives in a new `Net/` module.

4. **Serialization: a hand-written bit packer, one function per message for read, write and measure.**
   Fiedler's "serialize function" pattern: a templated `Serialize(Stream&)` per message, instantiated
   for a write stream, a read stream and a measure stream, so the reader and the writer cannot
   disagree. Floats are quantized where the range is known (positions to millimetres within the map's
   bounds, angles to 16 bits). No protobuf or flatbuffers: their schemas solve versioning between
   separately deployed services, and both ends of this protocol are built from one commit.

5. **Replication codecs are explicit per component, not generic over reflection.** The engine has
   member reflection, and it is tempting to replicate "every reflected field". But
   [scene.md](../engine/scene.md#the-one-discipline-line) forbids any engine system from reading a
   component through `entt::meta`, because reads go through a by-value `meta_any` that allocates for
   anything larger than a few bytes. A replication pass touches every replicated entity at 30 Hz, so
   it is exactly what that rule is for. Instead, a compile-time `ReplicatedList` (the
   `ComponentList` pattern) names the replicated components, each with a `NetSerialize` function,
   like Unreal's `NetSerialize`. Few components replicate:
   - the transform (position and yaw for characters, full rotation for others);
   - the character's movement state (velocity, and the ground-state enum — N0, decision 9);
   - the aim-offset angles;
   - the replicated script fields of decision 7.

6. **Animation is derived, not replicated.** Each machine chooses clips from replicated *gameplay*
   state (velocity, grounded, weapon, "attacking since tick T") exactly as `Player.lua` and
   `Enemy.lua` already do from local state. Replicating clip times would cost bandwidth and still
   look wrong after a correction. The server runs animation too, because melee hits are traced from
   animated weapon points (the melee hit detection section of PROVING_GROUND.md on `first-game`). A remote
   player's clip phase may differ slightly between machines. That is cosmetic.

7. **Script state is replicated through declared fields, and scripts know their role.** Most game
   state lives in Lua tables (health, ammo, the current weapon), not in components. A script declares
   what replicates:
   ```lua
   local Player = {}
   Player.Replicated = { health = "u8", weapon = "str16", attackTick = "tick" }
   ```
   The server reads those fields from the instance's table each snapshot, and writes them into the
   client's instance before its `OnUpdate`. There is a small fixed set of types; no table or
   function values replicate. Roles are explicit:
   - `Net.IsServer()` and `Net.IsClient()` (both false when playing offline);
   - `self.entity:IsLocallyControlled()`, for the client's own player;
   - a new hook, `OnTick(tick, command)`, which runs once per simulation tick.

   **Simulation moves from `OnUpdate` to `OnTick`.** `OnUpdate` keeps presentation (camera, HUD,
   sound choices).

8. **Input is a command, not a keyboard.** Today a script calls `Input.IsKeyPressed(Key.W)`, and
   there is one keyboard. A server has none and four players. Each client samples a `PlayerCommand`
   per tick: a movement vector, view yaw and pitch, and buttons (fire, melee, jump, equip slot,
   interact). `OnTick` receives it. On the client the command is built from `Input`; on the server
   it comes from that player's packets. Offline, the same code runs with a locally built command, so
   a game written this way still plays single-player in the editor.

9. **The local character is predicted by re-running `CharacterVirtual`, not the world.** When a
   snapshot acknowledges tick T, the client sets its character's position, linear velocity and
   ground state to the server's for T, then replays its unacknowledged commands T+1…now. Each
   replay is one `CharacterVirtual::ExtendedUpdate` per command, against the current world, with no
   `PhysicsSystem::Update`. A `CharacterVirtual` is not stepped by the physics system
   ([physics.md](../engine/physics.md#character-controllers)), which is what makes this cheap.
   Errors under a threshold are smoothed out over a few frames rather than snapped. Other players
   and enemies are interpolated and *kinematic* on the client, so the local character collides with
   them where they are drawn, which is slightly in the past. That is the standard compromise, and
   N0 measured what it costs.

   **What the reset copies (N0, 2026-10-10).** Position and velocity, which this decision originally
   named, are not sufficient. The ground enum has to be set as well. `mGroundState` on
   `CharacterBase` is protected, defaults to `InAir`, and has no setter, so N4 writes it through a
   small derived helper. Do not ship `SaveState` / `RestoreState`. The blob stores `BodyID`s
   (`mGroundBodyID`, and `mBodyB` on each contact), and a body id belongs to one `PhysicsSystem`.

   The spike ran 120 ticks at 1/60 s with the engine's character step: gravity integrated by hand
   only while the state is not `OnGround`, then `ExtendedUpdate` with a step-up of 0.4, capsule
   radius 0.35 and half-height 0.6, and the same layer filters as `PhysicsScene::StepCharacters`.
   It recorded tick 60 and replayed commands 61–120. On a static world, a character given the
   checkpoint's position, rotation, velocity and ground enum matched the reference on every
   replayed tick, to the bit. That held for a fresh character with an empty contact list, and for
   one that still held the contact list from tick 120. The scenes were a walk, a jump whose
   checkpoint was `InAir`, a wall slide, and a 0.2 m step. Rotation was restored and stayed the
   spawn quaternion, so it was not the field the match depended on. `OnSteepGround` and
   `NotSupported` never occurred; replicate the enum (four values), not a bool.

   The ground enum is the field that matters. Replaying the airborne checkpoint while the object
   was still `OnGround` diverged on tick 61: max position error 0.551 m, max velocity error
   4.09 m/s, 10 ticks whose ground state disagreed, and both runs had landed by tick 120. A fresh
   character, which starts `InAir`, replaying a grounded pose integrated one tick of gravity
   before `ExtendedUpdate` found the floor: velocity error 0.1635 m/s, which is 9.81/60, and a
   position error of about 1e-7 m. The ground state of the later ticks still matched.

   MSVC x64 Debug and g++ 11.4.0 `-O2` (WSL) printed the same poses and the same exact/inexact
   rows for all five scenes. That is these scenes, not a proof that every Jolt scene agrees
   across the two compilers.

   **A moved world is a different miss, and character state does not fix it.** A kinematic
   platform rose 1.2 m over the replayed half, 0.02 m per tick. Pose, the ground enum, a full
   `SaveState` restore and a fresh character all missed by 1.18 m on tick 61, because
   `ExtendedUpdate` saw the platform already at its tick-120 height. The character then stuck to
   that height, so the two end positions nearly met. The case that matched was a full `SaveState`
   restore together with the platform put back to its tick-60 pose and moved the same way during
   the replay, still with no `PhysicsSystem::Update`. Pose plus the ground enum was not re-measured
   against that rewound platform. A static map does not need a snapshot of the physics world. A
   body the character is standing on, which has moved since T, does — and remote players are that
   case, kinematic and drawn in the past. N4 does not grow a world snapshot to cover it.

10. **Projectiles: the server's are real, the clients' are cosmetic.** The weapons fire many rounds
    a second (the minigun's rate is the stress case), and replicating each round as an entity would
    dominate bandwidth. The server simulates rounds as today and owns every hit. Clients receive a
    *shot event* (muzzle, direction, speed, tick) and spawn a cosmetic round that cannot damage
    anything. The shooting client spawns its own at once, from its predicted muzzle, and ignores the
    echo of its own shot.

11. **Network identity: a 16-bit index per replicated entity, mapped to a UUID at spawn.** Scene
    entities already share UUIDs on every machine (the same `.ganymede` file). Entities spawned
    during play are created on the client with the server's UUID, through
    `CreateEntityWithUUID`. A prefab's **children** are the hard case: `Scene.Spawn` mints fresh UUIDs
    for every child, so a client spawning the same prefab would name the weapon or the body
    differently. Child UUIDs are therefore derived deterministically from the root's UUID and the
    child's index in the prefab, on both sides. That is an engine change to spawning, made in N3.

12. **A development mode that bypasses the backend.** Going through matchmaking costs a 10 s fill
    wait per test, and needs the stack, the agent and two profiles. `GanymedDedicated --dev
    --scene=…` listens on `127.0.0.1` only, does no agent lifecycle, and admits `HELLO dev:<name>`
    without a token. `GanymedRuntime --connect=127.0.0.1:<port>` joins it. **`--dev` refuses to bind
    anything but loopback**, so it cannot become an open server by accident. The real path stays the
    acceptance test of every phase that touches it.

13. **A network conditioner is part of the transport, from the first packet.** `--net-sim=` on both
    executables adds latency, jitter, loss, duplication and reordering on send, in-process. Unreal
    has the same (`PktLag`, `PktLoss`). Without it, everything works on localhost and nothing is
    learned. Every verification table below runs under it.

---

## Phases

| Phase | What | Branch | Rough size |
|---|---|---|---|
| **N0** | Spikes: headless boot, `CharacterVirtual` replay, the Ed25519 library | throwaway, done 2026-10-10 | — |
| **N1** | `GanymedDedicated`: a headless app, a Scene role, the agent lifecycle, token verification, the result (**was O5b**) | master | about a week |
| **N2** | Transport: sockets, connections, the packet header and acks, the conditioner, `--dev` | master | about a week |
| **N3** | Replication: ticks, network IDs, spawn and destroy, snapshots with delta compression, interpolation | master | 1½–2 weeks |
| **N4** | Input and prediction: commands, `OnTick`, the predicted character, reconciliation | master | 1½–2 weeks |
| **N5** | The game: script roles and fields, shot events, co-op rules, the match result through the backend | master (engine hooks) + `first-game` (the game) | 2–3 weeks |

**These are estimates, and probably low.** All of it adds up to two to three months of the pace
ONLINE.md ran at. N5 is the one most likely to grow: `Player.lua` alone is about 1,800 lines of
per-frame, single-player Lua, and it has to be split into tick simulation and frame presentation.
The milestone stops being a plan and becomes a playable result only at the end of N5. N1 is the
exception, and is worth doing first on its own merits: it finishes ONLINE.md, and the backend's
acceptance test exists already.

**Engine work lands on `master`** (its own branch per phase, merged in), and reaches `first-game` by
merge, under the Proving Ground's branch policy. N5's game changes are the only ones on
`first-game`.

### N0 — spikes (throwaway, nothing merged) — done 2026-10-10

Three questions, each answered by a throwaway program that was deleted afterwards. Nothing in the
engine changed. The programs called bgfx, Jolt and Monocypher directly; they did not boot
`Application`.

1. **A null window works with `Noop`.** `bgfx::init` with `RendererType::Noop` and
   `platformData.nwh`, `ndt`, `context` and `backBuffer` all null succeeded on Windows x64 Debug
   and on Linux (WSL, g++ 11.4.0), at 1280×720 and at 0×0. The calls a mesh and shader load makes
   returned valid handles: vertex, index and dynamic vertex buffers, an RGBA8 2D texture, a cube,
   a uniform, a framebuffer, a real `vs_FlatColor.bin` / `fs_FlatColor.bin` (dx11 on Windows, glsl
   on Linux) and the program built from them. `frame` and `shutdown` succeeded, and the fatal
   callback did not fire. bgfx 1.151.9149 (`f446c319`). `Noop` reports NDC depth [0, 1] and a
   top-left origin, which is what the engine assumes. `Noop` is excluded from bgfx's headless
   mode — that mode is a real renderer with a null window, and it rejects a non-zero size — which
   is why 1280×720 was accepted. N1 still passes a nominal non-zero resolution, so code that
   divides by the width stays safe. The fallback, a `Renderer::IsGpuAlive() == false` path through
   every asset apply, is not required for resource creation. What remains is wiring:
   `BgfxContext` asserts a non-null `GLFWwindow`, and `Application` always creates the window, the
   audio engine and the UI. That is N1 step 1. The spike did not run `AssetManager` or load a scene.
2. **Replay matches when the ground enum comes with the pose.** 120 ticks, checkpoint at 60,
   commands 61–120 replayed with `ExtendedUpdate` only. On a static world, position, rotation,
   linear velocity and `EGroundState` reproduced the reference to the bit: a walk, a jump whose
   checkpoint was `InAir`, a wall slide, and a 0.2 m step. The contact list was not required.
   Position and velocity alone missed the jump by 0.551 m. A kinematic platform that rose 1.2 m
   during the replayed half missed by 1.18 m even from a full `SaveState`. It matched when that
   full state was restored and the platform was put back and stepped the same way, still without
   `PhysicsSystem::Update`. MSVC x64 Debug and g++ 11.4.0 `-O2` printed the same rows. What N4 copies, and what it refuses to
   copy, is decision 9. `OnSteepGround` and `NotSupported` did not occur.
3. **Monocypher 4.0.3, the SHA-512 files.** `crypto_ed25519_check` verified the RFC 8032 empty-message
   and `"r"` vectors, and a token minted by the backend's Go `connecttoken.Mint`, on Windows and
   on Linux. The signature is over the ASCII bytes of the encoded payload. `crypto_eddsa_check`
   (BLAKE2b) rejected those same signatures, which is how the build proves it linked the SHA-512
   variant. Flipping signature byte 0, and flipping payload byte 0, were both rejected.
   SHA-512(`"abc"`) matched the known digest. Tarball sha256
   `8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66`. It is vendored in N1, not
   here. The two source files and the call are in N1 step 5.

**Done.** The answers are in N1 step 1, N1 step 5, decision 9 and N4, and the spike code is deleted.

### N1 — `GanymedDedicated` (was ONLINE.md's O5b)

#### Goal

A third executable, `GanymedDedicated`, that runs under the backend's fleet agent exactly as
`stubserver` does. It boots headless, waits for an allocation, loads the match's scene and runs it,
admits players with `HELLO`/`WELCOME`, and reports a result. There is no gameplay networking yet:
admitted players are counted, not played with. The result is decided by a timer, as the stub's is.

#### Steps

1. **`ApplicationSpecification::Headless`.** No `Window`, no ImGui, no `UIEngine`, no `AudioEngine`.
   The renderer is `bgfx::RendererType::Noop` with `platformData.nwh` (and `ndt`, `context`,
   `backBuffer`) null, after the `bgfx::renderFrame()` the engine already calls before `init`.
   Pass a nominal non-zero resolution; the spike used 1280×720, and bgfx also accepted 0×0.
   `BgfxContext`'s null-window assert and `Application`'s unconditional window go away on this
   path. Resource creation does not need a `IsGpuAlive() == false` branch (N0). The loop keeps
   its order without the window's `OnUpdate`. A headless app paces itself: it sleeps to the next
   tick instead of waiting on vsync, so it does not spin a core.
2. **A Scene role**: `Standalone` (today), `Server` or `Client`. `Server` skips `CameraSystem`,
   `AudioSystem`, `ParticleSystem` and `RenderSystem`, and keeps physics, scripts, animation,
   transforms and bone attachments. This is gated on the role in `SystemManager`, not done with
   `#ifdef`s. The architecture rule still holds: no compile-time editor or server code in the engine.
3. **The `GanymedDedicated` project** (premake, Windows and Linux): thin wiring like
   `GanymedRuntime`, with the scene chosen by the allocation (one scene for now, from a config file).
4. **The lifecycle client**, in `Online/`, over O1's transport. Ordered as follows:
   - The spawn flags are `--server-id`, `--agent`, `--game-port`, `--advertise` and `--public-key`.
     Refuse to start without them.
   - Bind the UDP port **before** the first `ready`.
   - `ready` is the long-poll: a per-request transfer timeout over 20 s, which O1 allows.
   - `allocated` goes **before** the scene loads.
   - `health` every 2 s **from a network-thread timer, not the frame loop**, so a slow scene load
     cannot get a healthy server killed.
   - The result, **to the agent** (`POST …/result`, `api-v0.6`), with retry and backoff on network
     errors and 5xx, stopping on 4xx. The server never sees a backend URL or a result token.
   - `shutdown`, then exit.
5. **Token verification**, rules 1–8 of `connect-token.md` in order: the signature before any claim,
   5 s of leeway, a nonce map with expiry (nonces only, not addresses: see below). Connect tokens are
   credentials: never logged. The verifier is Monocypher 4.0.3 (N0). Compile `src/monocypher.c` and
   `src/optional/monocypher-ed25519.c`, and call `crypto_ed25519_check` (0 means valid) on the
   ASCII bytes of the encoded payload. Do not call `crypto_eddsa_check`: that hashes with BLAKE2b
   and rejects an RFC 8032 signature. The tarball sha256 is
   `8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66`.
6. **Replies before allocation**: read the game port from the moment it is bound, and answer `HELLO`
   with `DENIED not allocated` until an allocation arrives (`api-v0.6`).

#### The backend contract N1 is written against: `api-v0.6`, decided 2026-10-10

**Built in GanymedServer on 2026-10-10** (its `server-lifecycle.md` and `docs/backend/fleet.md`
have the details and the measurements). N1 reads the `api-v0.6` tag.

- **The result goes through the agent.** Under `api-v0.5` a server posted to `result_url`, which
  reached one backend replica only, so a replica that is down for longer than the server's retries turns a
  finished match into `server_lost`. The agent already fails over between replicas on every call.
  Through it, the game server's only HTTP peer is its local agent: the Agones shape. The rejected
  alternative, a load balancer in Compose, is recorded in the backend's ToDo.
- **`DENIED not allocated` before allocation** (step 6), instead of the stub's silence, which cost
  O5a's client two timeouts against a replacement server.
- **Declined: answering a repeated token from the same address**, netcode.io's rule. Our client
  never resends a token: it re-reads the ticket for a fresh one (O5a), and a server admits an
  already-admitted player again. So the nonce map stores nonces only. The reason is recorded under
  rule 8 in the backend's `connect-token.md`.

#### Verification

| Check | Pass when |
|---|---|
| The backend's own acceptance test | `fleetagent -- GanymedDedicated` with `gscli load coop 4` passes the four stages it passes against `stubserver`: queue drained, servers ready, `WELCOME:4`, finished with results |
| Engine clients | two `GanymedRuntime` players, through O5a's `JoinMatch`, get `WELCOME`, then `match.finished` with a rating change |
| Tokens | a Go-minted token verifies; one changed byte, an expired token, the wrong `server_addr` and a replay are each `DENIED` with the rule's reason |
| Slow load | a scene load stretched to 8 s (an artificial delay) does not get the server killed: health continued throughout |
| Headless | no window opens; Linux (WSL, no display) and Windows both run; CPU use when idle is near zero, not one core |
| Crash | killing the server mid-match gives the players `ticket.failed` `server_lost`, as with the stub |
| Replica down | with backend replica A stopped before the result, the result arrives through B and the players get `match.finished` |
| Before allocation | a `HELLO` to a ready, unallocated server gets `DENIED not allocated` at once |

**Docs:** a new `docs/dedicated/dedicated.md` (the app, its flags, its lifecycle, like
[runtime.md](../runtime/runtime.md)), indexed from [docs/README.md](../README.md);
[online.md](../engine/online.md) for the lifecycle client and token verification;
[architecture.md](../engine/architecture.md) and [ecs.md](../engine/ecs.md) for headless and the
Scene role; [build-and-tooling.md](../engine/build-and-tooling.md) for the Ed25519 dependency.
ONLINE.md's O5b closes with this phase.

### N2 — transport

#### Goal

A client and a server exchange packets over a connection that knows its round-trip time and loss,
under simulated bad networks, with no game state in them yet.

#### Steps

1. **`Net/Socket`**: bind, non-blocking `sendto`/`recvfrom` with addresses, IPv4 to start. Winsock
   and BSD behind one interface. On Windows, a UDP socket whose sent datagram draws an ICMP port
   unreachable reports an error on its next receive (`WSAECONNRESET`), as O5a measured. A *server*
   socket must not treat that as fatal, because it would come from one client's dead port. Disable
   it with `SIO_UDP_CONNRESET`, or ignore it.
2. **`Net/Connection`**, after `WELCOME`:
   - **Session.** The client's first game packet carries the account ID that `WELCOME` named. The
     server answers with a random 64-bit session ID that every later packet carries, and drops
     packets with the wrong one. The token handshake stays exactly as the backend's contract says.
     The game protocol is engine-to-engine and is not part of `docs/api/` in GanymedServer. It is
     documented in this repo.
   - **The socket carries over from `JoinMatch`.** O5a closes its socket after `WELCOME`, and the
     server knows the client by the address it sent from, so a new socket means a new port and an
     unknown client. `JoinMatch` hands its socket to the connection instead (online.md recorded
     that this was deferred until now).
   - **One socket for every attempt.** O5a also opens a new socket per attempt, so each retry comes
     from a different port. With a handover, all attempts must share one socket. Otherwise a
     `WELCOME` that was lost on attempt 1 leaves the server holding a port the client has already
     closed.
   - **A player admitted twice keeps the newest address.** A fresh token re-admits an
     already-admitted account (`connect-token.md`, under rule 8). The server then points that
     account's session at the newest address instead of opening a second one.
   - **Keepalive and timeout**: a packet at least every 100 ms each way; 5 s of silence ends the
     connection, and the server tells its game a player left.
   - **Header and acks**: protocol ID, session, a 16-bit sequence with wraparound comparison, the
     newest acknowledged sequence plus a 32-bit bitfield. RTT is smoothed from acknowledgements; loss
     is measured over a window.
   - **A reliable-ordered channel for the few messages that need one** (a player joined, the match
     ended), resent until acknowledged, inside the same packets. Small, because nearly everything
     else supersedes itself.
3. **The conditioner** (decision 13), on both send paths.
4. **`--dev` mode** (decision 12), and `GanymedRuntime --connect=`.
5. **Stats**: RTT, loss in each direction, bytes per second each way, packets resent. Logged
   periodically, and readable from Lua (`Net.GetStats()`) so a HUD line can show them.

#### Verification

| Check | Pass when |
|---|---|
| Connect | `--dev` server and two runtime clients connect; the server logs each session |
| RTT | with `--net-sim=lat:50` on both sides, the measured RTT is 100 ms ± a frame |
| Loss | at 10% simulated loss each way, the measured loss is 10% ± 2 over 30 s |
| Reliable channel | 1,000 numbered reliable messages at 20% loss and reordering arrive complete, in order, exactly once |
| Timeout | killing a client: the server ends its session 5 s later; killing the server: the client reports it within 5 s |
| Through the backend | the real path: matchmaking → `JoinMatch` → the same socket continues into a session with the allocated `GanymedDedicated` |
| Spoofing | a packet with the wrong session ID, or from another port, is dropped and counted |

### N3 — replication

#### Goal

Everyone sees the same world. Players and enemies that the server moves are drawn smoothly on every
client, at 100 ms of latency and 5% loss. Nobody controls anything yet: the server moves the players
along a scripted path for the test.

#### Steps

1. **The tick clock.** The server numbers its physics steps. The client estimates the server's
   current tick from snapshots and RTT, and runs its own clock slightly ahead of it. N4 needs that
   lead, and N3 measures it.
2. **Replicated entities.** A `ReplicatedComponent` marks an entity for replication, and the server
   assigns it a 16-bit network index. Scene entities are matched by UUID. Spawned ones are created on
   the client with the server's UUID, and prefab children get derived UUIDs (decision 11, an engine
   change to `Scene.Spawn` and prefab instancing).
3. **Snapshots.** Per client, the server keeps the snapshots it sent in a ring, keyed by sequence.
   Each new snapshot is delta-encoded against the newest one that client acknowledged, or sent whole
   if there is none. Spawns and destroys are part of the snapshot, so they need no reliable channel.
   The per-component codecs come from `ReplicatedList` (decision 5), and quantization from decision 4.
4. **The client applies snapshots into an interpolation buffer** per entity, and renders at
   `serverTick − interpolationDelay`, blending the two snapshots around it. Replicated entities on a
   client are kinematic, and their physics bodies follow the interpolated transform.
5. **Measurement**: bytes per snapshot and per second per client, the interpolation buffer's depth,
   and how often it ran dry (then the entity is extrapolated for at most one interval, and the
   event is counted).

#### Verification

| Check | Pass when |
|---|---|
| Two clients see the same thing | positions of a scripted mover, logged on both clients at the same server tick, agree within quantization |
| Smoothness | at 100 ms and 5% loss, the buffer runs dry under 1% of frames, and no entity is drawn jumping more than a frame's travel |
| Delta compression | a scene at rest sends snapshots of a few bytes; bytes per second are recorded for the Proving Ground with 4 players and its enemies |
| Spawn/destroy | 500 spawns and despawns at 10% loss: every client ends with the server's entity set, with no leaks |
| Prefab children | a spawned prefab's child UUIDs match between server and client |

### N4 — input and prediction

#### Goal

Each player controls their character with no visible delay at 150 ms of RTT, and the server stays
authoritative: when the two disagree, the client corrects itself, smoothly when it is small.

#### Steps

1. **`PlayerCommand`** (decision 8), sampled once per tick from `Input` on the client, and sent in
   packets of the last four (decision 2).
2. **`OnTick(tick, command)`**, a script hook called once per physics step inside the accumulator,
   before the step. This is an engine change to `LuaScriptSystem` and `PhysicsSystem`, useful offline
   too. On the server, each player's script gets that player's command for the tick. A missing
   command repeats the last one, and the repeat is counted.
3. **Ownership**: the server tells each client which entity it controls. `IsLocallyControlled()`
   follows from it.
4. **Prediction and reconciliation** (decision 9): the client keeps its last ~1 s of commands and of
   predicted states. On each snapshot, it compares the server's state for the acknowledged tick with
   its own prediction for that tick. If they differ beyond a tolerance, it resets the character to the
   server's position, linear velocity and ground state — not a Jolt `SaveState` blob — and replays
   the newer commands with `ExtendedUpdate` only. The visual error is decayed over about 100 ms.
5. **Measurement**: corrections per minute, the correction size, and the replay length in ticks. A
   debug switch forces a desync (the server teleports the player) to prove the path recovers.

#### Verification

| Check | Pass when |
|---|---|
| Responsiveness | at 150 ms RTT, movement starts on the frame the key is pressed (logged tick of input vs first moved frame) |
| No corrections at rest | walking on flat ground at 150 ms and 0% loss: zero corrections in 60 s |
| Loss | at 5% loss: corrections are rare and small (numbers recorded), and no rubber-banding is visible |
| Forced desync | a server teleport is corrected on the client within RTT + one interval |
| Walls and steps | N0 replayed a wall slide and a 0.2 m step to the bit once the ground enum was restored (decision 9). This phase still has to show that, at 150 ms, corrections stay inside the tolerance |
| Offline unchanged | the same scripts play single-player in the editor with no server |

### N5 — the game

#### Goal

The Proving Ground played co-op by two to four players, matched by the backend, with a victory or a
defeat reported by the server and ratings that change.

#### Steps (engine, on `master`)

1. **Replicated script fields** (decision 7): the declaration, the type set, the server reading and
   the client writing before `OnUpdate`.
2. **Server-to-client events**: `Net.Broadcast(name, args)` from a server script, delivered to the
   same script on each client as `OnNetEvent(name, args)`. These are unreliable for cosmetics
   (impacts, sounds) and reliable for the few that matter (death, match end). There are no
   client-to-server RPCs: a client's only voice is its command.
3. **Shot events and cosmetic projectiles** (decision 10).

#### Steps (game, on `first-game`)

4. **Split `Player.lua`, `Enemy.lua`, `MeleeAttacker.lua` and `Projectile.lua`** into tick
   simulation (`OnTick`, server-side, predicted for the local player's movement) and frame
   presentation (`OnUpdate`). Of everything in this milestone, this is the step most likely to take
   longer than its estimate.
5. **The shared `PG` table assumes one player.** It is the only channel between script instances, and
   enemies chase "the player". It becomes per-player where it must be: enemies choose a target among
   the players.
6. **Co-op rules: waves (decided 2026-10-10).** Today the game never ends: the scene's 7 enemies
   die for good, and a dead player respawns at once with full health. The contract needs exactly
   one outcome per match, and a match that never reports one is failed `server_lost` after the
   backend's one-hour limit. The rules:
   - **Waves.** The 7 placed enemies are wave 1. A server-side spawner script spawns waves 2 and 3
     with the existing `Enemy` prefab at marker positions, scaled by player count. Clearing the
     last wave is **victory**.
   - **Respawn delay.** A dead player respawns after 10 s, if any teammate is still up. Everyone
     down at once is **defeat**. `Player.lua`'s down state is the starting point.
   - **A time limit**, 15 minutes, ends the match as **defeat**. A match must always end.
   - **Everyone leaves**: **defeat**. An abandoned match counts as a loss, which is the usual rule.
     Reporting nothing would read as a crash to the backend.

   The server reports through N1's result call. The numbers (wave sizes, the 10 s, the 15 minutes)
   are tuning and may change during N5; the rules are the decision. Waves were chosen over "clear
   the map" because a four-player match on 7 enemies lasts about a minute, and because waves
   exercise what N3 and N5 exist to test: spawns replicated mid-match, and enemies choosing among
   several players. A shared pool of lives was the other variant considered.
7. **The HUD is per local player.** The leaderboard submission (kills in one life) stays on the
   client, which trusts itself. A server-side submission needs a server credential the backend does
   not issue, and is recorded as a follow-up, not built.

#### Verification

| Check | Pass when |
|---|---|
| The whole path | four runtime clients queue through the backend, are matched and allocated a `GanymedDedicated`, join, play to a victory, and see `match.finished` with a rating change; the ticket shows the result |
| Defeat | the same, losing on purpose: a team wipe, and separately the time limit |
| Abandoned | every client quits mid-match: the server reports `defeat` and shuts down; the players' tickets show the result |
| A player leaves | the others continue; the server treats the leaver as down |
| Under a bad network | the whole path at 150 ms and 5% loss is playable; corrections and bandwidth recorded |
| Offline | the game still plays single-player in the editor and in the runtime |
| Linux | the server runs in WSL with the fleet; a Windows client joins it |

---

## Risks

- **Headless is wiring.** N0 showed `Noop` accepts a null window and returns
  valid resource handles, on Windows and Linux. N1's cost is `Application` and `BgfxContext`,
  which still require a `GLFWwindow`, plus pacing the loop without vsync. The no-GPU branch
  through every asset apply is not the plan.
- **The Lua split is the real work.** The game was written for one player and one frame loop, on
  purpose: the Proving Ground was built to test the engine, not to be networked. The split may show
  engine gaps (a missing `GetParent`, no animation time, no animation events) that the game worked
  around with tables. Each gap becomes a master change, then a merge, under the branch policy.
- **Kinematic remote characters may block the predicted one badly.** Colliding with where others
  were 100 ms ago is the standard compromise, but tight doorways with an enemy in them could produce
  constant corrections. If N4 shows it, the fix is to make remote characters non-blocking for the
  local character's queries (an object-layer filter), as many shooters do.
- **Two clocks to debug.** Bugs in tick estimation look like everything else (jitter, rubber-banding,
  dry buffers). The stats of N2 to N4 exist so that a symptom has a number before it gets a fix.
- **The branch policy doubles the merges.** Engine fixes found while playing in N5 go to master
  first. That is the policy, and it is slower on purpose.

## Design tensions, recorded

- **The session after `WELCOME` is unauthenticated.** An on-path attacker who sees a client's packets
  can forge its inputs. netcode.io's answer is per-client keys inside an encrypted token. Adopting it
  would change the backend's token contract (`connect-token.md`) to carry keys, and add packet
  encryption on both ends. That is reasonable for a shipped game and out of scope for this one. The
  random session ID only stops blind, off-path spoofing.
- **Client-side leaderboard scores.** In co-op, a modified client can submit any kill count, as it can
  today offline. Server-side submission needs a server credential and a backend route.
- **Prediction only for movement.** Firing and melee feel one RTT late on the server's side of
  things (damage, deaths). The cosmetic shot hides it for firing; melee hits are shown when the
  server confirms them. Predicting them is rollback of gameplay state, which this plan excluded.

## Docs this milestone must create or update

| Phase | Doc |
|---|---|
| N1 | **new** `docs/dedicated/dedicated.md`; [online.md](../engine/online.md) (lifecycle client, token verification); [architecture.md](../engine/architecture.md) (headless, roles); [ecs.md](../engine/ecs.md) (role-gated systems); [build-and-tooling.md](../engine/build-and-tooling.md) (the new project, the Ed25519 dependency); [ONLINE.md](ONLINE.md) (O5b done) |
| N2 | **new** `docs/engine/netcode.md`, indexed from [docs/README.md](../README.md): sockets, connections, the packet format, the conditioner; [runtime.md](../runtime/runtime.md) (`--connect=`, `--net-sim=`); online.md (`JoinMatch` hands over its socket) |
| N3 | netcode.md (ticks, snapshots, delta, interpolation); [scene.md](../engine/scene.md) (`ReplicatedComponent`, derived child UUIDs) |
| N4 | netcode.md (commands, prediction, reconciliation); [scripting.md](../engine/scripting.md) (`OnTick`, `PlayerCommand`, roles); [physics.md](../engine/physics.md) (replaying a character) |
| N5 | scripting.md (replicated fields, `Net.Broadcast`, `OnNetEvent`); the Proving Ground's own record on `first-game` |

## Dependencies that need approval

| What | Why | When |
|---|---|---|
| An Ed25519 implementation: **Monocypher 4.0.3 with its `monocypher-ed25519` files.** Approved 2026-10-10; N0's cross-language test passed the same day | connect-token verification, fixed by the backend's contract | vendored in N1 |

Nothing else is planned as a dependency. The transport, the serializer and the socket are written
here (decision 3).
