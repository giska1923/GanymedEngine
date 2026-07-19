# Platform Layer

`GanymedEngine/source/Platform/` — everything OS- or backend-specific: windowing, input, native
file dialogs, and the bgfx/ImGui glue. The engine proper sees only the abstractions in
`GanymedE/Core/` (`Window`, `Input`, `FileDialogs`).

## Windows per OS

`WindowsWindow` / `LinuxWindow` / `macOSWindow` are near-identical GLFW implementations of
[`Window`](../../GanymedEngine/source/GanymedE/Core/Window.h); `Window::Create` picks the one for
the compiled platform. The important choices (shown in
[`WindowsWindow.cpp`](../../GanymedEngine/source/Platform/Windows/WindowsWindow.cpp), mirrored on
the other platforms):

- **`GLFW_CLIENT_API = GLFW_NO_API`** — bgfx creates and owns the graphics device itself
  (including the GL context if the GL backend is picked). GLFW is windowing + input only.
- Every GLFW callback translates to an engine event and forwards through the
  `WindowData::EventCallback` (bound to `Application::OnEvent`). Key repeat becomes
  `KeyPressedEvent(repeat=1)`; there is also `KeyTypedEvent` for text input.
- **Resize is applied in `OnUpdate`, not in the GLFW callback** — the callback only updates the
  cached size (and emits the event); `BgfxContext::Resize` runs at the frame boundary so
  `bgfx::reset` never lands mid-frame.
- `OnUpdate` = `glfwPollEvents()` + `BgfxContext::Frame()`.
- VSync forwards to the context (a `bgfx::reset` flag, not `glfwSwapInterval`).
- Shutdown order matters: the context (i.e. bgfx) is destroyed **before** `glfwDestroyWindow`,
  because bgfx holds the native window handle.

## BgfxContext

[`BgfxContext`](../../GanymedEngine/source/Platform/Bgfx/BgfxContext.h) owns bgfx's lifetime and
the backbuffer swapchain:

- `Init` calls `bgfx::renderFrame()` **before** `bgfx::init` — the documented trick that puts bgfx
  in single-threaded mode (the calling thread becomes the render thread). Backend is auto-picked
  (`RendererType::Count`); the native window/display handles come from `glfwGetWin32Window` /
  `glfwGetX11Window`+`Display` / `glfwGetCocoaWindow`.
- After init it logs the renderer name and the two caps that bite across backends
  (`homogeneousDepth`, `originBottomLeft`), and **logs an error if the backend wants [-1,1] clip
  depth** — the workspace compiles glm with `GLM_FORCE_DEPTH_ZERO_TO_ONE`, which cannot adapt at
  runtime (see the premake comment and BGFX_MIGRATION §9.3).
- `Frame()` touches the backbuffer view (a view with no draws is skipped *including its clear*),
  calls `bgfx::frame()`, and reports the returned frame number to `Renderer::OnFrameSubmitted`
  (async readback polls against it).
- The destructor lowers `Renderer::SetGpuAlive(false)` **before** `bgfx::shutdown()` — the flag
  every GPU-resource destructor checks so statics outliving `main()` don't call into dead bgfx.
- `Resize`/`SetVSync` funnel into one `Reset()` (`bgfx::reset` + backbuffer view rect).

Deliberately **not** a virtual `GraphicsContext`: with bgfx there is exactly one backend
implementation, so the old interface (and `OpenGLContext`) was deleted with it.

## Input & file dialogs

- `Platform/<OS>/<OS>Input.cpp` implements the static
  [`Input`](../../GanymedEngine/source/GanymedE/Core/Input.h) API over
  `glfwGetKey`/`glfwGetMouseButton`/`glfwGetCursorPos` against the application's window.
- `Platform/<OS>/<OS>PlatformUtils.cpp` implements
  [`FileDialogs::OpenFile/SaveFile`](../../GanymedEngine/source/GanymedE/Utils/PlatformUtils.h)
  (Win32 common dialogs on Windows; zenity/osascript-style equivalents elsewhere). Filter strings
  use the Win32 double-NUL format: `"GanymedE Scene (*.ganymede)\0*.ganymede\0"`.

## ImGui

Two halves:

- [`ImGuiLayer`](../../GanymedEngine/source/GanymedE/ImGui/ImGuiLayer.h) (engine, an overlay
  pushed by `Application`) owns the ImGui context: docking enabled, dark theme
  (`SetDarkThemeColors`), font loading with a graceful fallback to the built-in font when
  `assets/fonts` is missing (a hard assert killed Sandbox once). `Begin()`/`End()` bracket each
  frame's UI; `OnEvent` marks events handled when ImGui wants the mouse/keyboard **unless**
  `BlockEvents(false)` — which the editor sets while the viewport is hovered/focused so camera and
  gizmo input reach the layers beneath. Platform half is stock `ImGui_ImplGlfw`, initialized with
  `InitForOther` (there is no GL context to assume).
- [`ImGuiRendererBgfx`](../../GanymedEngine/source/Platform/Bgfx/ImGuiRendererBgfx.h) (the render
  half, replacing `ImGui_ImplOpenGL3`): draw lists go into transient vertex/index buffers, one
  submit per `ImDrawCmd` with scissor, all on `RenderPass::ImGui` (view 200, `Sequential` mode so
  ImGui's own draw order is preserved) targeting the backbuffer. The ortho projection is built by
  hand from `caps->homogeneousDepth` (the compile-time glm choice can't adapt per backend). Uses
  its own `varying.ImGui.def.sc` — `ImDrawVert` is vec2 pos + vec2 uv + packed u8 color, unlike
  the engine's layouts. Multi-viewport is disabled (would need one bgfx framebuffer per OS
  window).

`ImTextureID` is the bgfx texture handle index (`Texture2D::GetRendererID()` /
`Framebuffer::GetColorAttachmentRendererID()`); the viewport image chooses its UV orientation from
`caps->originBottomLeft` because a *render target's* origin is backend-dependent, while plain
loaded textures are always top-left (see the comments at those call sites).
