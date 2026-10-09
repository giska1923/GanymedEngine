# Milestone — Online client (the engine side of the backend)

**Status: planned, not started.** The backend is complete (B1–B5, `api-v0.5`), so O0–O4 and
O5a have everything they need from it. O5b waits on a dedicated-server milestone that has not
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
| **O0** | Vendor IXWebSocket, a premake project, and a build on Windows and Linux | master | nothing | 1–2 days. Porting a CMake build to premake is the unknown |
| **O1** | `Online/` request layer: threading, ownership, cancellation, timeouts | master | nothing (a Python stub) | 2–3 days |
| **O2** | Identity: user-data dir, device ID, `--profile=`, session, `401` re-auth | master | **B1** (device auth) | 1–2 days |
| **O3** | `Backend.*` leaderboard bindings; the Proving Ground submits and shows scores | master + `first-game` | **B2** (leaderboards) | 1–2 days |
| **O4** | The WebSocket push channel: reconnect by close code, re-fetch on connect, Lua subscriptions, party bindings | master | **B3** (realtime gateway, `api-v0.3`) | 3–4 days |
| **O5a** | Matches from the client: queue, follow the ticket, join the server over UDP | master | **B4 + B5** (`api-v0.5`); O4 for the pushes | 2–3 days |
| **O5b** | `GanymedDedicated` hooks: lifecycle, connect-token verification, result | master | **B5** + the dedicated-server milestone | blocked, unsized |

These are estimates, not measurements. O1 is the phase that matters. Everything after it is a
consumer of O1's threading and ownership rules, and getting those wrong shows up as a crash on
scene stop a week later.

---

## Phase O0 — vendor the transport

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

---

## Phase O1 — the request layer

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
- **Connection-refused on Windows is slow.** I believe a connect to a closed localhost port on
  Windows takes on the order of a second or two, because the stack retries after the RST, where
  Linux fails in microseconds. It is async, so it costs no frame time. But it decides how fast
  the "backend is down" failure arrives. Measure it and record the number.
- **yaml-cpp and JSON edge cases.** `\u` escapes, very large integers, `null`. The probe payloads
  below include each.

### Verification

Against a throwaway Python stub, not committed, with endpoints `/ok`, `/delay/<s>`, `/status/<code>`
and `/edge` (a payload with `é`, `null`, a 2^53+1 integer and a nested array):

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

---

## Phase O2 — identity

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

---

## Phase O3 — leaderboards, and the Proving Ground uses them

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
6. The UDP socket lives in `Online.cpp`, beside IXWebSocket. IXWebSocket does not do UDP, so
   this is raw Winsock (`socket`/`sendto`/`recvfrom`) behind the same one-TU firewall as O0, and
   after the same `initNetSystem`. The netcode milestone owns what happens after `WELCOME`
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
| O1 | **new** `docs/engine/online.md`, indexed from [docs/README.md](../README.md); [architecture.md](../engine/architecture.md): module layout, the frame diagram, ownership; [scripting.md](../engine/scripting.md): the mailbox drain in `LuaScriptSystem` |
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
