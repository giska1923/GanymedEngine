# GanymedEngine

GanymedEngine is a C++17 game engine with a bgfx-based renderer (D3D11/D3D12/Vulkan/Metal/OpenGL),
an entt-based ECS with declared-access views and reactive change tracking, Jolt physics, a glTF
asset pipeline, and an ImGui/ImGuizmo editor. It builds on Windows, Linux and macOS via premake5.

Documentation for the engine, the editor and the standalone runtime lives in
[docs/](docs/README.md). The runtime (`GanymedRuntime`) boots a scene straight into play mode with
no editor chrome — see [docs/runtime/runtime.md](docs/runtime/runtime.md).

# Linux (prerequisites)

sudo apt install libglfw3-dev libwayland-dev libxkbcommon-dev xorg-dev

# Build & Run

Every platform starts with the same script, which needs Python 3.8+ and git. It downloads premake,
checks out the submodules, builds bgfx's shader compiler, compiles the shaders and generates the
project files. It skips any step that is already up to date, so it is safe to re-run after every
pull:

- `python scripts/setup.py` opens an interactive menu that shows each step's state and can force
  any of them
- `python scripts/setup.py auto` runs it unattended

See [docs/engine/build-and-tooling.md](docs/engine/build-and-tooling.md#workspace) for what each
step does, and for the escape hatches on hosts where a prebuilt tool will not run.

## Windows

- run `python scripts/setup.py` to generate GanymedEngine.sln
- open ./GanymedEngine.sln, build solution and run

## Linux

- run `python3 scripts/setup.py` to generate GanymedE projects
- run command: make -j$(nproc) config=debug in root to build everything
- run command: bin/Debug-linux-x86_64/GanymedEditor/GanymedEditor from the repository root to run Editor
- run command: bin/Debug-linux-x86_64/GanymedRuntime/GanymedRuntime from the repository root to run the game runtime

## macOS

- run `python3 scripts/setup.py` to generate GanymedE projects
- run command: xcodebuild -workspace GanymedEngine.xcworkspace -scheme GanymedEditor -configuration Debug build to build Editor
- run command: bin/Debug-macosx-x86_64/GanymedEditor/GanymedEditor from the repository root to run Editor
- run command: bin/Debug-macosx-x86_64/GanymedRuntime/GanymedRuntime from the repository root to run the game runtime
