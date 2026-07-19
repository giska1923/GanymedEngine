# GanymedEngine Documentation

GanymedEngine is a C++17 game engine with a bgfx-based renderer (D3D11/D3D12/Vulkan/Metal/OpenGL),
an entt-based ECS with declared-access views and reactive change tracking, Jolt physics, a glTF
asset pipeline, and an ImGui/ImGuizmo editor. It builds on Windows, Linux and macOS via premake5.

The repository contains three applications:

| Project | What it is |
|---|---|
| `GanymedEngine` | The engine static library. All code under `GanymedEngine/source/`. |
| `GanymedEditor` | The editor application (scene editing, play mode, content browser). |
| `Sandbox` | A minimal test app (not covered by these docs). |

## Documentation map

### Engine

| Document | Covers |
|---|---|
| [Architecture overview](engine/architecture.md) | The big picture: module layout, frame flow, ownership, design principles |
| [Core](engine/core.md) | Application, entry point, layers, events, input, window abstraction, logging, UUID, assert/profiling macros |
| [ECS](engine/ecs.md) | The view/access-wrapper ECS on top of entt: wrappers, accessors, change tracking, reactive views, systems, command queue, singletons, scheduling metadata |
| [Scene](engine/scene.md) | Scene, Entity, the component catalog, hierarchy, built-in systems, scene singletons, serialization, play-mode copy |
| [Rendering](engine/rendering.md) | The bgfx backend: view model, resources, shaders, Renderer2D/3D, shadows, IBL, post stack, async picking |
| [Assets](engine/assets.md) | AssetManager, the registry, handles, mesh import (cgltf) and the binary mesh cache |
| [Physics](engine/physics.md) | Jolt integration: PhysicsScene, body lifecycle, fixed timestep, interpolation, collision events, debug draw |
| [Platform](engine/platform.md) | GLFW windows per OS, input, BgfxContext (bgfx lifetime), ImGui layer and its bgfx renderer |
| [Build & tooling](engine/build-and-tooling.md) | premake workspace, dependencies, the shader toolchain (shaderc), profiling, compile-time tests |

### Editor

| Document | Covers |
|---|---|
| [Editor](editor/editor.md) | EditorLayer, the viewport (picking, gizmos, drag-drop), play/stop, panels, keyboard shortcuts |

### Historical / planning documents

`docs/toDo&done/` holds the working documents that drove the engine's three big refactors. They are
kept because they record *why* things are the way they are, including verification evidence:

- [`3D_ROADMAP.md`](toDo&done/3D_ROADMAP.md) — the original 2D→3D plan (phases 0–8, essentially complete)
- [`ECS_VIEWS_IMPLEMENTATION_GUIDE.md`](toDo&done/ECS_VIEWS_IMPLEMENTATION_GUIDE.md) — the file-by-file plan for the view/access-wrapper ECS (complete)
- [`BGFX_MIGRATION.md`](toDo&done/BGFX_MIGRATION.md) — the OpenGL→bgfx migration log, including every bug found along the way (complete except some Phase 7 hardening)
- [`Scripting-And-UI-Integration.md`](toDo&done/Scripting-And-UI-Integration.md) — the next planned phase (not yet implemented)

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
