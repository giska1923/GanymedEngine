# Online (the backend client)

[`GanymedE/Online/`](../../GanymedEngine/source/GanymedE/Online) is the engine's HTTP client to the
backend ([GanymedServer](https://github.com/giska1923/GanymedServer), a separate Go repository). It
issues requests without ever blocking a frame, and hands responses to Lua inside the owning scene's
script update. A response can never reach a destroyed scene or a destroyed script instance. The
milestone that builds it, and what comes next (identity, leaderboards, the push socket, matches), is
[ONLINE.md](../ToDo/ONLINE.md).

| File | Holds |
|---|---|
| [Online.h](../../GanymedEngine/source/GanymedE/Online/Online.h) / [.cpp](../../GanymedEngine/source/GanymedE/Online/Online.cpp) | The transport facade: `Send`, `Cancel`, stats, `--backend=`. **The only engine TU that includes IXWebSocket** |
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
       └─ Online::Send            picks the least-loaded of 4 clients, never blocks
            └─ ix::HttpClient (async) ── its own thread ── network ──┐
                                                                     ▼
                               network thread: copy status + body, nothing else
                               └─ JobSystem::SubmitToMainThread
Application::Run ─ JobSystem::OnUpdate (top of the next frame)
  └─ Online: cancelled? ── yes ─▶ dropped, counted as dropped late
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

## `Online`: the transport

`Online` is a static facade with `Init`/`Shutdown` owned by `Application`, like `AudioEngine`. It
knows nothing of scenes, scripts or owners. `Send(OnlineRequest, completion)` returns an id, and
`Cancel(id)` aborts the transfer and guarantees the completion never runs. Every entry point is safe
before `Init` and after `Shutdown`: `Send` then fails through the same completion path a network
failure takes.

- **The backend's base URL** comes from `--backend=<url>` (last one wins, like `--renderer=`).
  The default is `http://127.0.0.1:8080`, the port Compose publishes. `127.0.0.1` rather than
  `localhost` on purpose: on Windows a refused connect to `localhost` takes about 4.1 s against
  about 2.1 s, because both `::1` and `127.0.0.1` are tried. A request names only a path, so nothing
  can aim one at another host.
- **Every request** sends `Accept: application/json`, and `Content-Type: application/json` when it
  has a body. **Redirects are not followed.** The backend never sends one, and from O2 on a request
  carries a bearer token, which a redirect would forward to wherever `Location` points.
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
else), cancelled (`Cancel` reached a pending request), dropped late (a response came back for a
cancelled request), and in flight.

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

`Backend` has no endpoint bindings yet; identity (O2) and leaderboards (O3) add the first ones.
O1 was verified through a temporary generic hook, registered only under an environment variable,
and deleted once the checks were done ([ONLINE.md](../ToDo/ONLINE.md), O1's execution notes).

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
