# Audio

`GanymedEngine/source/GanymedE/Audio/` — a [miniaudio](https://miniaud.io)-backed playback engine
with two mixer groups and 3D spatialization, plus the `AssetType::Audio` file types.

`AudioEngine` is the sound-side counterpart to `Renderer`: a static facade over one global device,
`Init`/`Shutdown` by `Application`, with everything it owns living behind the `.cpp`. There is no
ECS surface yet — components, a system and Lua bindings are Phases 4–5 of
[`RUNTIME_AUDIO_ROADMAP.md`](../toDo&done/RUNTIME_AUDIO_ROADMAP.md).

## AudioEngine

[`AudioEngine.h`](../../GanymedEngine/source/GanymedE/Audio/AudioEngine.h) exposes:

| API | Behavior |
|---|---|
| `Init()` / `Shutdown()` / `IsInitialized()` | Open and close the device. `Application` calls them; re-`Init` after `Shutdown` in the same process works |
| `OnUpdate()` | Frees finished one-shots. Called once per frame from `Application::Run` |
| `CreateVoice(path, group, spatial, stream, loop)` → `VoiceId` | Load a file and attach a voice to a group. `InvalidVoiceId` (0) on failure, with the path and reason logged |
| `DestroyVoice(id)` | Stop and free |
| `Play(id)` / `Stop(id)` / `IsPlaying(id)` | `Stop` is *pause*: the cursor is kept and `Play` resumes from it. There is no separate `Pause()` |
| `SetVolume` / `SetPitch` / `SetLooping` / `SetPosition` | Per-voice state; push whatever you like every frame |
| `SetListener(pos, forward, up)` | The single listener |
| `PlayOneShot(path, group, position\|nullptr, volume)` | Fire and forget |
| `SetGroupVolume(group, v)` | `Master`, `Music`, `SFX` |
| `StopAll()` | Stop every voice, discard every one-shot |
| `GetVoiceCount()` / `GetOneShotCount()` | Diagnostics |

**Every one of those is a no-op when the engine is not initialized**, and that is the load-bearing
design decision in this file rather than defensive habit. `AudioEngine::Shutdown()` runs in the
`Application` *destructor body*; the `LayerStack` — and every scene inside it — is destroyed
afterwards, when members unwind. So a layer that stops its sounds in `OnDetach` is calling into an
already-uninitialized `ma_engine`, and nothing can reorder that without touching every app. This is
[`Renderer::IsGpuAlive`](rendering.md) applied to audio: same ordering problem, same solution. The
milestone's Phase 3 verification confirmed the sequence in a real run: `AudioEngine shut down` is
logged *before* the probe layer's `OnDetach` calls `StopAll()`.

The same flag carries the second failure this class has to survive: **no output device**. A headless
build machine or a VM with audio disabled makes `ma_engine_init` fail; `Init` logs it as an error,
leaves the flag false, and the game runs silent. Nothing downstream needs a special case.

`Init` has no ordering dependency on the renderer or the VM — audio touches neither. It sits after
`Renderer::Init` in the constructor so the boot log reads renderer → audio → scripting → UI, and so
a device failure is reported before anything slower runs.

`VoiceId` is monotonically issued and never reset, not even by a re-`Init`. An id held across a
`Shutdown`/`Init` pair therefore cannot alias a different sound afterwards; it just no-ops.

## Groups

`AudioGroup::Master` is the engine endpoint itself, so `SetGroupVolume(Master, v)` is
`ma_engine_set_volume`. `Music` and `SFX` are two real `ma_sound_group`s hanging off it. That is the
whole mixer. Arbitrary bus graphs, sends, ducking and snapshots are out of scope for v1; the first
acceptable follow-up is a data-driven group list, not a node editor.

Production engines (Wwise, FMOD, UE's audio mixer) put a full bus graph here because their content
teams need per-bus effects and mix snapshots. Ganymed has no DSP chain to hang off a bus, so a bus
graph would be a structure with nothing in it.

## Spatialization

`SetListener(position, forward, up)` and `SetPosition(voice, p)` take world-space vectors straight
through — **miniaudio's default handedness is right-handed with −Z forward**, which is glm's and the
engine's, so no conversion happens anywhere.

That claim is measured, not assumed. Reading miniaudio's mix back in `noDevice` mode with the
listener at the origin facing (0,0,−1), up (0,1,0), and an emitter swept along X at z = −2:

| emitter x | L (rms) | R (rms) | balance (R−L)/(R+L) |
|---|---|---|---|
| −8 | 0.0296 | 0.0060 | −0.66 |
| −4 | 0.0524 | 0.0111 | −0.65 |
| −2 | 0.0747 | 0.0175 | −0.62 |
| 0 | 0.0598 | 0.0598 | 0.00 |
| +2 | 0.0175 | 0.0747 | +0.62 |
| +4 | 0.0111 | 0.0524 | +0.65 |
| +8 | 0.0060 | 0.0296 | +0.66 |

Panning is symmetric about the listener's forward axis and **+X is its right**. Total energy falls
with distance under miniaudio's default inverse attenuation (min distance 1, rolloff 1, no max) —
compare the x = 0 row (2 units away) with x = ±8 (8.2 units away).

Attenuation parameters, cones and doppler are deliberately not exposed. Velocities stay zero, so
there is no doppler at all; feeding rigid-body velocity into a voice is the noted upgrade path if a
game ever needs it. Mono source files are the norm for 3D emitters, as everywhere else.

Passing `spatial = false` sets `MA_SOUND_FLAG_NO_SPATIALIZATION`, which *skips allocating* the
spatializer rather than bypassing it, and `SetPosition` on such a voice does nothing.

## Streaming vs decoding

`stream` is an explicit authored bool, not a size heuristic:

- `stream = false` → `MA_SOUND_FLAG_DECODE`. The whole file becomes PCM once and is shared by every
  voice on that path, so replaying an SFX costs nothing.
- `stream = true` → `MA_SOUND_FLAG_STREAM`. Decoded on the fly from disk; near-zero resident cost.

Measured on a 60 s mono WAV (5.0 MiB on disk), engine at 48 kHz:

| | `CreateVoice` | working-set delta |
|---|---|---|
| streamed | 11.6 ms | +0.2 MiB |
| decoded (first) | 289.8 ms | +11.0 MiB |
| decoded (same path again) | 0.07 ms | +0.0 MiB |

A size heuristic would guess wrong exactly at the boundary a human never mis-authors (a 4 MB ambience
loop), and it would make play-mode behavior depend on bytes on disk — invisible from the inspector.
Intent over inference.

## Loading is synchronous

`CreateVoice` and `PlayOneShot` do **not** pass `MA_SOUND_FLAG_ASYNC`. A missing or undecodable file
therefore fails at the call site that knows which path and which caller is at fault, instead of on a
job thread some frames later, and the behavior matches the rest of the asset layer, which is
synchronous throughout. The cost is a decode hitch the first time a clip is used — which is what the
`stream` flag is for on the long files where it would be noticeable, and which miniaudio's resource
manager charges only once per path (the 4000× second-load speedup above).

The upgrade path is `MA_SOUND_FLAG_ASYNC` plus a `ma_fence`. It is a bigger change than it looks,
because every caller's error handling moves off the call site.

## One-shots

`PlayOneShot` creates an engine-owned voice, starts it, and forgets it; `AudioEngine::OnUpdate`
frees the ones that have reached their end (`ma_sound_at_end`, miniaudio's own recycling signal).
Phase 3's verification fired 21 in 10 ms and watched the internal count return to 0.

The reap runs on a per-frame tick rather than lazily inside `PlayOneShot` because the leak it
prevents is time-shaped, not call-shaped: a game firing footsteps for ten minutes and then going
quiet would otherwise pin every clip it ever played until the next one-shot happened to run. It sits
*outside* the minimized gate in `Application::Run` — a window nobody is looking at has not stopped
making noise.

miniaudio has its own fire-and-forget helper, `ma_engine_play_sound`, and it is deliberately unused:
it hard-disables spatialization and pitch and takes no volume, so it cannot serve a positioned SFX.

## `AssetType::Audio`

`.wav`, `.mp3` and `.flac` map to `AssetType::Audio` in
[`AssetTypes.cpp`](../../GanymedEngine/source/GanymedE/Assets/AssetTypes.cpp). `.ogg` is absent
because miniaudio's built-in decoders are wav/flac/mp3 — Vorbis needs stb_vorbis vendored and wired
into the decoding backend. The enum is appended to, never reordered: it is persisted by ordinal in
`AssetRegistry.gr` (see [assets.md](assets.md)).

**There is deliberately no `GetAsset<AudioClip>`, no `Ref<AudioClip>` and no cache map.** Audio
follows the Script precedent, not the Texture one: the registry answers handle → path, and the
consumer loads itself. `ma_engine` already ref-counts decoded data by file path and shares it across
voices — a cache in `AssetManager` would be a second ref-counting owner of the same resource, and
two caches disagreeing about lifetime is a class of bug worth not having. The measured dedup above
*is* that cache working.

This is a divergence from the production norm, and it is a scope decision rather than a claim that
big engines are wrong: UE and Unity put audio behind the asset system because they cook and stream
banks, and a cooked bank is an engine-owned artifact that needs engine-owned lifetime. Ganymed has
no cooking. The upgrade path stays open because components already reference handles, not paths —
an `AudioClip` asset can appear the day cooking does, without touching a single scene file.

## miniaudio

Vendored as a committed single header at `GanymedEngine/extern/miniaudio/miniaudio.h` (v0.11.25) —
no submodule, following the cgltf precedent. Two TUs include it:

- `Audio/miniaudio_impl.cpp` — the one and only `MINIAUDIO_IMPLEMENTATION` in the engine.
- `Audio/AudioEngine.cpp` — declarations only.

The split exists so iterating on `AudioEngine.cpp` does not recompile the ~84k-line implementation.
Measured on x64 Debug, the whole engine builds in ~26 s; touching either audio TU costs ~3.5 s
against a ~2.6 s floor for a trivial TU, so the implementation is worth about **1 s** — the
`MA_NO_ENCODING`/`MA_NO_GENERATION` trims were skipped as not worth the cross-TU consistency
obligation they would create. Both includes are wrapped in `#pragma warning(push, 0)`, the spdlog
precedent; miniaudio compiles clean at the engine's warning level either way.

Paths reach miniaudio as `path::string()`, the platform's native narrow encoding. On Windows that is
the ANSI code page (miniaudio's default VFS calls `CreateFileA`), so a path outside it will not open.
Asset paths are ASCII in practice; miniaudio's wide-char entry points are the fix if that changes.

Platform link surface: nothing to add on Windows (WASAPI); on Linux miniaudio `dlopen`s ALSA and
PulseAudio, and `dl`/`pthread` are already in every app's link list; on macOS **every app** links
`CoreAudio.framework` and `AudioToolbox.framework`, because static libraries do not propagate links
off MSVC. See [build-and-tooling.md](build-and-tooling.md).

## Not in v1

Named so nobody half-builds them in passing: mixing beyond Master + Music/SFX, DSP effects (reverb,
filters, EQ), doppler, occlusion/obstruction, `.ogg`/Vorbis, an inspector preview-play button, and
asset packing/cooking with an `AudioClip` asset.
