# Build System & Tooling

## Workspace

[`premake5.lua`](../../premake5.lua) (workspace) + per-project `premake5.lua` files. Generate with:

- Windows: `scripts/Win_GenerateProjects.bat` → `GanymedEngine.sln` (VS2022), build in the IDE.
- Linux: `scripts/setup_dependencies.sh`, `scripts/Linux_GenerateProjects.sh`, then
  `make -j$(nproc) config=debug`.
- macOS: `scripts/macOS_GenerateProjects.sh` → Xcode workspace.

Projects: `GanymedEngine` (static lib, C++17, PCH `gepch.h`), `GanymedEditor` and `Sandbox`
(console apps linking the engine), plus the dependency group built from source via their own
premake scripts in `GanymedEngine/extern/*.lua`: GLFW, ImGui (+ImGuizmo), yaml-cpp, Jolt,
bx/bimg/bgfx, Lua. Configurations: `Debug` (`GE_DEBUG` → asserts, Jolt debug renderer), `Release`,
`Dist` (no Jolt debug renderer). Output goes to `bin/<config>-<os>-<arch>/<project>/`,
intermediates to `temp/`.

Workspace-wide define worth knowing: **`GLM_FORCE_DEPTH_ZERO_TO_ONE`** — bgfx normalizes clip
space to [0,1] on D3D/Vulkan/Metal; glm defaults to GL's [-1,1]. It is set at workspace scope on
purpose: glm is header-only, and a project disagreeing would silently change the layout of shared
glm types across the static-library boundary. `BgfxContext` asserts the live backend agrees.

Other build facts that have bitten before (details in
[`BGFX_MIGRATION.md`](../toDo&done/BGFX_MIGRATION.md) Phase 0):

- bx requires **C++20**; the three bgfx projects build at C++20 while the engine stays C++17 —
  safe because `<bgfx/bgfx.h>` includes no bx headers.
- MSVC needs `/Zc:preprocessor` for bx, and bgfx builds with exceptions off (`__try` in
  `thread.cpp`); `_HAS_EXCEPTIONS=0` is deliberately **not** defined (it would change STL layout
  across the lib boundary).
- `bimg_decode` is not built (it now drags in dav1d/libavif); images load through stb_image.
- The `JPH_*` instruction-set defines in the engine's premake **must match `Jolt.lua`**, or Jolt
  types change layout across the boundary.
- AVX2 is assumed (`/arch:AVX2`, `-mavx2 …`).
- The `lua/lua` submodule is the **raw source mirror**, which does not ship `lua.hpp` — that
  header only exists in the packaged release tarballs, and sol2 includes it unconditionally.
  `extern/lua_cxx/lua.hpp` supplies it, and `IncludeDir.lua_cxx` must be on the include path
  alongside `IncludeDir.lua`, never instead of it. It sits outside the submodule for the same
  reason the build scripts do.
- The engine defines **`SOL_ALL_SAFETIES_ON=1`**: bounds and type checks on every sol2 call, so a
  script bug surfaces as a logged Lua error instead of a crash across the C++ boundary.
- Lua is pinned to the newest **5.4.x** (5.4.8) rather than 5.5, because sol2 does not support 5.5
  and TypeScriptToLua's highest `luaTarget` is 5.4.

## Dependencies (vendored under `GanymedEngine/extern/`)

| Library | Used for |
|---|---|
| bgfx / bimg / bx | Rendering backend |
| GLFW | Windowing + input (no graphics API — `GLFW_NO_API`) |
| entt 3.16 | ECS registry the view layer wraps |
| glm | Math (with `GLM_FORCE_DEPTH_ZERO_TO_ONE`) |
| Jolt | Physics |
| ImGui + ImGuizmo | Editor UI + transform gizmo |
| yaml-cpp | Scene + asset-registry serialization |
| cgltf | glTF import (header-only) |
| stb_image | Image loading (header-only) |
| spdlog | Logging (header-only) |
| Lua 5.4.8 | Gameplay scripting VM (built as a C static lib) |
| sol2 3.5.0 | C++ binding layer over Lua (header-only) |
| RmlUi 6.2 | Game UI (HTML/CSS-style documents); Core + Lua plugin only |
| FreeType 2.14.3 | RmlUi's font engine (its one hard dependency) |

Build scripts for submodule-shaped deps live *outside* the submodule trees (`extern/GLFW.lua`,
`extern/Jolt.lua`, `extern/bgfx.lua`, `extern/Lua.lua`, `extern/RmlUi.lua`, `extern/FreeType.lua`).

Two defines these hand-written scripts must supply that CMake would have set for you, both of
which fail at *runtime* rather than at build time if missed:

- **`RMLUI_FONT_ENGINE_FREETYPE`** on the RmlUi project — CMake derives it from its
  `RMLUI_FONT_ENGINE` option (default `freetype`). Without it everything links and
  `Rml::Initialise()` fails with "No font engine interface set!".
- **`RMLUI_STATIC_LIB`** on the RmlUi project *and* every consumer, like the Jolt defines. A
  mismatch decorates RmlUi's API with `__declspec(dllimport)` on one side and the link fails.

