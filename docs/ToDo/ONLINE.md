# Milestone — Online client (the engine side of the backend)

**Status: O0–O2 done; O3's bindings done, its Proving Ground half (on `first-game`) open (2026-10-09).** The backend is complete (B1–B5, `api-v0.5`), so
O1–O4 and O5a have everything they need from it. O5b waits on a dedicated-server milestone that has not
been planned. See [Shape of the milestone](#shape-of-the-milestone-and-its-honest-size).

The engine half of a game backend: an HTTP and WebSocket client that never blocks the frame, a
device identity and session, typed Lua bindings for leaderboards and push notifications, and,
later, the hooks a dedicated server needs to be allocated by a fleet and to verify the players
it admits.

The backend itself is **not in this repository**. It is a Go service in its own repo,
[GanymedServer](https://github.com/giska1923/GanymedServer), with its own design document
([`docs/history/BACKEND.md`](https://github.com/giska1923/GanymedServer/blob/main/docs/history/BACKEND.md)
there, complete) and its own phases (B1–B5 below). This file covers only what lands
in `GanymedEngine/`, `GanymedRuntime/` and, for O3's game use, `first-game`.

**Naming, because the two are easy to confuse:** *GanymedServer* is the backend (accounts,
leaderboards, matchmaking, allocation). *`GanymedDedicated`* is the planned engine app that runs
one match headless: the game server the backend allocates and the one O5b is about.

---

## Why this, and why now

Both the engine and the backend are learning projects, and nothing is planned to ship. That
changes the priorities. Crash reporting, platform identity (Steam/EOS), patching, anti-cheat and
data-protection work all exist for real users, and there are none. What stays is the part that
teaches something:

- **how a frame loop consumes network I/O without ever waiting on it**, which is the same problem
  the asset loader solved for disk I/O, with worse latency and failure modes;
- **how a request that outlives its requester is made safe**, the problem `Future`'s
  cancel-and-wait destructor exists to solve, now with a remote party that cannot be cancelled;
- **how identity, sessions and a signed contract connect a client, a backend and a game server**
  that do not trust each other.

The Proving Ground is single-player, and that is enough to start. A leaderboard is a real round
trip from a Lua script to a Postgres row and back to the HUD, and it needs no netcode. Everything
up to O4 can be exercised by the game that already exists.

## What it is

- `GanymedEngine/source/GanymedE/Online/`: a new engine module, static facade, `Init`/`Shutdown`
  owned by `Application`, the same shape as `AudioEngine` and `ScriptEngine`.
- Async HTTP requests whose completions reach the main thread through
  `JobSystem::SubmitToMainThread` and reach Lua through `LuaScriptSystem`, **never inside a
  request's callback thread**.
- A device-ID identity kept in a per-user data directory, a session token held in memory, and a
  `--profile=<name>` flag so two instances on one machine can be two players.
- `Backend.*` Lua bindings. They are **typed per endpoint**, and there is no generic HTTP.
- One WebSocket push channel with reconnect and backoff.
- O5: queueing for a match and joining its game server from the client (O5a), then lifecycle
  reporting, connect-token verification and result reporting in `GanymedDedicated` (O5b).

## What it deliberately is not

- **No generic HTTP from Lua.** The VM is sandboxed. It opens no `io`, `os` or `package`
  ([scripting.md](../engine/scripting.md#sandboxing)), because gameplay scripts have no business
  touching the filesystem. Reaching arbitrary network endpoints is the same class of capability.
  Scripts get `Backend.SubmitScore`, not `Http.Post`.
- **No TLS.** The backend runs on `localhost` in Docker, and IXWebSocket's TLS needs OpenSSL or
  mbedTLS on Windows, which would be a second dependency for no learning payoff. Plain `http://`
  and `ws://`. A remote backend reopens this, and that is recorded under
  [Design tensions](#design-tensions-recorded).
- **No platform identity, no passwords.** The device-ID login only. A GitHub OAuth flow is a
  possible later learning exercise. It is not in this plan.
- **No automatic retries and no offline queue.** A failed request reports failure to its caller.
  The one exception is a single re-authentication on `401` (O2). A persistent retry queue is a
  real system with real design questions, and nothing here needs it.
- **No netcode, no headless mode.** Those belong to the dedicated-server milestone, which is not
  planned yet. O5b depends on it and is written only as far as the contract.
- **No backend code in this repository.** No Go and no Dockerfiles. The contract lives in the
  backend repo; see below.

---

## The contract, and who owns it

Three things cross the boundary between the repositories:

| Contract | Owner | Consumed here by |
|---|---|---|
| Client API (HTTP routes, JSON shapes, error codes) | backend repo, as an OpenAPI spec | O1–O3 |
| Push message schema (WebSocket frames) | backend repo | O4 |
| Connect token + server lifecycle protocol | backend repo | O5a presents the token, O5b verifies it and speaks the lifecycle |

**The engine never copies the spec.** `docs/engine/online.md` links to the backend repo's spec at
a named version and documents only the engine's side: which calls exist, what thread they
complete on, and what happens when they fail. A copied schema drifts silently. A link that goes
stale at least fails visibly.

Versions published so far, as git tags on the backend repo:

| Tag | Adds | Engine phase that reads it |
|---|---|---|
| [`api-v0.1`](https://github.com/giska1923/GanymedServer/blob/api-v0.1/docs/api/openapi.yaml) | device login, refresh rotation, `/v1/me`, the problem types | O2 |
| [`api-v0.2`](https://github.com/giska1923/GanymedServer/blob/api-v0.2/docs/api/openapi.yaml) | profiles, leaderboards, `Idempotency-Key`, integer score rules | O3 |
| [`api-v0.3`](https://github.com/giska1923/GanymedServer/blob/api-v0.3/docs/api/realtime.md) | the push socket ([`realtime.md`](https://github.com/giska1923/GanymedServer/blob/api-v0.3/docs/api/realtime.md)): messages, presence, close codes; party routes in [`openapi.yaml`](https://github.com/giska1923/GanymedServer/blob/api-v0.3/docs/api/openapi.yaml) | O4 |
| [`api-v0.4`](https://github.com/giska1923/GanymedServer/blob/api-v0.4/docs/api/openapi.yaml) | matchmaking tickets, the profile's `rating`, the `ticket.updated`, `match.found` and `ticket.failed` pushes | superseded by `api-v0.5` before any engine phase used it |
| [`api-v0.5`](https://github.com/giska1923/GanymedServer/blob/api-v0.5/docs/api/openapi.yaml) | the states after `matched`, the ticket's `server` and `result`, `match.ready` and `match.finished` ([`realtime.md`](https://github.com/giska1923/GanymedServer/blob/api-v0.5/docs/api/realtime.md)), [`connect-token.md`](https://github.com/giska1923/GanymedServer/blob/api-v0.5/docs/api/connect-token.md), [`server-lifecycle.md`](https://github.com/giska1923/GanymedServer/blob/api-v0.5/docs/api/server-lifecycle.md). **Changes a meaning:** `matched` was final in `api-v0.4` and is not any more | O5a, O5b |

**The session token is opaque to the engine.** The backend issues a JWT; the client stores it and
sends it as `Authorization: Bearer …` and never decodes it. That keeps a JWT library out of the
engine, and it is also correct: a client that reads its own token's claims starts trusting them.

## Where things run

| Piece | Where | Why |
|---|---|---|
| Backend (Go), Postgres, Redis | Docker Compose on the dev machine | Pinned database versions, one command up and down, and the same file moves to a VPS unchanged |
| Fleet agent (backend B5) | **Native**, on the host | It spawns `GanymedDedicated`, which is a Windows executable |
| `stubserver`, the Go stand-in game server (backend B5) | **Native**, spawned by the agent | Exactly where `GanymedDedicated` will run, so swapping one for the other changes only the agent's command line |
| `GanymedDedicated` (O5b) | **Native**, spawned by the agent | Docker Desktop runs Linux containers. Containerising it needs a Linux headless build first |
| `GanymedRuntime` / `GanymedEditor` | Native | They are the clients |

The engine reaches the backend at `--backend=<url>`, default `http://127.0.0.1:8080`, which is the
port Compose publishes. Containerising `GanymedDedicated` later, one container per match (the
Agones model), is a reasonable follow-up once a Linux headless build exists. It is not a
prerequisite for anything here.

---

## Shape of the milestone, and its honest size

| Phase | What | Branch | Needs from the backend | Rough size |
|---|---|---|---|---|
| **O0** | Vendor IXWebSocket, a premake project, and a build on Windows and Linux. **Done** | master | nothing | took under a day |
| **O1** | `Online/` request layer: threading, ownership, cancellation, timeouts. **Done** | master | nothing (a Python stub) | about a day |
| **O2** | Identity: user-data dir, device ID, `--profile=`, session, `401` re-auth. **Done** | master | **B1** (device auth) | under a day |
| **O3** | `Backend.*` leaderboard bindings; the Proving Ground submits and shows scores | master + `first-game` | **B2** (leaderboards) | 1–2 days |
| **O4** | The WebSocket push channel: reconnect by close code, re-fetch on connect, Lua subscriptions, party bindings | master | **B3** (realtime gateway, `api-v0.3`) | 3–4 days |
| **O5a** | Matches from the client: queue, follow the ticket, join the server over UDP | master | **B4 + B5** (`api-v0.5`); O4 for the pushes | 2–3 days |
| **O5b** | `GanymedDedicated` hooks: lifecycle, connect-token verification, result | master | **B5** + the dedicated-server milestone | blocked, unsized |

These are estimates, not measurements. O1 is the phase that matters. Everything after it is a
consumer of O1's threading and ownership rules, and getting those wrong shows up as a crash on
scene stop a week later.

---

## Phase O0 — vendor the transport — **DONE**

### Goal

IXWebSocket builds as its own static library under premake, links into the engine, and a probe
can issue one `GET` against a local server on Windows and on Linux (WSL2).

### Steps

1. **Sign-off first.** This is a new third-party dependency. AGENTS.md requires asking, and this
   plan is the ask. Alternatives are under Decisions.
2. Add the submodule at `GanymedEngine/extern/IXWebSocket`, pinned to a release tag rather than
   `master`.
3. Write `extern/IXWebSocket.lua` beside the others (`extern/enkiTS.lua` is the closest model):
   a `StaticLib`, output under `%{wks.location}/bin` and `temp` (never inside the submodule, per
   [build-and-tooling.md](../engine/build-and-tooling.md#dependencies-vendored-under-ganymedengineextern)),
   sources `ixwebsocket/*.cpp` **minus** the TLS backends (`IXSocketOpenSSL`, `IXSocketMbedTLS`,
   `IXSocketAppleSSL`) and minus anything that pulls in zlib. `IncludeDir.IXWebSocket` goes on
   the **engine project only**.
4. Find the defines CMake would have set. The two RmlUi defines in build-and-tooling.md are the
   warning: a define CMake supplies silently can fail at *runtime* when a hand-written script
   forgets it. Read `CMakeLists.txt` for every `add_definitions` / `target_compile_definitions`
   and record which ones the premake script supplies and why.
5. Links: `ws2_32` (and whatever else the CMake file names) on Windows; `pthread` on Linux, which
   is already in every app's link list.
6. Regenerate project files. This adds a project, so `python scripts/setup.py generate` is
   required.

### Decisions, with reasoning

**IXWebSocket, over the alternatives.** One library covers both HTTP (`ix::HttpClient`, with an
async mode) and WebSocket (`ix::WebSocket`). It is C++11-compatible, needs no Boost, and runs
its I/O on its own background threads, which is exactly the property O1 needs. Considered:

| Option | Why not |
|---|---|
| **libcurl** | The industry default, and on Windows it can use Schannel for TLS with no OpenSSL. But its CMake build has dozens of options to port to premake, and its WebSocket API is younger than its HTTP one. The right choice if TLS ever becomes a requirement. |
| **cpp-httplib** + **HTTP long-polling** instead of WebSocket | Header-only and actively maintained. Long-polling is a legitimate push technique. But WebSocket is what game backends use (Nakama's realtime API is WebSocket), and the backend half of this project exists to learn that model. The runner-up. |
| **cpp-httplib** + a separate WebSocket library | Two dependencies for what one covers. |
| Raw sockets, own HTTP/1.1 | HTTP parsing and chunked transfer are not the lesson. |

**Known cost, stated plainly.** IXWebSocket's README carries a note from its author saying they
no longer have much time for it. For a learning project pinned to a tag, that is acceptable: the
code is small enough to read, and nothing here depends on future releases. For a shipped product
it would not be.

**Build without TLS and without zlib.** See [What it deliberately is not](#what-it-deliberately-is-not).
Leaving the TLS sources out of the premake project, rather than compiling them and leaving them
unused, keeps OpenSSL headers from ever being required on a clean clone.

### Risks

- **`winsock2.h` after `windows.h`.** Including `windows.h` first pulls in the old `winsock.h`, and
  `winsock2.h` then redefines half of it. If the engine's PCH includes `windows.h`, an IXWebSocket
  header included from any engine TU breaks. Mitigation: IXWebSocket is included from **exactly
  one engine `.cpp`** (O1). That is the same firewall miniaudio gets
  ([architecture.md](../engine/architecture.md), principle 6). IXWebSocket's own TUs compile in
  its own project, with no `gepch.h`.
- **Windows needs `ix::initNetSystem()`** (WSAStartup) before any socket and `uninitNetSystem()`
  after the last. O1 owns that call.
- The async HTTP API's exact shape is taken from IXWebSocket's `docs/usage.md`, read 2026-09-26.
  **Re-read it against the pinned tag before writing O1.**

### Verification

| Check | Pass when |
|---|---|
| Windows x64 Debug, Release, Dist | engine, editor and runtime all link |
| Linux (WSL2) Debug | same |
| Submodule clean after a build | `git status` in `extern/IXWebSocket` shows nothing |
| Probe: one sync `GET` to `python -m http.server` | status 200 and the body length logged. Probe deleted afterwards |

### Execution notes

2026-10-09, on `hello-online` after merging `master` into it. IXWebSocket pinned to **`v12.0.1`**.
What it does now is in [build-and-tooling.md](../engine/build-and-tooling.md#dependencies-vendored-under-ganymedengineextern).

| Check | Result | Evidence |
|---|---|---|
| Windows x64 Debug, Release, Dist | **pass** | engine, editor and runtime linked in all three; the runtime held the probe, so its link really pulled IXWebSocket and Winsock in. No new warnings (the one `LNK4006`, `psapi` over `gdi32`, predates this) |
| Linux (WSL2, Ubuntu 22.04, gcc 11.4) Debug | **pass for the engine and runtime**; the editor does not compile, for reasons unrelated to this phase | `libIXWebSocket.a` built clean, the engine and runtime linked with it after the engine and before `pthread`. The editor fails in `EditorPicking` and `EditorUndo` code from `master` ([cross-cutting.md](cross-cutting.md#the-editor-does-not-compile-on-linux-gcc)) |
| Submodule clean after a build | **pass** | `git status` in `extern/IXWebSocket` is empty after Windows and Linux builds |
| Probe | **pass**, against an HTTP/1.1 server (below) | Windows Debug, Release, Dist: `GET` → 200 with the full body, also against the real backend's `/healthz`. Linux: 200, 15 of 15 bytes. Probe (`Online/NetProbe.cpp`, called from the runtime's `CreateApplication` under `GE_NET_PROBE`) deleted, and its stale objects removed from both platforms' archives |

**Where the plan was wrong or incomplete, kept visible:**

- **"Minus anything that pulls in zlib" was unnecessary.** Every zlib and TLS use is behind
  `IXWEBSOCKET_USE_ZLIB` / `IXWEBSOCKET_USE_TLS` inside the sources. The script compiles every
  source except the three TLS backends, which is exactly CMake's list with both options off (the
  lists were diffed, 36 files each). No define was needed beyond `_CRT_SECURE_NO_WARNINGS`.
- **CMake's Windows links were too many.** It links `ws2_32`, `wsock32` and `shlwapi`. `shlwapi`
  is used only by the OpenSSL backend, and nothing includes the Winsock 1 header. Since a
  `StaticLib`'s links are merged into the `.lib` on MSVC, `wsock32` over `ws2_32` produced about
  60 `LNK4006` warnings. Only `ws2_32` is linked.
- **The `winsock2.h`-after-`windows.h` risk did not materialise.** `gepch.h` does include
  `<Windows.h>` without `WIN32_LEAN_AND_MEAN`, but the probe, compiled with the PCH, built clean on
  Windows SDK 10.0.26100. That was observed, not traced, so O1 keeps the one-TU rule regardless,
  for the errno macros that `IXNetSystem.h` redefines for its includer.
- **`python -m http.server` fails the probe.** IXWebSocket's HTTP client parses the status line with
  `sscanf(line, "HTTP/1.1 %d")` and rejects Python's default `HTTP/1.0` reply ("Cannot parse
  response code from status line"). An HTTP/1.1 Python server passed. The backend (Go) answers
  1.1, so this only matters for test servers: see O1's verification.
- **Two fixes landed upstream after `v12.0.1`** and are not in the pin. #609 fixes a hang in
  `stop()` when an automatic reconnect races it (the thread join never returns). #597 replaces
  `ssize_t` with `std::ptrdiff_t` to avoid a typedef clash on MSVC; at the pin, `IXSocket.h`
  still has `typedef SSIZE_T ssize_t` on Windows. Both are O1 risks below. A later bump to a tag
  that contains them is cheap: re-diff the source list and rebuild.

---

## Phase O1 — the request layer — **DONE**

### Goal

`Online::` can issue asynchronous HTTP requests whose results reach Lua on the main thread,
inside the owning scene's script update. A request can never deliver into a destroyed scene or a
destroyed script instance, and nothing in this layer ever blocks a frame.

### How the delivery path works, end to end

```
Lua: Backend.X(self.entity, …, callback)
  └─ ScriptBindings: store callback under (scene, owner UUID, request id); call Online::Send
       └─ ix::HttpClient (async) ── its own thread ── network ──┐
                                                               ▼
                                    completion on IX's thread: copy status + body
                                    └─ JobSystem::SubmitToMainThread(result)
Application::Run ─ JobSystem::OnUpdate (top of next frame)
  └─ Online: owner still alive? ── no ─▶ drop, count as cancelled
                              └─ yes ─▶ ScriptEngine mailbox for that scene
Scene update ─ LuaScriptSystem::OnUpdate, scene context set
  └─ drain mailbox before instances' OnUpdate: protected_function(ok, result)
```

Three hops. Each one exists for a reason:

1. **IX's thread → main thread.** Lua is single-threaded and the VM belongs to the main thread.
   The IX documentation says async callbacks run "in a background thread". Touching `sol::state`
   there is a data race.
2. **Main-thread drain → the scene's script update.** Every per-entity binding resolves through
   `ScriptEngine`'s current scene context (`CurrentScene()` in `ScriptEngine.cpp`), which is set
   only while that scene's scripts run. A callback fired from `JobSystem::OnUpdate` would call
   `self.entity:SetTranslation` with **no scene context**, or with the wrong one when the editor
   has a preview scene. Collision callbacks already arrive the same way, inside the scene update.
3. **The ownership check sits in the middle.** It is the last point before the result becomes a
   Lua call, and it is on the main thread, where destruction happens.

### Steps

1. `Online/Online.h`: static facade. It has `Init(const OnlineConfig&)`, `Shutdown()`,
   `Send(Request) → RequestId` and `Cancel(RequestId)`. No IXWebSocket or sol2 types in the header.
2. `Online/Online.cpp`: the single TU that includes IXWebSocket (see O0's risk). It owns one
   `ix::HttpClient` in async mode, `initNetSystem`/`uninitNetSystem`, and the pending-request map.
3. `Application` calls `Online::Init` after `ScriptEngine::Init` and `Online::Shutdown` **before**
   `ScriptEngine::Shutdown`, because pending callbacks are sol2 objects. The config comes from
   the command line: `--backend=<url>`, read by `Online` itself, the same way `BgfxContext` reads
   `--renderer=`.
4. Ownership: every request carries `(Scene*, owner UUID)`. `ScriptEngine::DestroyInstance` and
   `DestroySceneInstances` cancel every request their instance or scene owns. **Cancel means
   "drop the result on arrival".** Whether IXWebSocket can abort an in-flight transfer is not
   known; see Risks. This is the same cooperative meaning `JobState::Cancelled` has.
5. The mailbox: a per-scene queue inside `ScriptEngine`, drained at the top of
   `LuaScriptSystem::OnUpdate`. A callback that errors disables its instance exactly as a failing
   `OnUpdate` does ([scripting.md](../engine/scripting.md#errors)).
6. Timeouts on every request: `connectTimeout` and `transferTimeout` from `HttpRequestArgs`.
   Values are chosen in O1 from measurement, not guessed here.
7. JSON: parse with **yaml-cpp**, emit with a small hand-written writer. See Decisions. **The
   writer must emit whole numbers as integers** (`1234`, never `1234.0` or `1.234e3`). Every Lua
   number reaching it is a double, and the backend's integer fields (scores, from `api-v0.2`)
   decode into `int64`, which refuses a fractional literal with a 400. So an integral double is
   written with no fraction. A value above 2^53−1 cannot be represented exactly as a double, and
   the backend refuses it anyway, so the writer refuses it as well rather than sending a rounded
   number.
8. Instrument: `GE_PROFILE_SCOPE` on the drain, plus counters for sent, completed, failed,
   cancelled and dropped-late.

### Decisions, with reasoning

**The library's threads, not `JobSystem` workers.** The earlier discussion said "async over
`JobSystem`". That was wrong, and this is the correction. A blocking HTTP call on an enkiTS
worker holds one of `hardware_concurrency() - 1` threads for the whole round trip, which starves
`ParallelFor` and Jolt, since Jolt now runs on the same pool
([physics.md](../engine/physics.md#the-job-system)). Network I/O is latency-bound, not
compute-bound. It belongs on a thread that sleeps in `select`/`poll`, and IXWebSocket already
has one. `JobSystem` keeps the only job it is right for here: `SubmitToMainThread`, the handoff.

**Callbacks, owned by an entity, rather than polling.** A polling API
(`local id = Backend.X(); … Backend.Poll(id)`) has no lifetime problem, because nothing is ever
called back. But every script would then carry request-id bookkeeping in `OnUpdate`, and that
would be the most common code in this area. Callbacks with explicit ownership move that
bookkeeping into the engine, once. **The owner is passed explicitly** (`self.entity`) rather than
inferred from "whichever instance is executing", in line with explicit over clever. It also means
the binding can refuse an ownerless call with a named error, instead of silently binding the
callback to something.

**Where production engines diverge.** Unreal's `FHttpModule` delivers completion delegates on the
game thread from its own tick. Unity's `UnityWebRequest` is polled or awaited from coroutines.
Both leave lifetime to the caller, and a stale delegate or a coroutine on a destroyed object is a
classic bug in both. Ganymed's owner-scoped cancellation is stricter than either. It can afford
to be, because all callers are scripts with a known owner.

**JSON via yaml-cpp plus a writer, not a new dependency.** In practice, JSON as a Go service
emits it is valid YAML 1.2 flow syntax, and yaml-cpp is already vendored. The writer is roughly
50 lines: objects, arrays, strings with escapes, numbers and bools. The alternative is
nlohmann/json, which is header-only and ubiquitous but a second new dependency with a notable
compile-time cost. **If yaml-cpp mis-parses a real backend payload, switch rather than patch.**
The risk below is the trigger.

### Risks

- **Shutdown can block.** An async `ix::HttpClient` owns a thread, and destroying it presumably
  joins that thread. An in-flight request could then hold `Application` exit for up to
  `transferTimeout`. Not known yet. **O1 measures it**: quit with a request parked on a
  10-second delay endpoint, and time the exit.
- **Head-of-line blocking.** I am not sure whether IXWebSocket's async client runs requests
  concurrently or queues them on one thread. If it queues them, one slow request delays every
  request behind it. Measure with two requests to a delay endpoint and a fast one. If it matters,
  the fix is a small client pool, not a different library.
- **Connection-refused on Windows is slow. Measured in O0:** the probe's whole process ran in
  132 ms against an open port, about **2.1 s against a closed port on `127.0.0.1`**, and about
  **4.1 s on `localhost`**, which resolves to `::1` and `127.0.0.1` and pays for both. On Linux
  the same run took 0.17–0.21 s: refused at once. It is async, so it costs no frame time, but it is
  how long "backend is down" takes to arrive on Windows. Keep the `--backend=` default on
  `127.0.0.1`, never `localhost`. Windows also reports that refusal as `Connect error: No error`
  (an errno mapping upstream fixed after `v12.0.1`), so a "readable reason" will need the engine's
  own wording.
- **`stop()` can hang** with IXWebSocket's automatic reconnection on (fixed upstream after the pin,
  #609). O4 reconnects by close code itself, so it must call `disableAutomaticReconnection()`,
  which avoids the race as well.
- **`ssize_t` on MSVC.** `IXSocket.h` typedefs it on Windows, which clashes with any other
  library's `ssize_t` typedef in the same TU (replaced upstream after the pin, #597). It can only surface in `Online.cpp`, the one
  TU that includes IXWebSocket, and it will surface at compile time if it does.
- **yaml-cpp and JSON edge cases.** `\u` escapes, very large integers, `null`. The probe payloads
  below include each.

### Verification

Against a throwaway Python stub, not committed, with endpoints `/ok`, `/delay/<s>`, `/status/<code>`
and `/edge` (a payload with `é`, `null`, a 2^53+1 integer and a nested array). **The stub must
answer HTTP/1.1** (`protocol_version = "HTTP/1.1"` on its handler): IXWebSocket's client rejects
the `HTTP/1.0` that `http.server` sends by default (found in O0).

| Check | Pass when |
|---|---|
| Callback timing | a request completes and its callback runs inside `LuaScriptSystem`, one or more frames later, with the scene context set |
| **Stop play mid-request** (editor, `/delay/3`) | no callback, no crash, a `dropped-late` count of 1 |
| Entity destroyed mid-request | same, for one owner, while the other instances' callbacks still fire |
| Backend down | the game plays normally; the callback gets `ok == false` with a readable reason; the time to failure is recorded |
| **Frame cost** | frame time with 20 requests in flight against `/delay/2` is indistinguishable from none, from the Instrumentor trace |
| Exit with a request in flight | exit time measured and recorded; if it exceeds the timeout, a Risk becomes a ToDo entry |
| `/edge` payload | every field round-trips through the parser and the writer |
| Whole-number doubles | the writer emits `1234` for the Lua value `1234` (a double), and refuses 2^53 |
| Ownerless call | refused with a named error, and nothing is sent |

### Execution notes

2026-10-09, on `hello-online`. Live behaviour is in [online.md](../engine/online.md). Verified on
the Windows x64 Debug **runtime**, booted on throwaway test scenes with
`GE_ONLINE_TEST=1 GanymedRuntime scenes/O1Test.ganymede --backend=http://127.0.0.1:8781`, against
an HTTP/1.1 Python stub. The scenes, the script and the stub are not committed.

| Check | Result | Evidence |
|---|---|---|
| Callback timing | **pass** | a request from `OnCreate` was delivered in the scene's first update, inside `LuaScriptSystem`; `self.entity:GetName()` worked in the callback |
| **Stop play mid-request** (editor) | **pass**, on the second attempt (below) | Stop at 14.5 s into a `/delay/30`: `OnDestroy` saw `inFlight=1`, the heartbeat stopped, no callback, closing stats `1 cancelled, 1 dropped late, 0 abandoned`. The stub still slept 30 s and answered into an open connection, closed only at editor exit |
| Entity destroyed mid-request | **pass** | victim sent `/delay/3`, destroyed 0.5 s later: `cancelled=1 droppedLate=1`, nothing in flight, its callback never ran; the driver's requests kept completing |
| Backend down | **pass** | `ok=false`, "cannot connect to http://127.0.0.1:8799/ok", about 2 s in, game still running |
| Frame cost | **pass**, from script-side frame times rather than an Instrumentor trace | 287 frames each: idle mean 6.98 ms (max 10.01), 20 in flight mean 6.96 ms (max 12.53) |
| Exit with a request in flight | **pass** | 10 s request in flight: exit 0.62–0.75 s; nothing in flight: 0.64–0.71 s |
| `/edge` payload | **pass** | both `\u00e9` and raw `é`, `null` → nil, 2^53+1 exact as a Lua integer, nested array, quoted literals stayed strings; written back, the stub found it equal to the original (nulls aside) |
| Whole-number doubles | **pass** | `1234.0` → `1234`, `3 * 1.0` → `3`, int64 `9007199254740993` exact; `2.0^53` refused, sent count unchanged |
| Ownerless call | **pass** | nil owner, an entity with no script, a non-function callback: each a named Lua error, sent count unchanged |
| Head-of-line (added) | **pass** | `/ok` behind two `/delay/2`: delivered the next frame |
| Failure reasons (added) | **pass** | 404 problem+json → `urn:ganymed:problem:not-found`; plain 503 → `HTTP 503` |
| Linux (WSL2, gcc 11.4) | **pass** | the engine and runtime build with the O1 code, no warnings outside `extern/`. The same test scene: T1–T6 identical results; the victim's callback never ran and the final stats read `cancelled=1 droppedLate=1`. WSL's software GL runs at ~44 ms a frame, so its frame-cost numbers (43.7 vs 46.8 ms) measure the renderer, not this code, and exit was not timed there |

**Where the plan was wrong or incomplete, kept visible:**

- **"Shutdown can block" was wrong, and the measurement caught it.** I read IXWebSocket's
  `HttpClient::request` as checking only the request's own `cancel` flag during a transfer, and
  wrote an explicit abort into `Online::Shutdown`. Removing that abort (a deliberate mutation)
  still exited in 0.74 s with a 10 s request in flight. The cancellation check is a lambda that
  ORs in the client's `_stop` and captures its timeout object by reference, so it applies to the
  transfer as well. The abort is gone, and the comment says why.
- **Head-of-line blocking was real, and is answered by reading, not guessing.** The async client is
  one thread popping one queue (`HttpClient::run`). `Online` keeps four clients and sends each
  request to the least loaded. That bounds the problem rather than removing it: 20 two-second
  requests take five rounds, and 4 had landed after 2.5 s.
- **Cancel stops the waiting, not the connection.** The plan did not know whether IXWebSocket
  could abort a transfer. `HttpRequestArgs::cancel` is checked between socket waits, so a cancelled
  request frees its network thread within 0.6 s and its late response is counted, never delivered.
  **First written here as "the connection is closed (the stub logged the abort)", which was wrong:**
  that stub line came from the exit test. The editor check showed the stub sleeping its full 30 s
  and answering into a connection that stayed open until exit. It is harmless: IXWebSocket opens a
  new socket per request, so no later request can read the stale response, and at most four idle
  sockets linger ([online.md](../engine/online.md#what-cancel-does-and-does-not-do)).
- **The first stop-play attempt proved nothing, because the check was badly built.** Its request was
  `/delay/10`, equal to the default 10 s transfer timeout, so it timed out by itself while play was
  still running, and nothing logged when Stop was pressed. The second attempt used `/delay/30` with a
  60 s timeout, a heartbeat line per second and a line from `OnDestroy`. A third instruction bug on
  the way: the editor resolves a command-line scene against the working directory, unlike the
  runtime ([cross-cutting.md](cross-cutting.md#the-editor-and-the-runtime-read-a-command-line-scene-path-differently)).
- **"Every Lua number reaching the writer is a double" is false for Lua 5.4**, which has an integer
  subtype. The writer writes Lua integers exactly and applies the whole-number and 2^53 rule only
  to floats. That is also what makes the `/edge` round trip possible: 2^53+1 is a Lua integer and
  is written back exactly, where the plan's rule would have refused its own probe.
- **Owner bookkeeping lives in `ScriptEngine`, not `Online`.** The plan's diagram had `Online`
  checking whether the owner was alive. `Online` knows nothing of scenes; `ScriptEngine` keeps a
  per-scene map of requests and a mailbox, and cancels on `DestroyInstance` and
  `DestroySceneInstances`. `Online` stays a transport, and the dependency points one way.
- **O1 needed a Lua surface the plan did not name.** Its checks call arbitrary stub paths, and the
  permanent bindings are typed per endpoint. A temporary `Backend.__Request` (and `__Stats`) was
  registered only under `GE_ONLINE_TEST`, and was deleted, with the test scenes, script and stub,
  once every check had passed. `Backend` is an empty table until O2.
- **Frame cost came from script-side `ts`, not the Instrumentor.** Turning `GE_PROFILE` on
  recompiles the engine. With frame time unchanged in mean and nothing new on the main thread
  but a map lookup per response, a trace was not worth that build.
- **Two things noticed in passing:** sol2 prints `[sol2] An exception occurred` to stderr for
  errors a script catches with `pcall`, which is noise, not a failure. And a script summing `ts` sees
  the engine's known over-a-second first frame ([cross-cutting.md](cross-cutting.md#the-first-frames-timestep-is-over-a-second)),
  which inflated the "backend down" time to 4.7 s in script terms against about 2 s of play.

---

## Phase O2 — identity — **DONE**

### Goal

Each running instance has a stable player identity, signs in to the backend on start without
blocking boot, keeps its session alive, and degrades to "offline" without affecting play.

### Steps

1. A per-user data directory: `%LOCALAPPDATA%\GanymedEngine\` on Windows, `$XDG_DATA_HOME` or
   `~/.local/share/GanymedEngine/` on Linux. It belongs in `PlatformUtils`. The engine has no such
   concept today, and it needs one: the runtime's asset tree is read-only by design
   ([runtime.md](../runtime/runtime.md)), because a shipped game must not write into its install
   directory.
2. `profiles/<profile>/device_id`: a random UUID, created on first run. `--profile=<name>`
   selects the file, default `default`.
3. On `Online::Init`, send the backend's device-auth call (B1's route) asynchronously. Store the
   returned session and refresh tokens in memory only.
4. On a `401`, refresh once, then retry the original request once. A second `401` is a failure
   returned to the caller. **Refresh tokens rotate** (backend B1): every refresh returns a new
   refresh token and consumes the old one, and presenting a consumed one revokes the whole
   session. So the client must replace its stored refresh token on every refresh, and must never
   run two refreshes at once. Concurrent `401`s from several in-flight requests share one
   refresh rather than each starting their own.
5. An `Online::GetStatus()` of `Offline`, `SigningIn` or `SignedIn`, plus the player's display name.
   Exposed to Lua as `Backend.IsSignedIn()` and `Backend.GetPlayerName()`.

### Decisions, with reasoning

**`--profile=` exists for the multiplayer future, and costs nothing now.** Two clients on one
machine is how every networked game is first tested. Without a profile switch, both would read
the same `device_id`, sign in as the same account, and the backend would see one player
connecting twice. Some backends reject that, and it is an exceedingly confusing first bug to hit
in B3 or O5.

**Tokens in memory only.** Persisting the refresh token would skip a login round trip on start.
It would also be a credential on disk, and the device-ID login is already one localhost round
trip. Not worth it.

**Sign-in never gates boot.** The game must be fully playable with the backend down, because
every Proving Ground gate run happens without it.

### Verification

| Check | Pass when |
|---|---|
| First run | `device_id` created; sign-in logged with the player id the backend assigned |
| Second run | same player id |
| `--profile=b` beside the default | two different player ids; the backend shows two accounts |
| Backend down at boot | boot time unchanged; status `Offline`; one warning, not one per frame |
| Backend restarted mid-session (token invalid) | the next call refreshes and succeeds, visible in the log |

### Execution notes

2026-10-09, on `hello-online`. Live behaviour is in
[online.md](../engine/online.md#identity-and-the-session) and
[platform.md](../engine/platform.md#per-user-data). Verified on the Windows x64 Debug runtime against
the **real backend**: the GanymedServer binary on the host, on 8081, sharing the Compose Postgres and
Redis, with `GS_ACCESS_TOKEN_TTL=2s` so expiry happens inside a run. A throwaway test scene and
script (not committed) polled the status and called `Backend.GetProfile`.

| Check | Result | Evidence |
|---|---|---|
| First run | **pass** | `profiles/o2a/device_id` created under `%LOCALAPPDATA%\GanymedEngine`; "signed in as account 941855b7-… (profile 'o2a')", then "playing as 'Player-941855'" |
| Second run | **pass** | the same account ID |
| `--profile=b` beside the default | **pass** | `o2b` signed in as a different account; the backend logged two `account created` |
| Backend down at boot | **pass** | boot finished in the same second as with the backend up; offline; 9 failed calls over 11 s caused 2 sign-ins and 2 warnings, not one per call |
| Backend restarted mid-session | **pass**, as a sign-in rather than a refresh (below) | restarted with a new JWT secret: the next call got `unauthorized`, the engine signed in again and the retry succeeded; 14 of 14 calls in the run succeeded |
| Concurrent 401s share one refresh (added) | **pass** | five `GetProfile`s at once on an expired token: one refresh, five successes; the backend logged no refresh-token reuse in any run |
| Sign-in never gates boot (added) | **pass** | `OnCreate` saw `signedIn=false`; "Boot complete" logged before "signed in" |
| Invalid profile (added) | **pass** | `--profile=../evil`: refused at init, no directory created, every call fails with the reason |
| Linux (WSL2, gcc 11.4) | **pass** | builds with no warnings outside `extern/`; `~/.local/share/GanymedEngine/profiles/o2lin/device_id`, mode 600; two runs, same account, against the Compose backend |

**Where the plan was wrong or incomplete, kept visible:**

- **"On a 401, refresh once" is only right for an expired token.** The contract has two 401s:
  `token-expired` (refresh) and `unauthorized` (sign in again). A backend restarted with a new
  secret answers `unauthorized`, and refreshing then would only waste a round trip on a session
  that is fine on the backend's side but whose token it can no longer read. So the plan's row
  "the next call refreshes" passed as "the next call signs in again".
- **Sharing one refresh is not enough; a request needs to know which token it used.** A request
  answered 401 after another request already recovered the session would otherwise start a second
  recovery. Each request records the session generation it was sent with, and a stale 401 just
  retries. With rotating refresh tokens, a second concurrent refresh is reuse and revokes the session.
- **The refresh token is dropped the moment it is sent.** Not in the plan: if the network fails
  mid-refresh, the backend may already have spent it, and presenting it again would be reuse. The
  next recovery signs in instead.
- **"No automatic retries" needed a companion rule.** With no retry loop, a backend that is down at
  boot leaves the game offline for good unless something signs in later. An authenticated request
  now waits for a sign-in started on its behalf, and a 5-second backoff after a failed sign-in stops
  a per-frame caller from turning that into a sign-in, and a warning, every frame.
- **O2 needed a request to verify against**, and the plan named only local reads
  (`IsSignedIn`, `GetPlayerName`). `Backend.GetProfile` was added: typed, and something scripts
  need anyway.
- **The session got its own layer.** `OnlineSession.cpp` implements `Online`'s API over a private
  `OnlineTransport` (single HTTP attempts, still the only TU with IXWebSocket), so the token and
  retry rules are not tangled with the network code.
- **An invalid `--profile=` is an error, not a fallback to `default`.** Two test instances quietly
  sharing a player is the bug `--profile=` exists to prevent.
- **Not done: refreshing before expiry.** `expires_in` is ignored; the first 401 after expiry
  costs one extra round trip (40–100 ms on localhost). Recorded in online.md.
- **Not tested:** the `$XDG_DATA_HOME`-set branch on Linux (the fallback was), and two *editor*
  instances (two runtimes were). macOS's directory is written but unbuilt.

---

## Phase O3 — leaderboards, and the Proving Ground uses them — **bindings done; game half open**

### Goal

A Lua script submits a score and reads a ranked list. The Proving Ground shows the player's best
and the top five on its HUD.

### Steps

1. Bindings in `ScriptBindings.cpp`, on master:
   ```lua
   Backend.SubmitScore(self.entity, "proving-ground", score, function(ok, result) end)
   Backend.GetLeaderboard(self.entity, "proving-ground", 5, function(ok, entries) end)
   Backend.GetMyStanding(self.entity, "proving-ground", function(ok, result) end)
   ```
   `result` is `{ rank, best }`; `entries` is an array of `{ rank, name, score }`. Plain Lua
   tables, built by the binding. Scripts never see JSON. Three mapping rules the binding owns,
   against `api-v0.2`:
   - The API's field is **`display_name`**; the binding exposes it as `name`.
   - `GetMyStanding` for a player with no score yet gets `rank` and `best` as JSON **`null`**,
     with a 200, not an error. The binding passes `ok == true` with both fields `nil`, so "no
     score yet" and "request failed" stay distinguishable.
   - On failure, `ok == false` and the second argument is the problem's `type` URN (for example
     `urn:ganymed:problem:not-found` for an unknown board), never the free-text `title`.
   `GetMyStanding` exists so the HUD can show the player's best at start-up, before any
   submission.
2. Every `SubmitScore` carries an **`Idempotency-Key`** header holding a UUID. The key identifies
   one **logical submission**, not one HTTP request: generate it once when the script calls
   `SubmitScore`, and **send the same key on every retry of that submission**. A fresh key per
   retry would make each retry a new score, which is exactly what the key exists to prevent. The
   backend (B2) remembers keys for at least 24 hours, answers a repeat with the original response
   (`Idempotent-Replayed: true`), and refuses a key reused for a different score with 422. The
   engine does not retry today (see [What it deliberately is not](#what-it-deliberately-is-not)).
   The key costs a header now, and it is what makes adding retries safe later, provided the retry
   path reuses it.
3. On `first-game`, **not master**: `Player.lua` submits on death, or on the gate route's end, and
   the HUD data model gains a leaderboard block.

### Decisions, with reasoning

**Client-submitted scores are forgeable, and that is accepted.** Anyone can `curl` a score.
Production fixes this with server-authoritative results (O5b's result report), or accepts it
and runs anomaly detection. Here, with one player, it is noted and ignored. It is the reason O5b
exists as more than plumbing.

### Verification

| Check | Pass when |
|---|---|
| Death submits | a row in Postgres with that score; the log shows the rank returned |
| HUD | the top five render after one round trip, and update after a new best |
| Duplicate submit (probe: resend the same idempotency key) | one row, not two; the second response carries `Idempotent-Replayed: true` |
| No score yet | `GetMyStanding` calls back with `ok == true` and `nil` rank and best; the HUD shows a placeholder |
| Backend down | the HUD shows a placeholder; the game plays |
| Stop play before the response lands (editor) | no callback, and no HUD write into a torn-down document |

### Execution notes — steps 1 and 2, the bindings

2026-10-09, on `hello-online`. Live behaviour is in
[online.md](../engine/online.md#what-a-script-sees). Verified on the Windows x64 Debug runtime
against the real backend (host binary, `GS_ACCESS_TOKEN_TTL=2s`, a fresh profile), with a throwaway
test scene and a temporary probe that re-sent one submission verbatim; both are deleted. Linux
(WSL2, gcc 11.4) builds the bindings with no warnings outside `extern/`; it was not run.

| Check | Result | Evidence |
|---|---|---|
| No score yet | **pass** | `GetMyStanding`: `ok == true`, `rank` and `best` nil |
| Duplicate submit (probe: same key) | **pass**, and harder than planned | the probe and the original went out in the same frame and raced: the first to arrive was applied (`replayed=false`), the other got the stored response (`replayed=true`); one row |
| Retry after a 401 reuses the key (added) | **pass** | a submission on an expired token: refresh, retry, applied once |
| All submissions applied once (added) | **pass** | four sent, one twice and one retried: 3 rows and 3 keys in `score_submissions` / `score_idempotency_keys`, best 2000 |
| Top N (added) | **pass** | 4 rows, ties 1, 2, 2, 4, integer scores, `isMe` on the player's row |
| Bad arguments refused, nothing sent (added) | **pass** | fraction, negative, 2^53, a string score, `"../../auth/device"` as a board, limits 0 and 101 |
| Unknown board | **pass** | `ok == false`, `urn:ganymed:problem:not-found` |
| Death submits, HUD, backend down, stop play | **open** | step 3, the game half, on `first-game` |

**Where the plan was wrong or incomplete, kept visible:**

- **A board name is a path segment and needed validating.** The plan named the bindings and their
  results, not their inputs. A script-supplied board goes into the URL, so it is held to `a-z`, `0-9`
  and `-`; `"../../auth/device"` would otherwise reach another route.
- **Scores are checked at the call.** The plan relied on the writer's whole-number rule. A fraction is
  also refused there now, with the binding's name, instead of being sent and refused by the backend
  with a 400. Truncating quietly, as `UI.SetScore` does for display, would change what is recorded.
- **`replayed` is exposed** (`{ rank, best, replayed }`). The plan had `{ rank, best }`, but a replayed
  standing is as of the first submission, and a script deserves to know which it got. That needed
  response headers in `OnlineResponse`, which it now carries.
- **`GetLeaderboard` rows carry `accountId` and `isMe`**, beyond the plan's `{ rank, name, score }`,
  so the HUD can highlight the player.
- **The race was found by accident.** The engine's over-a-second first frame pushed the test's
  schedule into one update, so four sequential steps ran concurrently. It turned the duplicate check
  into a concurrent one, which the backend handled as B2 designed; and it showed that two
  submissions in flight report standings in whatever order the backend processed them.

---

## Phase O4 — the push channel

### Goal

One persistent WebSocket per signed-in client carries server-initiated messages (presence,
party invites, later "match found"). It reconnects on its own, and Lua subscribes by message
type. Scripts can form a party, so the pushes have something to be about.

Against `api-v0.3`: [`realtime.md`](https://github.com/giska1923/GanymedServer/blob/api-v0.3/docs/api/realtime.md)
is the socket's contract, and the party routes are in `openapi.yaml`. The rules below are the
engine's side of it.

### Steps

1. One `ix::WebSocket` in `Online.cpp`, opened after sign-in with the session token. The
   upgrade request carries it as an `Authorization: Bearer` header (backend B3's choice: a native
   client can set headers on the upgrade, where a browser cannot). **The token is checked once,
   at the upgrade:** the socket stays valid when the access token later expires, so there is no
   re-authentication over the socket. A failed upgrade is an HTTP 401 with a problem body:
   `token-expired` means refresh (O2) and reconnect; `unauthorized` means sign in again.
2. **The socket is receive-only.** The engine never sends a data message on it; one would get
   the socket closed with `1008`. Every action is an HTTP call (step 5). IXWebSocket answers the
   server's pings itself, every 10 s, which is the only traffic the engine sends.
3. Its message callback runs on IX's thread (per the IX docs) and follows the same path as O1:
   copy, `SubmitToMainThread`, route by message type. **Unknown types are ignored, not errors:**
   the backend adds types without a version bump.
4. **Reconnect by close code**, with the backoff capped and always jittered:

   | Close | Meaning | Engine does |
   |---|---|---|
   | `1001` | that replica is shutting down | reconnect at once (another replica takes it) |
   | `4001` | replaced by a newer socket for this account | **do not reconnect.** Another session of this account is active, and reconnecting would kick that one, which kicks this one, forever. Log it and surface it |
   | `1008` | policy violation (a data message, or too slow to keep up) | reconnect with backoff, and log loudly: it is an engine bug |
   | `1006`, or any other | the connection dropped | reconnect with exponential backoff and jitter |

5. **Re-fetch state on every (re)connect.** Pushes are at most once: anything sent while the
   socket was down is gone, never replayed. So as soon as the socket is up, the engine fetches
   `GET /v1/party` and `GET /v1/party/invites` and hands the results to whoever displays them.
   That makes the client correct whether or not any push ever arrives; pushes only make it
   prompt.
6. `Backend.Subscribe(self.entity, "party.invite", function(msg) end)`, owned and cancelled
   exactly like O1's requests. Delivery goes through the same mailbox. The types today are
   `party.invite`, `party.updated` and `party.removed` (payloads in `realtime.md`). Each is a nudge
   to re-fetch, not the state itself. A script that gets `party.updated` calls `Backend.GetParty`.
   The matchmaking types arrive with O5a.
7. Party bindings, thin wrappers over the `api-v0.3` routes in the same callback shape as O3:
   ```lua
   Backend.GetParty(self.entity, function(ok, party) end)        -- party is nil when in none
   Backend.CreateParty(self.entity, function(ok, party) end)
   Backend.InviteToParty(self.entity, accountId, function(ok) end)
   Backend.GetPartyInvites(self.entity, function(ok, invites) end)
   Backend.AcceptPartyInvite(self.entity, partyId, function(ok, party) end)
   Backend.DeclinePartyInvite(self.entity, partyId, function(ok) end)
   Backend.LeaveParty(self.entity, function(ok) end)
   Backend.KickFromParty(self.entity, accountId, function(ok) end)
   ```
   `party.members[i]` carries `name` (mapped from `display_name`, as in O3), `status`
   (`"online"`, `"away"` or `"offline"`) and `leader`. On failure the second argument is the
   problem's `type` URN, for example `…:party-full` or `…:not-party-leader`.

### Decisions, with reasoning

**Messages are delivered, not replayed.** A message that arrives while no script is subscribed is
dropped and counted. Anything that must survive a disconnect is fetched over HTTP after
reconnect. That is the standard split: the socket is a *nudge* ("you have an invite") and the
HTTP API is the *source of truth* ("list my invites"). A backend that treats the socket as the
source of truth needs delivery guarantees, and that is a much harder system.

**Jitter is not optional.** When the backend restarts, every client disconnects at the same
instant. Fixed-interval reconnects then arrive at the same instant too, every time: a thundering
herd. With one client that is invisible. It is recorded here because the habit is the point.

**`4001` is the one close that must not reconnect.** The backend enforces one socket per account,
and the newer socket always wins. Two engine instances on one profile (two editor play sessions,
say) that both reconnect on `4001` would replace each other forever, each connection killing the
other. Treating `4001` as final is what breaks the loop. Two instances that *should* both be
online are two `--profile`s (O2).

**Presence has a 30 s grace, so a reconnect inside it is invisible to the party.** A dropped
socket leaves the player `away` for 30 s, and reconnecting in that window keeps their party seat
with nothing sent to anyone. That is why the reconnect schedule's first attempts should be fast
(well under a second, plus jitter). Backing off to the cap only matters for a backend that stays
down.

### Verification

| Check | Pass when |
|---|---|
| Push arrives | a backend-sent test message reaches a subscribed script inside its scene update |
| Backend restart | the client reconnects, re-authenticates if needed, and the log shows the backoff schedule |
| Replica shutdown (`docker compose stop backend-b` with the engine on it) | close `1001`; reconnect at once; party seat kept (the player was only `away`) |
| Same profile twice (two instances) | the older instance gets `4001`, logs it, and **stays disconnected**: no ping-pong between the two |
| Re-fetch on reconnect | invite the player while their socket is down; after reconnect, the invite appears from `GET /v1/party/invites` with no push involved |
| No subscriber | the message is counted as dropped, with no error spam |
| Unknown message type (`docker compose exec redis redis-cli PUBLISH user:<account> '{"type":"x.new","id":"1"}'`) | ignored, logged at debug, no error |
| Stop play | subscriptions for that scene are gone; the socket stays up (it belongs to the application, not the scene) |

---

## Phase O5 — matches: the client joins, `GanymedDedicated` hosts

Two halves with different blockers, so they are planned separately:

| Half | What | Blocked on |
|---|---|---|
| **O5a** | The client: queue, follow the ticket to a server, be admitted by it | nothing. The backend side is done, and `stubserver` is a real game server to join |
| **O5b** | `GanymedDedicated`: be allocated, admit players, report the result | the dedicated-server milestone, which is not planned |

Both are written against `api-v0.5`. The tickets and their states are in
[`openapi.yaml`](https://github.com/giska1923/GanymedServer/blob/api-v0.5/docs/api/openapi.yaml), the `match.*` pushes in
[`realtime.md`](https://github.com/giska1923/GanymedServer/blob/api-v0.5/docs/api/realtime.md), the token in
[`connect-token.md`](https://github.com/giska1923/GanymedServer/blob/api-v0.5/docs/api/connect-token.md), and the game server's side in
[`server-lifecycle.md`](https://github.com/giska1923/GanymedServer/blob/api-v0.5/docs/api/server-lifecycle.md).

### O5a — the client side

#### Goal

A script queues the player, or their whole party, for a mode. It follows the ticket until a
server is allocated, and the engine joins that server. Before netcode exists, being admitted is
the end of the line. A `WELCOME` from `stubserver` proves matchmaking, allocation, the token and
the UDP path all work, with the engine as the client.

#### Steps

1. Bindings, in the callback shape of O3 and O4:
   ```lua
   Backend.Queue(self.entity, "coop", function(ok, ticket) end)   -- alone, or the party if leader
   Backend.GetTicket(self.entity, function(ok, ticket) end)       -- latest ticket, any state; nil if none
   Backend.CancelTicket(self.entity, ticketId, function(ok) end)  -- only while queued
   Backend.JoinMatch(self.entity, function(ok, accountIdOrReason) end)
   ```
   `ticket` is `{ id, mode, state, players, failureReason, match = { id, players },
   server = { address }, result = { outcome, ratingChange } }`. `match`, `server` and `result`
   are `nil` until they apply. On failure the second argument is the problem's `type` URN, as in
   O3: `…:already-queued`, `…:not-party-leader`, `…:ticket-not-queued`. **`already-queued` also
   answers a player whose match is still running.** A player stays in one active ticket until the
   match ends, not only until it is matched.
2. **The connect token never reaches Lua.** It is a credential, and it gets the session token's
   rule (O2): the engine holds it for the one call that needs it. That is why there is no
   `ticket.server.connectToken`, and why joining is the engine's job (`JoinMatch`), not a script's.
3. **States** (`api-v0.5`): `queued → matched → allocating → ready → finished`, or `cancelled`, or
   `failed` with `failureReason` `timeout`, `no_server`, `allocation_failed` or `server_lost`.
   **`matched` is not final.** It was in `api-v0.4`. The binding passes an unknown state through
   unchanged, and a script treats an unknown state as "still waiting". This is the same rule as
   unknown push types (O4), and for the same reason: the backend adds states.
4. Pushes, as nudges: `ticket.updated`, `match.found`, `match.ready`, `match.finished` and
   `ticket.failed`. **O4's re-fetch on (re)connect gains `GET /v1/matchmaking/ticket`.** A client
   that reconnects during allocation must still end up `ready` with no push involved.
5. `JoinMatch` reads the ticket. Every read mints a fresh token, valid for 30 s. It then sends one
   UDP datagram, `HELLO <token>`, to `server.address`, and waits for one back: `WELCOME
   <account_id>` or `DENIED <reason>`. It runs off the main thread like all network I/O, and
   completes through O1's mailbox. **On a timeout it starts again from the ticket read, with a
   fresh token**, up to 3 attempts of 1 s each. Resending the same token is wrong:
   - UDP loses datagrams. If the server got the `HELLO` and its `WELCOME` was lost, the same token
     again is a replay (`connect-token.md` rule 8) and gets `DENIED`.
   - A fresh token has a fresh nonce, and the server admits an already-admitted player again.
     The backend measured this: one account, two tokens, two `WELCOME`s.
   - The same rule covers a slow client. If loading took 40 s, the token from `match.ready` is
     dead, and a new read is the fix.

   `DENIED` reasons are for the log. The contract says not to parse them.
6. The UDP socket lives in `Online.cpp`, beside IXWebSocket, behind the same one-TU firewall and
   after the same `initNetSystem`. IXWebSocket ships a minimal `ix::UdpSocket` (`init(host,
   port)`, `sendto`, `recvfrom`, no timeout of its own), which may be enough for one datagram and
   a wait; raw Winsock is the fallback if its blocking behaviour does not fit. Found while doing
   O0; this step said earlier that IXWebSocket has no UDP. The netcode milestone owns what happens after `WELCOME`
   (`server-lifecycle.md`: "a real game would continue with its own protocol"). So this
   handshake is the first exchange of that future protocol, and should be written so the socket
   can be handed over rather than reopened.

#### Verification

Against the backend's own fleet: `fleetagent -pool 2 -- bin/stubserver.exe -match-seconds 30s`
(see the backend's `docs/backend/gscli.md`).

| Check | Pass when |
|---|---|
| Two profiles queue (`--profile=b`) | both tickets `queued → matched` after the 10 s fill wait, then `ready`; `match.ready` reaches each subscribed script inside its scene update |
| Join | `JoinMatch` → `ok == true` with the player's own account ID; `stubserver` logs the join |
| Party queue | the leader queues both; a member's `Queue` fails `…:not-party-leader` |
| Slow client | wait 40 s after `ready`, then `JoinMatch` → `WELCOME` (a fresh token, read inside the call) |
| Server gone | kill the stub before joining: three timeouts, `ok == false`; then `ticket.failed` with `server_lost` within ~5 s |
| No fleet | stop the agent: the ticket fails `no_server` 30 s after matching |
| Finished | after the stub's 30 s, `match.finished` with `ratingChange == 16`; `GetTicket` shows `result` |
| Reconnect during allocation | drop the socket right after queueing: after reconnect, the re-fetch alone shows `ready` |
| Queue while in a match | `…:already-queued` until `finished` |
| The token stays out of Lua | no binding returns it; the engine log never prints it |

### O5b — `GanymedDedicated` hooks (blocked)

**Blocked on** a dedicated-server milestone that does not exist yet (`GanymedDedicated`, a
headless `ApplicationSpecification`, a Scene role, netcode). The backend side is done, and the
contract is fixed by `server-lifecycle.md` and `connect-token.md`. **This replaces the earlier
sketch here**, which had the server push its states to the agent with a per-process secret and
left the signature algorithm open. Neither survived contact with the backend's design.

The backend's [`cmd/stubserver`](https://github.com/giska1923/GanymedServer/blob/api-v0.5/cmd/stubserver/main.go)
does all of the following in about 270 lines of Go. It is the reference: read it first.

What `GanymedDedicated` needs from `Online/`:

- **Spawn flags.** `--server-id`, `--agent`, `--game-port`, `--advertise`, `--public-key`, read the
  way `--backend=` is (O1). Refuse to start without them.
- **The lifecycle, as an HTTP client only.** The server opens no HTTP listener. It calls its
  agent on localhost, which is the Agones SDK model, and it means O1's client is all it needs.
  - Bind the UDP game port **before** the first `ready`. A server that cannot accept players
    must never be offered.
  - `POST …/ready` is a long-poll held up to 20 s (`204`: ask again; `200`: the allocation).
    **O1's `transferTimeout` must be overridable per request**, or this call times out by
    design.
  - `POST …/allocated` **as soon as the allocation arrives, before loading the map**. The backend
    withdraws an allocation that is not acknowledged within 5 s. A `4xx` means: do not run the
    match, shut down.
  - `POST …/health` every 2 s; 6 s of silence gets the process killed. **Health must not depend
    on the frame loop.** A 6 s map load on the main thread would otherwise get a healthy server
    killed. Drive it from a timer on the network thread.
  - `POST …/shutdown`, then exit. Any non-2xx from the agent means exit as well.
- **Connect-token verification**, rules 1–8 in order. The contract fixes **Ed25519 (RFC 8032)**,
  so the HMAC option is gone: a server holds only the public key and cannot mint a token. What
  is left to choose is the library:
  - **Monocypher**, with a caution I am fairly sure of but have not checked against a pinned
    version: its default `crypto_eddsa_*` functions use BLAKE2b, which is *not* RFC 8032
    Ed25519. The standard variant (SHA-512) is in its optional `monocypher-ed25519` files.
    Linking the default one would reject every backend signature.
  - **libsodium** is the larger, unambiguous alternative.

  Either way the acceptance test is cross-language: tokens minted by the backend's Go
  `connecttoken.Mint` must verify in C++, and a token with one changed byte must not. The rest
  is small:
  - base64url without padding, hand-written, about 30 lines;
  - the claims parsed with O1's JSON reader, **after** the signature checks out;
  - 5 s of leeway on `iat` and `exp`;
  - a nonce map with expiry, for replays.
- **The result**: `POST {result_url}` with `Authorization: Bearer <result_token>` and
  `{"outcome": "victory" | "defeat"}`.
  - Retry network errors and `5xx` with backoff. The endpoint is idempotent, so a retry after an
    unseen success is harmless.
  - Stop on `4xx`. A `409` means a different outcome was already recorded.
  - The result token is a credential: never logged, never in Lua.
  - **Check the contract before implementing this.** The backend has an open item to route results
    through the agent, because today they reach only one backend replica. That would change this
    call, and arrive as a new `api-v0.x` tag.

**The acceptance test exists already.** Run the backend's agent with `GanymedDedicated` in place
of the stub, `fleetagent -- GanymedDedicated.exe`, and the backend's load test,
`gscli load coop 4`. It must pass the same four stages it passes against `stubserver`: queue
drained, servers ready, `WELCOME:4`, finished with results.

---

## Explicitly not doing

- TLS, OAuth, platform identity, crash reporting, telemetry, patching (see
  [What it deliberately is not](#what-it-deliberately-is-not)).
- A generic HTTP or JSON surface in Lua.
- Persisting any token to disk.
- A retry or outbox queue.
- An editor panel for online status. The log and `Backend.IsSignedIn()` are enough until
  something proves otherwise.

## Design tensions, recorded

- **Plain HTTP is only acceptable because the backend is on localhost.** The day it moves to a
  VPS, the session token crosses the internet in clear text. That day reopens O0's decision, and
  libcurl with Schannel is then the likely answer on Windows.
- **The JSON shortcut.** yaml-cpp as a JSON parser is a bet that the backend's payloads stay
  inside the common subset. O1's `/edge` probe is the evidence. The first real payload that breaks
  it ends the bet.
- **An unmaintained-ish transport.** IXWebSocket is pinned and small enough to read. If it
  becomes a problem, O1's facade is the only file that knows its name, and that is the point of
  the firewall.
- **Engine-level, not runtime-level.** `Online` lives in the engine, not in `GanymedRuntime`,
  because scripts run in the editor's play mode too, and a `Backend.SubmitScore` that works only
  in the runtime would make every gameplay change unverifiable in the editor.

## Docs this milestone must update

| Phase | Doc |
|---|---|
| O0 | [build-and-tooling.md](../engine/build-and-tooling.md): dependency table, `extern/IXWebSocket.lua`, the defines it had to supply |
| O1 | **new** `docs/engine/online.md`, indexed from [docs/README.md](../README.md); [architecture.md](../engine/architecture.md): module layout, the frame diagram, ownership; [scripting.md](../engine/scripting.md): the mailbox drain in `LuaScriptSystem`. **Done** |
| O2 | `online.md`; [platform.md](../engine/platform.md): the user-data directory; [runtime.md](../runtime/runtime.md) and [editor.md](../editor/editor.md): `--backend=` and `--profile=` |
| O3 | [scripting.md](../engine/scripting.md): `Backend.*` bindings. Game-branch HUD changes are recorded in `first-game`'s Proving Ground doc |
| O4 | `online.md` (reconnect rules by close code); [scripting.md](../engine/scripting.md): `Backend.Subscribe` and the party bindings |
| O5a | `online.md` (matchmaking, `JoinMatch`, the retry-with-a-fresh-token rule, the UDP socket); [scripting.md](../engine/scripting.md): the matchmaking bindings |
| O5b | `online.md` (the lifecycle, token verification, the result call), plus the dedicated-server milestone's own docs |

## Found while planning, out of scope

- **No dedicated-server milestone exists.** O5b depends on one. Its shape was discussed
  (server-authoritative, a headless `ApplicationSpecification`, a Scene role gating the
  presentation systems, snapshot interpolation plus local prediction on `CharacterVirtual`), but
  no plan is written. The backend went ahead without it, with a Go stub server standing in. It is
  now the only thing between the backend and a playable online match.
