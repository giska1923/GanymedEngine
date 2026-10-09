# Online (the backend client)

[`GanymedE/Online/`](../../GanymedEngine/source/GanymedE/Online) is the engine's HTTP client to the
backend ([GanymedServer](https://github.com/giska1923/GanymedServer), a separate Go repository). It
holds the player's identity and session, issues requests without ever blocking a frame, and hands
responses to Lua inside the owning scene's script update. A response can never reach a destroyed
scene or a destroyed script instance. The milestone that builds it, and what comes next
(leaderboards, the push socket, matches), is [ONLINE.md](../ToDo/ONLINE.md).

| File | Holds |
|---|---|
| [Online.h](../../GanymedEngine/source/GanymedE/Online/Online.h) | The public facade: `Send`, `Cancel`, status, identity, stats |
| [OnlineSession.cpp](../../GanymedEngine/source/GanymedE/Online/OnlineSession.cpp) | `Online`'s implementation: `--backend=` and `--profile=`, the device ID, sign-in, the session, 401 recovery and the one retry |
| [OnlineTransport.h](../../GanymedEngine/source/GanymedE/Online/OnlineTransport.h) / [Online.cpp](../../GanymedEngine/source/GanymedE/Online/Online.cpp) | Private: single HTTP attempts over a pool of four clients, and the push socket. **Online.cpp is the only engine TU that includes IXWebSocket** |
| [Json.h](../../GanymedEngine/source/GanymedE/Online/Json.h) / [.cpp](../../GanymedEngine/source/GanymedE/Online/Json.cpp) | `JsonValue`, `ParseJson` (over yaml-cpp), `WriteJson` |
| [ScriptEngine.cpp](../../GanymedEngine/source/GanymedE/Scripting/ScriptEngine.cpp) | Who owns each request, per-scene mailboxes, cancellation, delivery to Lua |
| [ScriptBindings.cpp](../../GanymedEngine/source/GanymedE/Scripting/ScriptBindings.cpp) | The `Backend` table, JSON ↔ Lua |

## How engines usually do this, and where Ganymed differs

Unreal's `FHttpModule` runs requests on its own thread and fires completion delegates on the game
thread, from the HTTP manager's tick. Unity's `UnityWebRequest` is polled or awaited from a
coroutine. Both leave **lifetime** to the caller. A delegate bound to a destroyed actor, or a
coroutine on a destroyed object, is a classic bug in both, which is why Unreal grew `WeakLambda` and
`UObject`-bound delegates. Client SDKs for game backends (Nakama, PlayFab, EOS) sit on top of such a
layer, with typed calls per endpoint and their own retry and session handling.

Ganymed's layer is stricter about lifetime, because every caller is a script with a known owner:

- **A request is owned by a script instance** in a scene. Destroying the instance or stopping the
  scene cancels the request: the callback never runs, and the transport stops waiting on it
  (see [what cancel does and does not do](#what-cancel-does-and-does-not-do)).
- **Callbacks run inside the owning scene's script update**, with the scene context set, so a
  callback can use `self.entity` like `OnUpdate` does.
- **There is no generic HTTP from Lua.** Scripts get typed bindings per endpoint (O2 on), never
  `Http.Post(url)`. The VM's sandbox has no `io` or `os` either ([scripting.md](scripting.md#sandboxing)).

## A request's path, end to end

```
Lua: Backend.X(self.entity, ..., function(ok, result) end)
  └─ SendScriptRequest            checks the owner, records (owner UUID, callback, reader)
       └─ Online::Send            no session yet? wait for a sign-in (started if none is running)
            └─ OnlineTransport::Start   access token attached; least-loaded of 4 clients
                 └─ ix::HttpClient (async) ── its own thread ── network ──┐
                                                                     ▼
                               network thread: copy status + body, nothing else
                               └─ JobSystem::SubmitToMainThread
Application::Run ─ JobSystem::OnUpdate (top of the next frame)
  └─ transport: cancelled? ── yes ─▶ dropped, counted as dropped late
                 └─ no ─▶ session: a 401 on the first try? ── yes ─▶ recover the session, send once more
                                    └─ no ─▶ completion ─▶ that scene's mailbox (ScriptEngine)
Scene update ─ LuaScriptSystem::OnUpdate, scene context set
  └─ ScriptEngine::DeliverResponses, after the reactive drain and before any OnUpdate:
       owner instance still there and alive? ── no ─▶ skipped
                                             └─ yes ─▶ callback(ok, result), protected
```

There are three hops, and each one is needed:

1. **Network thread → main thread.** The Lua VM belongs to the main thread; touching `sol::state`
   from IXWebSocket's thread is a data race. The network thread copies the response and queues a
   main-thread job. It never touches `Online`'s own state, so the pending map needs no lock.
2. **Main thread → the scene's script update.** Bindings resolve entities through `ScriptEngine`'s
   scene context, which is set only while that scene's scripts run. A callback run from
   `JobSystem::OnUpdate` would have no scene context, or the wrong one when the editor holds a
   preview scene. So the completion only queues into the scene's mailbox.
3. **Cancellation in the middle.** `Online` drops a response whose request was cancelled, and
   `DeliverResponses` skips one whose owner is gone or disabled. Both checks run on the main thread,
   where destruction happens.

## The transport

`Online` is a static facade with `Init`/`Shutdown` owned by `Application`, like `AudioEngine`. It
knows nothing of scenes, scripts or owners. `Send(OnlineRequest, completion)` returns an id, and
`Cancel(id)` guarantees the completion never runs and stops the transport waiting on the request
([what cancel does and does not do](#what-cancel-does-and-does-not-do)). Under it, `OnlineTransport`
runs single HTTP attempts; the session layer above it decides what to send and with which token. Every entry point is safe
before `Init` and after `Shutdown`: `Send` then fails through the same completion path a network
failure takes.

- **The backend's base URL** comes from `--backend=<url>` (last one wins, like `--renderer=`).
  The default is `http://127.0.0.1:8080`, the port Compose publishes. `127.0.0.1` rather than
  `localhost` on purpose: on Windows a refused connect to `localhost` takes about 4.1 s against
  about 2.1 s, because both `::1` and `127.0.0.1` are tried. A request names only a path, so nothing
  can aim one at another host.
- **Every request** sends `Accept: application/json`, and `Content-Type: application/json` when it
  has a body, and `Authorization: Bearer <access token>` when it is authenticated (every route but
  sign-in and refresh). **Redirects are not followed.** The backend never sends one, and a redirect
  would forward that token to wherever `Location` points.
- **Timeouts:** 5 s to connect and 10 s for the transfer, overridable per request. IXWebSocket's own
  default transfer timeout is 1,800 s. 5 s clears a refused connect on Windows (about 2 s), and
  10 s is long for any call the backend answers today; a long-poll overrides it.
- **A pool of four clients.** IXWebSocket's async `HttpClient` is one thread running one request at
  a time off a queue (`HttpClient::run`). With one client, a slow request delays every request
  behind it. With four, a request goes to the client with the fewest outstanding. Measured: a `/ok`
  sent behind two 2-second requests was delivered on the next frame. The pool only moves the limit:
  20 two-second requests take five rounds of four, and 2.5 s after sending them, 4 had landed.
- **Transport errors are reworded** ("cannot connect to http://127.0.0.1:8080/ok", "timed out").
  IXWebSocket's text embeds `strerror`, and Windows reports a refused connect as "No error".
- **IXWebSocket is in exactly one TU**, `Online.cpp`. `IXNetSystem.h` redefines `EWOULDBLOCK`,
  `EAGAIN`, `EINVAL` and friends for whoever includes it ([build-and-tooling.md](build-and-tooling.md#dependencies-vendored-under-ganymedengineextern)).
  `Online.h` names no IXWebSocket or sol2 type.

**Stats** are kept by `Online` and logged at shutdown: sent, succeeded (2xx), failed (anything
else), cancelled (`Cancel` reached a pending request), retried (sent a second time after a 401),
sign-ins, refreshes, dropped late (a response came back for a cancelled request), and in flight.

## Identity and the session

### Profiles and the device ID

The backend's only credential is a **device ID**: random data generated once per install and kept
private (`POST /v1/auth/device`, the backend's `openapi.yaml`). The engine keeps one per **profile**:

- `--profile=<name>` picks it, default `default`. **Two instances on one machine are two players
  only if they are two profiles**; that is how every networked game is first tested, and without it
  both would sign in as the same account. A name is 1–32 of `A-Z a-z 0-9 - _`, because it names a
  directory. Anything else (`--profile=../evil`) is an error and the instance stays offline, rather
  than quietly becoming `default`: two test instances sharing a player by accident is a confusing bug.
- The file is `profiles/<name>/device_id` under the per-user data directory
  ([platform.md](platform.md#per-user-data)): `%LOCALAPPDATA%\GanymedEngine` on Windows,
  `~/.local/share/GanymedEngine` on Linux. Never beside the executable, whose directory a shipped
  game cannot write to.
- **Created on first use** as a random version-4 UUID, from `std::random_device` (the OS CSPRNG on
  both building platforms). It is written to a temporary file and renamed into place, so a crash
  cannot leave half an ID, and made owner-only where the OS has permission bits. **An existing file
  that does not hold a usable ID is never overwritten**: it may be the only copy of a real player's
  credential. The instance stays offline and says which file.
- **The device ID and the tokens are never logged.** The account ID and the profile name are.

### Signing in, and what "signed in" means

`Online::Init` reads the profile, loads the device ID, and **starts** a sign-in. It never waits for
it: boot does not depend on the backend. Status moves `Offline` → `SigningIn` → `SignedIn`, and a
script polls it with `Backend.IsSignedIn()`. Once signed in, the engine reads `/v1/me/profile` for the
player's display name (`Backend.GetPlayerName()`).

**The session lives in memory only**: the access token (15 minutes on the backend's default) and the
refresh token. Persisting the refresh token would save one localhost round trip per launch and put a
credential on disk; sign-in is cheap enough that it is not worth it.

An authenticated request made with no session **waits for a sign-in**, started on its behalf if none
is running. So a backend that was down at boot is picked up by the next call, with no retry loop
running in the background. A failed sign-in fails every waiting request with
`not signed in: <reason>` and logs **one** warning. For **5 seconds** after it, requests that need a
sign-in fail at once instead of each starting another: without that, a script calling every frame
against a dead backend would start a sign-in, and log a warning, sixty times a second.

### A 401, and the one retry

Every authenticated request gets **one** recovery and **one** retry on a 401:

- **`token-expired`** → refresh (`POST /v1/auth/refresh`), then send again with the new token.
- **Anything else** (`unauthorized`: a token signed with another secret, a revoked session) → sign in
  again, then send again.
- **A second 401** is the request's failure, delivered like any other.

Two rules come from the backend's **rotating refresh tokens**. Every refresh consumes the token and
returns a new one, and presenting a consumed token revokes the whole session:

- **Only one recovery runs at a time.** Every request that hits a 401 while one is running waits for
  it. Each request also remembers the session **generation** it was sent with. A 401 for a token that
  has already been replaced (a slow request, answered after another request's recovery finished) is
  retried with the current token, with no second recovery. This is the standard fix for the refresh
  race, and with rotation it is mandatory, not an optimisation: two concurrent refreshes count as
  reuse.
- **The refresh token is dropped from memory the moment it is sent.** If the network fails mid-refresh
  the backend may already have spent it, and presenting it again would be reuse. The next recovery
  then signs in instead. A refresh the backend refuses (`invalid-refresh-token`) is followed by a
  sign-in at once.

**Not done: refreshing before expiry.** The session's `expires_in` is ignored, and expiry is found
by the first 401 after it. That costs one extra round trip on that call (40–100 ms measured, on
localhost), against a timer per session and a rule for when it should fire. Worth revisiting if a
call ever cannot afford the round trip.

### Measured (O2, 2026-10-09, Windows x64 Debug runtime, against the real backend)

(O3's leaderboard checks are under [What a script sees](#what-a-script-sees).)

Host backend on 8081 with `GS_ACCESS_TOKEN_TTL=2s`, so expiry happens inside a run.

| Check | Result |
|---|---|
| First run, new profile | `device_id` created (36 characters + newline); signed in; `OnCreate` saw `signedIn=false`, and boot completed before the sign-in did |
| Second run, same profile | the same account ID |
| `--profile=o2b` beside it | a different account; the backend logged two `account created` |
| Five requests at once, token expired | **one** refresh, five successes, each 0.08–0.10 s after sending; the backend logged no refresh reuse |
| Backend restarted with a new JWT secret mid-session | the next call got `unauthorized`, the engine signed in again and the retry succeeded; 14 of 14 calls in that run succeeded |
| Backend down at boot | boot unchanged; offline; 9 failed calls over 11 s caused 2 sign-in attempts and 2 warnings |
| `--profile=../evil` | refused at init, no directory created, offline with the reason |

### Init order and shutdown

`Online::Init` runs right after `ScriptEngine::Init`, and `Online::Shutdown` right before
`ScriptEngine::Shutdown`, after `UIEngine`. Requests are owned by script instances, and their
callbacks are sol2 references into the VM. Shutdown also precedes `JobSystem::Shutdown`: it joins
the network threads, so nothing can queue a main-thread job into a scheduler being torn down.
Completions already queued are drained by `JobSystem::Shutdown` and find `Online` gone.

**Exit does not wait for requests in flight.** Destroying an `ix::HttpClient` sets `_stop`, and the
request's cancellation check reads it during the connect *and* the transfer, so the join is prompt.
Measured on the runtime: closing it with a 10-second request in flight took 0.62–0.75 s, against
0.64–0.71 s with nothing in flight. (The plan expected the opposite and planned an explicit abort;
removing that abort changed nothing measurable, so it is gone.)

## The push channel

One WebSocket per signed-in client carries server-initiated messages (the backend's `realtime.md`:
party invites and changes, and from O5a the match lifecycle). The rule it is built around is the
contract's: **pushes are nudges, delivered at most once.** Anything sent while the socket is down is
lost, never replayed, so the state of record is always the HTTP API, and a push only says "fetch
again".

### The socket

`OnlineTransport` owns one `ix::WebSocket` at `ws://<backend>/v1/realtime`, opened with the session's
access token as `Authorization: Bearer`. The token is checked once, at the upgrade: an open socket
outlives the token's expiry, and refreshing needs no reconnect.

- **IXWebSocket's own reconnection is off.** The session decides by close code (below), and one code
  must not reconnect on reflex. Off also avoids a `stop()` hang upstream fixed after our pin (#609),
  which needs that loop to race.
- **Compression is never negotiated** (`disablePerMessageDeflate`): IXWebSocket is built without
  zlib, so a compressed frame could not be read.
- **The handshake times out after 5 s** (IXWebSocket's default is 60).
- **The engine sends nothing on it.** A data message would get it closed `1008`. IXWebSocket answers
  the server's pings itself.
- Socket events reach the main thread through `JobSystem`, tagged with the socket's generation, so
  an event still queued from a socket since closed or replaced is dropped.

### Connecting and reconnecting

The session opens the socket after every successful sign-in. What happens when it closes depends on
the close code, as the contract asks:

| Close | Meaning | The engine |
|---|---|---|
| `1001` | that replica is shutting down | reconnects at once (0–0.25 s jitter): another replica is up |
| `1008` | policy violation (a data message, or too slow to keep up) | logs an **error** (it is an engine bug), then backs off |
| `1006`, a refused connect, any other | the connection dropped | exponential backoff with jitter: 0.25 s doubling to a 30 s cap, each delay drawn from its upper half |
| `4001` | replaced by a newer connection for this account | **one** more try ~5 s later; a second `4001` within a minute is final (status `"replaced"`) |
| an upgrade refused with **401** | the token is no good | one authenticated `GET /v1/me`, which recovers the session the way every request does (refresh, or sign in again), then reconnects at once; a second 401 in a row backs off |

**Jitter is not optional.** When a backend restarts, every client loses its socket in the same
instant, and a fixed schedule brings them all back in the same instant too. **The first attempts are
fast** because presence has a 30 s grace: a player who reconnects inside it keeps their party seat
with nothing sent to anyone.

**Why the upgrade's 401 needs a probe.** The contract sends a problem body with that 401
(`token-expired` or `unauthorized`), but IXWebSocket reports only the status. One authenticated call
tells the two apart and fixes either, through O2's recovery; the socket needs no token logic of its
own.

**Why `4001` gets one more try**, against the contract's "do not reconnect". Reconnecting on every
`4001` would make two live sessions of one account replace each other forever, which is the
contract's point. But a newer connection is not always a live session. In one replica-restart test the
backend logged an upgrade for the player's account arriving 1.7 s after the replica came back, at a
moment the engine sent nothing. It upgraded in 4 ms on a connection that was already dead, and it
closed the player's live socket `4001`. The likeliest source is Docker Desktop's port forwarder
delivering an upgrade request the engine had already given up on, while the replica was down. It did
not reproduce on demand. Never reconnecting would have left that player without pushes for the rest of
the session, and their party seat would have gone after the 35 s grace. One retry inside the grace
recovers from a ghost, and two real sessions swap once and settle.

**The reconnect timer** is `Online::OnUpdate`, called once per frame from `Application::Run` right
after `JobSystem::OnUpdate`: one clock read when nothing is due. When it fires with no session (the
backend was down at boot, say), it starts a sign-in through the same `GET /v1/me` probe. That makes the
push channel the one thing that keeps trying in the background, at its backoff, and so it is also what
brings an offline game back online.

### Subscriptions, and the `connected` event

```lua
Backend.Subscribe(self.entity, "party.invite", function(msg) end)
Backend.Unsubscribe(self.entity, "party.invite")
Backend.GetPushStatus()   -- "disconnected", "connecting", "connected" or "replaced"
```

- A subscription is **owned** like a request: by `self.entity`'s script instance, and removed when
  the instance or its scene goes. Stopping play removes a scene's subscriptions; the socket belongs to
  the application and stays up. One subscription per (owner, type); subscribing again replaces the
  callback.
- A push reaches a scene through **the same mailbox as responses**: queued from `JobSystem::OnUpdate`
  (no scene context there), and delivered in `DeliverResponses`, before `OnUpdate`. A callback that
  errors disables its script.
- `msg` is the **wire envelope**, `{ type, id, payload }`, with the payload's keys as the contract writes
  them (`party_id`, `display_name`). A push is a nudge; the typed bindings that fetch the state are
  where names are mapped (`display_name` → `name`).
- **A push nobody subscribes to is dropped and counted**, logged at trace level. Not a warning: the
  contract adds types without a version bump, so an unknown type is normal.
- **`connected`** is raised by the engine on every (re)connect, with an empty id and a nil payload.
  Pushes sent while the socket was down are gone, so this is the moment to re-fetch whatever the script
  shows. A script fetches in `OnCreate` too: in the editor, Play starts long after the socket opened.

The plan had the engine re-fetch the party and invites itself on every reconnect. It announces
`connected` instead, because the engine does not know what a game shows (O5a adds the match ticket).
The guarantee is the same: a script that fetches on `connected` is correct whether or not any push
arrives, and pushes only make it prompt.

### Measured (O4, 2026-10-09, Windows x64 Debug runtime, against both Compose replicas)

The engine on replica b (8082); `gscli` players on replica a (8080), so every push crossed replicas
through Redis.

| Check | Result |
|---|---|
| Push arrives | a `party.invite` from a leader on the other replica reached the subscribed script inside its scene update; it accepted, and `party.updated` followed |
| Unknown type (`PUBLISH user:<account> {"type":"x.new"}`) | dropped and counted, trace log only |
| Replica shutdown (`docker compose stop backend-b`) | `1001` → reconnect in 0.12 s → backoff with jitter while it was down → connected 0.4 s after it came back; the leader saw the player `online` throughout, seat kept |
| Re-fetch on reconnect | an invite sent while the socket was down appeared in the `connected` re-fetch, with no push |
| Same account twice (a `gscli` session with the engine's device ID) | first `4001` → one retry ~5 s later (the other session got the `4001`); second `4001` → `"replaced"`, stayed off |
| Backend restarted after the token expired | dropped → backoff → upgrade refused 401 → `GET /v1/me` refreshed the session → connected, in the same second |

Not exercised: a `1008` (the engine never sends a data message, and the backend's slow-client close
was not provoked), and stopping play in the editor (the subscriptions go with the scene's record, the
same way its instances and requests do).

## Ownership, mailboxes and cancellation

`ScriptEngine` keeps, per scene (inside the same per-scene record as the instances, see
[scripting.md](scripting.md#instances-are-stored-per-scene)):

- `Requests`: script request id → owner UUID, the transport id, the Lua callback, and a *reader*
  that turns a 2xx response into the callback's second argument;
- `Mailbox`: responses that reached the main thread but not yet the scene's script update.

Script request ids are separate from `Online`'s ids. A request `Online` could not even queue
still completes, with a failure, and the script must hear about it the same way.

- **`DestroyInstance`** cancels the requests that instance owns, after its `OnDestroy`, so a request
  sent from `OnDestroy` is cancelled too.
- **`DestroySceneInstances`** (stopping play, a scene torn down) and `DestroyAllInstances` cancel
  every request in the scene.
- A response that arrives for a request no longer in `Requests` (cancelled after it arrived) is
  skipped. So is one whose owner is disabled by an earlier script error: a dead script gets no
  callbacks, like every other entry point.

Measured: a script entity destroyed with a 3-second request in flight → `cancelled=1`,
`droppedLate=1` and nothing in flight within 0.6 s, its callback never ran, and the driver's own
requests kept completing. In the editor, Stop pressed 14.5 s into a 30-second request: the
request was cancelled, its callback never ran, and the closing stats read `1 cancelled, 1 dropped
late`.

### What cancel does and does not do

`Cancel` sets IXWebSocket's per-request `cancel` flag, which the network thread checks between
waits on the socket. So:

- **It does** guarantee the callback never runs, keep a request that is still queued from ever being
  sent, and free the network thread within well under a second (measured above), so the next
  request on that client is not stuck behind a response nobody will read.
- **It does not** close the TCP connection, and it cannot stop the server. HTTP/1.1 has no way to
  say "never mind"; the server finds out only when it writes. In the editor check the stub slept its
  full 30 s and answered into a connection still open. The socket stays open, idle, until that
  client's next request replaces it or until shutdown: at most one idle socket per client, four in
  all.
- **A late response can never be mistaken for another request's answer.** IXWebSocket opens a new
  connection for every request (`HttpClient::request` creates `_socket` afresh, and replacing the
  old one closes it), so nothing is read from a socket a cancelled request used.

## What a script sees

```lua
Backend.X(self.entity, ..., function(ok, result) end)
```

- **The owner is explicit**, `self.entity`, and checked: it must be an entity with a live script
  instance in the current scene. Anything else (nil, an entity without a script, a non-function
  callback) raises a Lua error naming the binding, and **nothing is sent**. Like any script error,
  that disables the calling script unless it is inside `pcall`.
- **`ok` is true only for a 2xx.** Then `result` is whatever the binding's reader made of the body.
- **On failure, `result` is a string:** the problem's `type` URN for an
  `application/problem+json` response (`urn:ganymed:problem:not-found`); else `HTTP <status>`; else
  why no response arrived. A 2xx whose body the reader cannot use gives
  `ok == false, "malformed response: ..."`, not a half-read table.
- **A callback that errors disables its script**, logged once, exactly as a failing `OnUpdate` does
  ([scripting.md](scripting.md#errors)).
- Callbacks run **before** the instance's `OnUpdate` that frame. A request sent in `OnCreate` was
  delivered in the scene's first update, with `self.entity:GetName()` working inside the callback.

The bindings so far:

| Binding | Does |
|---|---|
| `Backend.IsSignedIn()` | `true` once a session exists. Local, no request |
| `Backend.GetPlayerName()` | the display name, or `nil` until it is known. Local, no request |
| `Backend.GetProfile(self.entity, cb)` | `GET /v1/me/profile` → `{ accountId, name, rating }`; `rating` is a Lua integer, `name` is the API's `display_name` |
| `Backend.SubmitScore(self.entity, board, score, cb)` | `POST /v1/leaderboards/<board>/scores` → `{ rank, best, replayed }` |
| `Backend.GetMyStanding(self.entity, board, cb)` | `GET /v1/leaderboards/<board>/me` → `{ rank, best }`, both `nil` with no score yet |
| `Backend.GetLeaderboard(self.entity, board, limit, cb)` | `GET /v1/leaderboards/<board>?limit=` → array of `{ rank, name, score, accountId, isMe }`, best first; `limit` 1–100 or `nil` (the backend's 10) |
| `Backend.Subscribe(self.entity, type, fn)` / `Unsubscribe(self.entity, type)` | pushes of `type` → `fn({ type, id, payload })`; see [the push channel](#subscriptions-and-the-connected-event) |
| `Backend.GetPushStatus()` | `"disconnected"`, `"connecting"`, `"connected"` or `"replaced"`. Local |
| `Backend.GetParty(self.entity, cb)` | `GET /v1/party` → `{ id, leaderId, members = { { accountId, name, status, leader } } }`, or `nil` when in none |
| `Backend.CreateParty(self.entity, cb)` | `POST /v1/party` → the party |
| `Backend.InviteToParty(self.entity, accountId, cb)` | `POST /v1/party/invites` (leader only) |
| `Backend.GetPartyInvites(self.entity, cb)` | `GET /v1/party/invites` → array of `{ partyId, from = { accountId, name }, expiresAt }` |
| `Backend.AcceptPartyInvite(self.entity, partyId, cb)` / `DeclinePartyInvite` | `POST /v1/party/invites/<id>/accept` → the party / `.../decline` |
| `Backend.LeaveParty(self.entity, cb)` / `KickFromParty(self.entity, accountId, cb)` | `POST /v1/party/leave` / `POST /v1/party/kick` (leader only) |

Every request binding is typed per endpoint, and maps the API's names to script-style ones
(`display_name` → `name`). There is no generic request.

**Arguments are checked before anything is sent**, and a bad one is a Lua error naming the binding,
like a missing owner:

- **A board name becomes part of the URL path**, so it is held to `a-z`, `0-9` and `-`. Without that,
  a string like `"../../auth/device"` would aim the request at another route.
- **A score must be a whole number from 0 to 2^53 − 1**, the contract's range. A whole float
  (`1234.0`, which is what script properties and arithmetic on them produce) is sent as `1234`. A
  fraction, a negative, a string or anything past 2^53 − 1 is refused at the call, rather than
  truncated quietly or refused by the backend a frame later.
- A limit is a whole number from 1 to 100, or `nil`.
- **A party ID goes into the URL path and an account ID into a body**, so both must be UUIDs
  (8-4-4-4-12 hex).

**A score is applied once, however often it arrives.** Each `SubmitScore` call generates one
`Idempotency-Key` (`Online::NewUuid`) and carries it in the request. The only time the engine sends a
request again is the session's one retry after a 401, and that re-sends the same request, key and all.
`replayed` is `true` when the response is the original one served again for a key the backend had
already processed: its standing is as of that first submission, not now. A submission's standing is
also only as current as its own response: two submissions in flight at once can complete in either
order, and each reports the board as it stood when the backend processed it.

`isMe` compares each row's account with the signed-in one, so a HUD can highlight the player without
holding account IDs itself. A failure's reason is the problem type, as everywhere:
`urn:ganymed:problem:not-found` for an unknown board.

Measured (O3, 2026-10-09, Windows x64 Debug runtime, against the backend with a 2 s token lifetime,
a fresh profile):

| Check | Result |
|---|---|
| No score yet | `GetMyStanding`: `ok == true`, `rank` and `best` nil |
| Duplicate submission (a probe re-sent one request, key and all, in the same frame as the original) | the two raced: whichever arrived first was applied (`replayed=false`), the other got the stored response (`replayed=true`); one row in `score_submissions` |
| A submission on an expired token | 401 → refresh → retried with the same key → applied once |
| The whole run | four submissions sent, one of them twice and one retried: **3 rows and 3 keys** in Postgres, best 2000 |
| `GetLeaderboard(…, 5, …)` | 4 rows, ties ranked 1, 2, 2, 4, integer scores, `isMe` on the player's row |
| Refused at the call, nothing sent | fraction, negative, 2^53, a string score, `"../../auth/device"` as a board, limits 0 and 101 |
| Unknown board | `ok == false`, `urn:ganymed:problem:not-found` | O1 was verified through a temporary generic
hook, registered only under an environment variable and deleted once the checks were done
([ONLINE.md](../ToDo/ONLINE.md), O1's execution notes). The TypeScript declarations are in
`GanymedEditor/scripts-src/types/ganymed.d.ts`.

## JSON

`JsonValue` is a plain recursive struct (null, bool, integer, number, string, array, object), with
object members kept in document order.

**Parsing goes through yaml-cpp**, which is already vendored: JSON as a Go service emits it is YAML
1.2 flow syntax. The one thing to get right is telling a string from a literal. yaml-cpp tags every
quoted scalar `"!"` and every plain one `"?"`, so `"123"` stays a string while `123` becomes a
number, and the same for `"true"`/`true` and `"null"`/`null`. Plain scalars are read only as JSON
allows: YAML 1.1 would also read `yes`, `on` and `0x1F`, none of which can appear unquoted in JSON.
A JSON integer that fits int64 stays an integer (2^53+1 exactly); any other number is a double. An
empty body is refused (YAML would read it as null).

**Writing** is hand-written and compact:

- **Lua integers are written exactly**, whatever their size: Lua 5.4 has an integer subtype, and
  `9007199254740993` is a real value there. (The plan assumed every Lua number is a double; Lua 5.4
  says otherwise.)
- **A whole-number float is written as an integer** (`1234.0` → `1234`, `3 * 1.0` → `3`). Script
  properties are floats ([scripting.md](scripting.md#all-numbers-are-floats)), and the backend
  decodes integer fields into int64, which refuses a fraction with a 400.
- **A whole-number float beyond ±(2^53 − 1) is refused** with an error, not sent: the double may
  already be a rounded neighbour of what the script computed. Non-finite numbers are refused too.
- **A Lua table is an array** if its keys are exactly 1..n, **an object** if they are all strings,
  and refused otherwise. An empty table is written `{}` (Lua cannot say which one was meant).
  Object keys are sorted, so the same table always sends the same bytes.
- Strings pass UTF-8 through unescaped and escape `"`, `\` and control characters.

Reading into Lua, JSON `null` becomes `nil`, so it is **absent** from the table: a script cannot
tell `{"a": null}` from `{}`. Integers stay Lua integers, and arrays become 1-based sequences.

Verified against a stub serving a payload with both `é` and raw UTF-8 `é`, a `null`, 2^53+1, a
nested array, quoted literals, `1.5`, `-7`, `2.5e3`, an escaped quote and backslash, and a `"k: v"`
string. Every field read back correctly, and writing the parsed table back produced JSON equal to
the original (nulls aside). One cosmetic loss: `2.5e3` is read as the float `2500.0` and written as
`2500`, the same number.

## Measured (O1, 2026-10-09, Windows x64 Debug runtime, against the stub)

| Check | Result |
|---|---|
| Callback timing | delivered inside `LuaScriptSystem` with the scene context set; a request from `OnCreate` landed in the first update |
| Entity destroyed mid-request | cancelled, dropped late, callback never ran; other requests unaffected |
| Stop play mid-request (editor) | cancelled at Stop, callback never ran; the server still answered, into an idle connection |
| Backend down (`--backend=` on a closed port) | `ok == false`, "cannot connect to ...", after ~2 s of play, which did not stop |
| Frame cost | 287 frames each: idle mean 6.98 ms (max 10.01), with 20 requests in flight mean 6.96 ms (max 12.53) |
| Exit with a 10 s request in flight | 0.62–0.75 s, the same as with nothing in flight |
| Ownerless call | refused with a named error; nothing sent |
| Whole-number floats | `1234.0` → `1234`; `2.0^53` refused, nothing sent |
| Head-of-line | a fast request behind two slow ones: next frame |
