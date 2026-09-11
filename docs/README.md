# GanymedEngine Documentation

The repository contains four projects:

| Project          | What it is                                                                                     |
| ---------------- | ---------------------------------------------------------------------------------------------- |
| `GanymedEngine`  | The engine static library. All code under `GanymedEngine/source/`.                             |
| `GanymedEditor`  | The editor application (scene editing, play mode, content browser).                            |
| `GanymedRuntime` | The standalone game player: boots a scene into play mode, renders to the backbuffer, no ImGui. |
| `Sandbox`        | A minimal test app (not covered by these docs).                                                |

## Documentation map

### Engine

| Document                                        | Covers                                                                                                                                                    |
| ----------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [Architecture overview](engine/architecture.md) | The big picture: module layout, frame flow, ownership, design principles                                                                                  |
| [Core](engine/core.md)                          | Application, entry point, layers, events, input, window abstraction, logging, UUID, assert/profiling macros                                               |
| [ECS](engine/ecs.md)                            | The view/access-wrapper ECS on top of entt: wrappers, accessors, change tracking, reactive views, systems, command queue, singletons, scheduling metadata |
| [Scene](engine/scene.md)                        | Scene, Entity, the component catalog, hierarchy, built-in systems, scene singletons, member reflection, serialization, `.gprefab` prefabs, play-mode copy |
| [Rendering](engine/rendering.md)                | The bgfx backend: view model, resources, shaders, Renderer2D/3D, shadows, IBL, post stack, async picking                                                  |
| [Assets](engine/assets.md)                      | AssetManager, handles, `.meta` sidecars and the asset scan, `AssetRef<T>`, async loading, hot reload, compiled outputs, mesh import (cgltf), and `.gmat` material assets |
| [Physics](engine/physics.md)                    | Jolt integration: PhysicsScene, body lifecycle, fixed timestep, interpolation, collision events, debug draw                                               |
| [Audio](engine/audio.md)                        | miniaudio integration: AudioEngine, voices, mixer groups, spatialization, streaming, the Audio asset type                                                 |
| [Scripting](engine/scripting.md)                | Lua 5.4 + sol2: ScriptEngine, LuaScriptSystem, the binding rules, error handling, sandboxing                                                              |
| [Game UI](engine/ui.md)                         | RmlUi: UIEngine, the bgfx render backend, RenderPass::UI, RCSS gotchas, the Debugger                                                                      |
| [Platform](engine/platform.md)                  | GLFW windows per OS, input, BgfxContext (bgfx lifetime), ImGui layer and its bgfx renderer                                                                |
| [Build & tooling](engine/build-and-tooling.md)  | premake workspace, dependencies, the shader toolchain (shaderc), profiling, compile-time tests                                                            |

### Editor

| Document                   | Covers                                                                                        |
| -------------------------- | --------------------------------------------------------------------------------------------- |
| [Editor](editor/editor.md) | EditorLayer, the viewport (picking, gizmos, drag-drop), play/stop, panels, keyboard shortcuts, undo/redo, prefab authoring |

### Runtime

| Document                      | Covers                                                                                          |
| ----------------------------- | ----------------------------------------------------------------------------------------------- |
| [Runtime](runtime/runtime.md) | GanymedRuntime: boot sequence, runtime.yaml config, backbuffer render mode, the assets snapshot |

### ToDo

| Document | Covers |
| --- | --- |
| [ToDo](ToDo/README.md) | **Everything not done yet** — milestone plans, known bugs, deferred follow-ups, verification gaps. Start here for what to work on next. |

### Historical records

`docs/history/` holds the working documents that drove the engine's big refactors. They are kept
because they record _why_ things are the way they are, including the verification evidence and the
alternatives that were rejected.

**They are historical and are not rewritten when the code moves on**, so some describe problems that
were fixed later. [`ToDo/README.md`](ToDo/README.md) lists the entries known to be stale. For what
the code does *now*, read the subsystem docs above; for what is still open, read `ToDo/`.