FreeType builds from the canonical minimal file list — one `.c` per module, each of which
`#include`s the rest of its module. Adding the individual sources instead multiply-defines half of
them.

Those scripts must also keep their **output** outside the submodule trees — every one of them
uses `%{wks.location}/bin` and `%{wks.location}/temp`, same as the first-party projects. A parent
repo never applies its own `.gitignore` inside a nested repo; it only tracks the submodule's
commit SHA. So build artifacts written under `extern/<dep>/` show up as untracked files *in that
submodule*, which reports the submodule as dirty in `git status` and in GUI clients, and the root
`.gitignore` cannot suppress it.

## Shader toolchain

Shaders are **compiled offline**; the compiled `.bin` files are gitignored. On a fresh clone:

```
scripts\build_shader_tools.bat    # builds bgfx's shaderc via its GENie build (once per machine)
                                  # → staged at scripts/tools/<os>/shaderc
scripts\compile_shaders.bat       # every .sc in assets/shaders/src → dx11 / spirv / glsl profiles
                                  # → GanymedEditor/assets/shaders/compiled/<profile>/ and Sandbox's copy
```

(`.sh` twins exist for Linux/macOS.) The profile-folder ↔ backend mapping must match
`ProfileDirectory()` in [`Shader.cpp`](../../GanymedEngine/source/GanymedE/Renderer/Shader.cpp).
`compile_shaders` prefers a per-shader `varying.<Name>.def.sc` over the shared `varying.def.sc`
when present (ImGui needs this). There is no file watcher: **edit a shader → re-run the script →
restart the app** (a failed/missing program logs and skips its draws rather than crashing).

It is deliberately not a premake prebuild step — that would hard-fail builds on machines that
haven't built shaderc yet.

## Script toolchain (optional)

Gameplay scripts may be authored in TypeScript and compiled to Lua by
[TypeScriptToLua](https://typescripttolua.github.io/). Entirely optional — the engine loads `.lua`,
and hand-written Lua is a first-class path.

```
cd GanymedEditor/scripts-src
npm install        # once; needs Node + npm, nothing else in the C++ build depends on it
npm run watch      # recompiles into ../assets/scripts on every save
```

Unlike shader bytecode, the emitted `assets/scripts/*.lua` **is tracked in git** — the folder also
holds hand-written scripts, so it cannot be ignored wholesale. `scripts-src/node_modules/` is
ignored; `package.json`, `package-lock.json` and `tsconfig.json` need explicit `!` negations in
`.gitignore` because a blanket `*.json` rule would otherwise swallow them.

Pinning note: `typescript-to-lua` declares an **exact** `typescript` peer version (1.37.1 ↔ 6.0.2).
Take the pair the lockfile records rather than upgrading TypeScript on its own. Config rationale
(`luaLibImport`, `noImplicitSelf`, why `sourceMapTraceback` is off) is in
[scripting.md](scripting.md#typescript-authoring-typescripttolua).

## Assets

Each app resolves `assets/` **relative to its working directory** — run the editor from
`GanymedEditor/`. `GanymedEditor/assets/` holds shaders (`src/` + gitignored `compiled/`),
environments, models, scenes, textures, fonts, and the asset registry (`AssetRegistry.gr`).
`assets/.assets/` is the binary mesh cache (safe to delete; also gitignored from the browser's
perspective — the content browser hides it).

## Profiling & debug tooling

- **Instrumentor** (`GE_PROFILE_*` macros) → chrome://tracing JSON, three sessions per run
  (Startup/Runtime/Shutdown). Enable with `GE_PROFILE`.
- **F1** in any app toggles bgfx's stats/debug-text overlay (draw counts, GPU/CPU timings).
- The editor Stats panel shows Renderer2D/3D counters (draws, quads, meshes, frustum-culled,
  instanced, transparent) plus live post-processing and physics-debug toggles.
- bgfx leak reporting on shutdown will name leaked handles — a leak there usually means a resource
  outlived `Renderer::Shutdown` (see the `IsGpuAlive` discussion in
  [rendering.md](rendering.md#renderer-the-umbrella)).

## Compile-time tests

The ECS ships its invariants as `static_assert` files that compile with the engine and emit no
code: [`ViewsTests.cpp`](../../GanymedEngine/source/GanymedE/ECS/ViewsTests.cpp),
[`AccessWrappersTests.cpp`](../../GanymedEngine/source/GanymedE/ECS/AccessWrappersTests.cpp),
[`ComponentAccessorTests.cpp`](../../GanymedEngine/source/GanymedE/ECS/ComponentAccessorTests.cpp).
They pin the slot rules (a tracked+writeable slot is never `T&`, filters produce no slots,
normalization order, …) — the mistakes they guard against would otherwise fail *silently* at
runtime as unlogged writes. If a wrapper-grammar change breaks one of these, the test is telling
you the change breaks a load-bearing rule; fix the change, not the test.

There is no runtime test suite; the verification culture here is measured captures (pixel
percentages, byte-identical frame diffs) recorded in the migration doc.
