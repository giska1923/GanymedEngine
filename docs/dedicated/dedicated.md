# GanymedDedicated

The process the fleet agent spawns for one match. It boots without a window, waits until the agent
allocates it, loads one scene as a server, admits players who present a connect token, and reports
a victory or a defeat when a timer expires. Admitted players are counted, and after `WELCOME` they
have a session ([netcode.md](../engine/netcode.md)). Nothing is replicated yet: that is N3.

Production engines usually ship this as a second build of the game. Unreal's dedicated server is a
target: `WITH_SERVER_CODE`, the client rendered out, often a different executable. Unity's headless
player is the same player with `-batchmode -nographics`. Godot's dedicated export drops the display
server. All three still compile a lot of "is this a server?" into the binary. Ganymed does not.
`GanymedDedicated` is a third application, as thin as [GanymedRuntime](../runtime/runtime.md), and
the engine binary is the one the editor and the runtime already link. Headless is a field on
`ApplicationSpecification`. Which systems exist is a `SceneRole` checked when the scene registers
them. There is no server `#ifdef`.

| | |
|---|---|
| Sources | `GanymedDedicated/source/` — `DedicatedApp.cpp`, `DedicatedConfig`, `DedicatedLayer` |
| Content | `assets/dedicated.yaml`, then the scene it names (default `scenes/BoxesPhysicsExample.ganymede`) |
| Working directory | whatever `--root` names. The Visual Studio `debugdir` is the repository root, and that is only the debugger |
| Log | stderr, which the agent captures, and `dedicated-<server-id>.log` in that directory, opened append. `--dev` with no id uses `dedicated-dev.log`. `GanymedE.log` is not touched |

## How the agent starts it

The agent does not run the process with the engine tree as its working directory. It starts the
command you gave it, in the agent's own directory, and appends its flags after that command.
`assets/dedicated.yaml`, the asset root and `assets/shaders` are all relative to the working
directory, so a start from anywhere else cannot load a match. `debugdir` in the project file is
what F5 uses. The agent does not read it.

`--root` is applied in `main`, before logging and before `CreateApplication`, via
`std::filesystem::current_path`. One directory change covers every relative path the process
already opens. Resolving those paths from the executable instead would be wrong in this tree: the
exe lives under `bin/<config>-<os>-<arch>/GanymedDedicated/`, which is not a fixed distance from
the assets, and a copied exe is a different layout again.

```
fleetagent -pool 3 -- bin/Debug-windows-x86_64/GanymedDedicated/GanymedDedicated.exe --root D:\Projects\C++\GanymedEngine
```

The agent then appends `--server-id`, `--agent`, `--game-port`, `--advertise` and `--public-key`.
`--root` may be relative to the directory the agent was launched from. A `--root` that is not a
directory exits **2** before the engine starts, and the message is on stderr because the log file
is not open yet.

## Flags

The process exits **2** if a required flag is missing, if `--outcome` is not `victory` or
`defeat`, if a number does not parse, or if `--public-key` is not 32 bytes of base64. That check
is in `ConfigFromCommandLine`, before `Application` is constructed. A failure after start logs
and returns 0 from `main`. The agent treats any exit as a dead process and does not read the code.