- [`3D_ROADMAP.md`](history/3D_ROADMAP.md) — the original 2D→3D plan (phases 0–8, essentially complete)
- [`ECS_VIEWS_IMPLEMENTATION_GUIDE.md`](history/ECS_VIEWS_IMPLEMENTATION_GUIDE.md) — the file-by-file plan for the view/access-wrapper ECS (complete)
- [`BGFX_MIGRATION.md`](history/BGFX_MIGRATION.md) — the OpenGL→bgfx migration log, including every bug found along the way (complete except Phase 7 multi-backend hardening — see [ToDo/rendering.md](ToDo/rendering.md))
- [`Scripting-And-UI-Integration.md`](history/Scripting-And-UI-Integration.md) — scripting + game UI (revised 2026-07-19 for the post-bgfx/post-ECS engine; complete — see [scripting.md](engine/scripting.md) and [ui.md](engine/ui.md))
- [`ANIMATION_ROADMAP.md`](history/ANIMATION_ROADMAP.md) — the skeletal animation milestone (phases 1–5 executed; complete — read the per-phase execution notes)
- [`RUNTIME_AUDIO_ROADMAP.md`](history/RUNTIME_AUDIO_ROADMAP.md) — the standalone runtime + audio milestone (GanymedRuntime app, miniaudio subsystem; **complete** — phases 1–5 executed, read the per-phase execution notes)
- [`CONTENT_AUTHORING_ROADMAP.md`](history/CONTENT_AUTHORING_ROADMAP.md) — the content authoring milestone (serialization hygiene, editor undo/redo, `.gmat` material assets, linked prefabs; complete — see [editor.md](editor/editor.md), [scene.md](engine/scene.md) and [assets.md](engine/assets.md))
- [`PARTICLE_ROADMAP.md`](history/PARTICLE_ROADMAP.md) — the particle system milestone (CPU-simulated emitters, billboard + instanced-mesh rendering, keyframed curves with an in-house curve editor; **phases 1–5 complete** — curve/RNG, deterministic CPU sim, billboard + mesh draws, curve editor + inspector preview, Lua + runtime spark demo)
- [`ASSET_PIPELINE_ROADMAP.md`](history/ASSET_PIPELINE_ROADMAP.md) — the asset pipeline + resource management milestone, drawn from BlankEngine's `AssetService`/`ResourceService` design (`.meta` sidecars, a typed manager registry with weak caches, `AssetRef<T>`, compiled outputs with epoch invalidation, async loading, hot reload; **complete** — all six phases are live in [assets.md](engine/assets.md); it deliberately excludes most of BlankEngine, so read the scope assessment and "Not doing" first)
- [`THREADING_ROADMAP.md`](history/THREADING_ROADMAP.md) — `Core/JobSystem` over enkiTS: a parallel-for and cancellable background tasks with a bgfx-submit-thread handoff, drawn from BlankEngine's `ThreadService` (**complete** — T1 the scheduler, T2 thread naming and profiler callbacks, T3 parallel BCn encode, T4 async loading; see [core.md](engine/core.md#job-system). plus decision 4, which put Jolt on it instead of a second pool — see [physics.md](engine/physics.md#the-job-system))
- [`REFLECTION_ROADMAP.md`](history/REFLECTION_ROADMAP.md) — member-level reflection on `entt::meta` for the generic inspector, generic serializer, and prefab per-property overrides (**done** — all 23 components registered and self-validating, 15 of 20 inspector sections and every component's serialization now driven from that registration, plus prefab per-property overrides; see [scene.md](engine/scene.md#member-reflection))

## Building & running

See the top-level [README](../README.md) for per-platform build steps. Two one-time steps matter on
a fresh clone, because compiled shader bytecode is gitignored:

```
scripts\build_shader_tools.bat   # builds bgfx's shaderc (once per machine)
scripts\compile_shaders.bat      # compiles assets/shaders/src -> compiled/<profile>/
```

Without them the app runs but draws nothing except the clear color and the UI. Re-run
`compile_shaders` after any shader edit — shaders are no longer compiled at runtime
(see [Rendering — shaders](engine/rendering.md#shaders)).
