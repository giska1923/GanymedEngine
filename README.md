# GanymedEngine

GanymedEngine is C++ game engine.

Documentation for the engine, the editor and the standalone runtime lives in
[docs/](docs/README.md). The runtime (`GanymedRuntime`) boots a scene straight into play mode with
no editor chrome — see [docs/runtime/runtime.md](docs/runtime/runtime.md).

# Linux (prerequisites)

sudo apt install libglfw3-dev libwayland-dev libxkbcommon-dev xorg-dev

# Build & Run

## Windows

- run ./scripts/Win_GenerateProjects.bat to generate .sln
- open ./GanymedEngine.sln, build solution and run

## Linux

- run ./scripts/setup_dependencies.sh
- run ./scripts/Linux_GenerateProjects.sh to generate GanymedE projects
- run command: make -j$(nproc) config=debug in root to build everything
- run command: cd GanymedEditor && ../bin/Debug-linux-x86_64/GanymedEditor/GanymedEditor to run Editor
- run command: cd GanymedRuntime && ../bin/Debug-linux-x86_64/GanymedRuntime/GanymedRuntime to run the game runtime

## macOS

- run ./scripts/setup_dependencies.sh
- run ./scripts/macOS_GenerateProjects.sh to generate GanymedE projects
- run command: xcodebuild -workspace GanymedEngine.xcworkspace -scheme GanymedEditor -configuration Debug build to build Editor
- run command: cd GanymedEditor && ../bin/Debug-macosx-x86_64/GanymedEditor/GanymedEditor to run Editor
- run command: cd GanymedRuntime && ../bin/Debug-macosx-x86_64/GanymedRuntime/GanymedRuntime to run the game runtime
