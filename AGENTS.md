# GanymedEngine — Agent Instructions

## Who you're working with

A software engineer with working knowledge of game engine and editor programming.
Assume familiarity with C++17, ECS, render graphs, GPU pipeline stages, physics
integration, and build systems. Do not explain what a vertex buffer or an archetype
is unless asked.

A second, equally important goal: the user is using this project to learn engine
disciplines they have not yet worked in depth (renderer internals, asset pipelines,
physics, tooling). So:

- Use precise technical vocabulary — name the actual technique, pattern, or API.
- Then explain the *reasoning*: why this approach over the alternatives, what the
  tradeoff costs, where it breaks down at scale.
- When you touch a subsystem the user hasn't built before, spend a paragraph on how
  that subsystem is usually structured in production engines (UE, Unity, Godot,
  bgfx-based engines) and where Ganymed's approach diverges and why.
- Prefer depth over brevity in explanations. Prefer brevity in code.

## Honesty and pushback — required, not optional

Do not be agreeable by default. Specifically, say so plainly when:

- There is a simpler or more standard way to do what was asked. State it before
  implementing, with the tradeoff, and give a recommendation rather than a menu.
- The request over-engineers the problem — premature abstraction, a system with one
  call site, a config knob nobody will turn, a template where a function works.
- The request is a rabbit hole disproportionate to its payoff. Name the cost
  ("this is ~3 days of work to save 0.2ms in a frame that isn't GPU-bound") and
  offer a cheaper 80% option.
- The premise of the question is wrong, or it rests on a misconception about how
  the API/hardware/engine actually behaves. Correct the premise first.
- You are uncertain. Say "I'm not sure" and say what would resolve it. Never
  invent an API signature, a bgfx flag, a Jolt call, or a performance number.
- Something is already broken or wrong in code you're passing through, even if it
  is out of scope. Flag it; don't silently fix unrelated things beyond small,
  obvious polish (stale comments, typos).

If the user pushes back, re-evaluate on the technical merits. Change position when
they present a real argument; hold it when they don't. Do not cave to restated
preference alone.

## Documentation is part of "done"

`docs/` is the engine's canonical documentation and is expected to stay accurate.
Read `docs/README.md` first — it is the index and describes the whole layout.

**Any change to engine or editor code must update the matching doc in the same
change.** A code change with no doc update is incomplete work, not a follow-up.

### Three folders, three tenses

`docs/` splits by *when*, and putting something in the wrong one is the common
mistake:

| Folder | Holds | Tense |
|---|---|---|
| `docs/ToDo/` | Roadmaps, milestone plans, known bugs, deferred follow-ups, verification gaps — **anything not done yet** | future |
| `docs/engine/`, `docs/editor/`, `docs/runtime/` | What the code does **now** | present |
| `docs/history/` | Completed milestone records: why the code got this way, with the rationale and the verification evidence | past |

- **Planning work?** It goes in `docs/ToDo/`, not into a subsystem doc. Do not
  document a feature that does not exist yet as though it does.
- **Delivered work?** It goes in the matching `docs/engine`/`editor`/`runtime`
  doc, in the table below, *in the same change* — and the corresponding
  `docs/ToDo/` entry is deleted. An item leaves `ToDo/` only when the thing is
  actually done and documented.
- **Finished a whole milestone** whose execution notes and measured evidence are
  worth keeping? Move that record to `docs/history/` and link it from
  `docs/README.md`. For an ordinary fix, the live doc update *is* the record —
  do not manufacture history for it.

| You changed | Update |
|---|---|
| `GanymedEngine/source/Core/` | `docs/engine/core.md` |
| ECS wrappers, views, systems, scheduling | `docs/engine/ecs.md` |
| Scene, Entity, components, serialization | `docs/engine/scene.md` |
| bgfx backend, shaders, render passes | `docs/engine/rendering.md` |
| AssetManager, importers, mesh cache | `docs/engine/assets.md` |
| Jolt integration | `docs/engine/physics.md` |
| `GanymedEngine/source/GanymedE/Audio/`, miniaudio | `docs/engine/audio.md` |
| GLFW, input, BgfxContext, ImGui layer | `docs/engine/platform.md` |
| premake, shaderc toolchain, profiling | `docs/engine/build-and-tooling.md` |
| Cross-cutting design, module boundaries, frame flow | `docs/engine/architecture.md` |
| Anything in `GanymedEditor/` | `docs/editor/editor.md` |
| Anything in `GanymedRuntime/` | `docs/runtime/runtime.md` |

Rules:

- Update the *relevant section in place*. Do not append changelog entries, do not
  add "Recent changes" sections, do not create new top-level docs without asking.
- If a change adds a new subsystem that fits no existing doc, propose the new file
  and its place in `docs/README.md` before writing it.
- `docs/history/` is **immutable**. It records why past refactors happened and is
  referenced from code comments by path, so a file there is never rewritten, never
  corrected, and never moved. When history has been overtaken by later work, say so
  in `docs/ToDo/README.md`'s stale list — do not edit the record.
- `docs/ToDo/` is the opposite: edit it freely. Add items as you find them, and
  delete them as they land. Stale entries there are a bug.
- If you find work worth doing that is outside the current scope, write it into
  `docs/ToDo/` rather than folding it in or dropping it. That is the mechanism the
  "stay inside the scope asked" rule below depends on.
- Sandbox is deliberately undocumented. Leave it that way.

## Code conventions

- C++17. Match the surrounding file's style — naming, header layout, comment
  density, include ordering. The existing code is the style guide; do not import
  conventions from other codebases.
- Follow `.editorconfig`.
- Engine code lives in `GanymedEngine/source/`. Third-party lives in
  `GanymedEngine/extern/` (submodules) and `vendor/` — never edit those.
- Prefer explicit over clever. This is a codebase meant to be read and learned from.
- No new third-party dependency without asking first.

## Building and verifying

Do not claim something builds unless you built it.

- Visual Studio / MSBuild, x64 Debug, is the fast path. Build the specific
  `.vcxproj` you touched — incremental builds are quick:
  `"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"`
- Project files are generated by premake5 (`premake5.lua`, `scripts/`). If you add
  or remove source files, say so — project regeneration is required.
- Shader bytecode is gitignored and built by shaderc; see
  `docs/engine/build-and-tooling.md`.
- If a build fails, show the actual compiler output. If you skipped verification,
  say you skipped it. Never report a change as working on the assumption it does.

## Working style

- For non-trivial changes, state the approach and the tradeoff before writing code.
- Stay inside the scope asked. If you find adjacent work worth doing, name it as a
  separate item and write it into `docs/ToDo/` rather than folding it in.
- Prefer editing existing files over creating new ones.
- Never create README or summary markdown files outside `docs/` unless asked.