| Flag | Meaning |
|---|---|
| `--root` | Working directory for this process. Optional under the debugger, required in the agent command above |
| `--server-id` | Path segment in every agent URL, and the log file's name |
| `--agent` | The agent's origin, no path (`http://127.0.0.1:…`). A trailing slash is stripped |
| `--game-port` | UDP port to bind. All interfaces on the fleet path; loopback only with `--dev` |
| `--advertise` | `host:port` the token's `server_addr` must equal. With `--dev`, forced to `127.0.0.1:<port>` |
| `--public-key` | Ed25519 public key, 32 bytes. The standard alphabet, padding and a trailing newline are accepted here only. Not required with `--dev` |
| `--dev` | No agent, loopback only, `HELLO dev:<name>`. See [development mode](#development-mode) |
| `--net-sim` | Latency, jitter, loss, duplicate, reorder on the send path. See [netcode.md](../engine/netcode.md#the-conditioner). A bad value exits 2 |
| `--net-test` | On this process: `move`, `spawn`, `prefab`, `rest`, or a predict mode (`predict`, `predict-loss`, `predict-desync`, `predict-wall`, `predict-step`). Anything else exits 2. The runtime's flag is a different set |

`--match-seconds`, `--outcome`, `--scene` and `--load-delay` override `assets/dedicated.yaml`.
Both `--name value` and `--name=value` work, and so does the single dash, because the agent's
example command is a Go flag set. Unknown flags are ignored, so a stray `--renderer=` does not
refuse to start: a headless process forces bgfx's Noop backend anyway. `--load-delay` holds the
main thread for that many seconds after the allocation is acknowledged and before the scene loads.
It exists so the health check can be proven; the default is 0.

## Development mode

`--dev` is how a transport test runs without the fleet. The agent flags are not required. The port
defaults to 7777 when `--game-port` is omitted. The socket binds `127.0.0.1` only, and `--advertise`
must be that same `127.0.0.1:<port>` or it is filled in as that. Anything else exits 2, so a dev
process cannot be pointed at a public address by accident.

There is no `OnlineTransport::Init`, no health thread, no `ready`, no `allocated`, and no result.
The match clock is not armed: `dedicated.yaml`'s ten seconds is shorter than a loss measurement,
and the process stays up until it is killed. `GiveUp` and `Finish` only tear the socket down.
`Finish` logs `Dev server stopped`. The scene still loads as `SceneRole::Server`, and the door
thread is the same one as the fleet path.

Admission is `HELLO dev:<name>`. The name is 1–32 characters from `A–Z`, `a–z`, `0–9`, `-` and `_`.
Anything else is `DENIED expected HELLO dev:<name>`. A good hello is `WELCOME <name>`, and the
session in [netcode.md](../engine/netcode.md) starts from there. `GanymedRuntime --connect=` is the
other half.

`--net-test=move|spawn|prefab|rest` loads the scene and then drives a replication check. `move`
instantiates `assets/prefabs/NetMover.gprefab` and writes a scripted pose before each step.
`rest` creates one entity and leaves it. `spawn` creates 500 entities and destroys them after
18 s of server ticks. `prefab` instantiates `assets/prefabs/NetPair.gprefab`, a root and one
child, and logs both UUIDs. The lines are `Replicate ...` in the dedicated log. They are how N3
was measured. A predict mode spawns `assets/prefabs/NetWalker.gprefab` for each account and
applies that client's commands; the lines are `Predict server ...`. A bad value exits 2 before
boot, the same as a bad `--net-sim`.

## Boot

`CreateApplication` loads the yaml, then the flags, then builds a spec with `Headless` and with
`PlayerBackend = false`.

`Headless` skips the window, ImGui, `UIEngine` and `AudioEngine`. `Application` owns a
`BgfxContext` constructed with a null `GLFWwindow` and `headless = true`. That context initialises bgfx as
`RendererType::Noop` with every native handle null, at 1280×720, vsync off. Noop is the one
backend that accepts a null window at a non-zero resolution (measured in N0). A failed init does
not retry on a real backend: a real backend needs the window this process does not have.

`PlayerBackend = false` skips `Online::Init`. A dedicated process must not sign in to the game
backend or open the push socket. Its only HTTP peer is the agent, and that client is
`ServerLifecycle`, which calls `OnlineTransport::Init` itself. `--dev` does not: there is no agent.

The run loop is not `glfwGetTime` and not a swap. It takes a timestep from `steady_clock` and
sleeps out the rest of 1/60 s after `BgfxContext::Frame()`. 60 Hz is the physics step. The sleep
is what keeps an idle server off a core; vsync is what does that job when a window exists.

`DedicatedLayer::OnAttach` on the fleet path then:

1. Binds the UDP port and starts two threads: health, and the door that reads that port. The port
   is bound before the first `ready`. A server that cannot accept a datagram must not be offered.
2. Long-polls `POST /v1/servers/{id}/ready` until the body is an allocation (`match_id`, `players`).
   `204` asks again. A transport error waits a second and retries. Anything else stops the process
   without a `shutdown`, matching the stub: the agent already knows the process is not ready.
3. `POST …/allocated` with `{"match_id":…}` before any scene work. A failed acknowledgement posts
   `shutdown` and exits. The backend withdraws an allocation that is not acknowledged in 5 s.
4. Opens admission, then the optional load delay, then `AssetManager::Init` (read-only) and
   deserializes the scene with `SceneRole::Server`.
5. `OnRuntimeStart`. The match clock starts here.

The frame loop steps the scene. On the fleet path, when the clock expires it posts the result,
posts `shutdown`, and closes. A refused health post is not a finished match: the process calls `GiveUp`, which
posts `shutdown` once and does not report the configured outcome, and does not spend the result
endpoint's five retries on an agent that has already stopped answering. The same split is in
`OnDetach`, so a layer torn down after a refusal does not report one either. A scene that fails
to load posts `shutdown` and no result: reporting a victory for a match that never started would
be a lie.

`Finish` runs from the frame, while the health thread is still alive, and then joins both threads.
The layer's `OnDetach` stops the scene and shuts the asset manager down while bgfx is still up.
`Application`'s destructor body has already run by then; the headless `BgfxContext` is destroyed
after the layer stack, which is what keeps that shutdown legal.

## Health, and why it is not a frame timer

The agent kills a process that misses three health posts, 6 s. A scene load blocks the main
thread, and the async HTTP pool delivers onto that thread, so a health post issued from
`Application::Run` would go silent for the whole load. Health is its own thread. It posts
immediately, then every 2 s, through `OnlineTransport::PostBlocking`: a synchronous `ix::HttpClient`
constructed for that call, so it does not share the long-poll's client and does not touch the
main-thread completion queue. A non-2xx or a transport error stops the process.

The door thread is the other one. It calls `Net::PumpServer(2)`: `select` for up to 2 ms, then
whatever datagrams are waiting. That wake is how the thread notices a shutdown. Teardown sets
stop, joins the health thread, joins the door, and only then closes the socket. Closing under a
blocked `select` would race the descriptor, and on Linux that number can be reused before the
call returns. On Windows the server socket disables `SIO_UDP_CONNRESET` and treats
`WSAECONNRESET` as another empty read, so one dead client does not close the port. The socket,
the session and the conditioner are [netcode.md](../engine/netcode.md).

## Admission

One datagram, `HELLO <token>`, and one reply. The rules run in the order
[connect-token.md](https://github.com/giska1923/GanymedServer/blob/api-v0.6/docs/api/connect-token.md)
writes them. Before the allocation is acknowledged every well-formed hello is
`DENIED not allocated`, and the token is not inspected. After the match the reply is
`DENIED match over`. In between:

1. Exactly one dot, both parts raw base64url (no `+`, `/` or `=`), a 64-byte signature, or
   `DENIED malformed connect token`. The public-key flag is looser than this on purpose; a token
   is not.
2. `crypto_ed25519_check` over the ASCII bytes of the encoded payload. Zero is valid. The call is
   Monocypher's SHA-512 Ed25519, not `crypto_eddsa_check` (BLAKE2b). A failure is
   `DENIED bad connect token signature`. The claims are not parsed until this passes.
3. Version 1, the 5 s leeway on `iat` and `exp`, `server_addr` equal to `--advertise`, `match_id`
   equal to the allocation. The reason strings are the backend's.
4. `account_id` is one of the allocation's players, or `DENIED not a player in this match`.
5. The nonce has not been seen. A replay is `DENIED token already used`. The map stores nonces
   only, and drops them once `exp + 5 s` has passed. The same account with a new nonce is admitted
   again; the admitted set is accounts, so a re-admit does not increment the count the result log
   prints.

The token and the public key are never logged. The line that is logged is the peer and the reply.
A `WELCOME` also records the address on the session (`Net::NoteAdmitted`), so the client's next
datagram is a session request rather than a stranger. A re-admit points the existing session at
the new address and keeps the id.

## The result

`POST …/result` with `{"outcome":"victory"}` or `"defeat"`, from `--outcome`. Five attempts, 500 ms
backoff doubling, on a transport error or a 5xx. A 4xx stops. Then `POST …/shutdown` and the
process exits. The server never sees a backend URL or a result credential: the agent forwards the
outcome. That is `api-v0.6`. Under `api-v0.5` the server posted to one replica itself, and a
replica that stayed down turned a finished match into `server_lost`.
