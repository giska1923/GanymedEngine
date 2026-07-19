# GanymedEngine

GanymedEngine is C++ game engine.

Documentation for the engine and the editor lives in [docs/](docs/README.md).

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

## macOS

- run ./scripts/setup_dependencies.sh
- run ./scripts/macOS_GenerateProjects.sh to generate GanymedE projects
- run command: xcodebuild -workspace GanymedEngine.xcworkspace -scheme GanymedEditor -configuration Debug build to build Editor
- run command: cd GanymedEditor && ../bin/Debug-macosx-x86_64/GanymedEditor/GanymedEditor to run Editor
