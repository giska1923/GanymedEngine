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
- `OnUpdate` = `glfwPollEvents()` + `BgfxContext::Frame()`. Custom title bars on Linux/macOS
  also run the manual caption drag here, after poll.
- VSync forwards to the context (a `bgfx::reset` flag, not `glfwSwapInterval`).
- Shutdown order matters: the context (i.e. bgfx) is destroyed **before** `glfwDestroyWindow`,
  because bgfx holds the native window handle.

### Borderless fullscreen

`WindowProps::Fullscreen` (fed from `ApplicationSpecification::Fullscreen`) creates an
**undecorated window sized to the primary monitor's video mode and positioned at its origin** — not
an exclusive-mode swapchain. `Width`/`Height` are ignored; the resolved size is written back into
`WindowData` before `BgfxContext::Init`, so bgfx and `UIEngine::Init` both see the real size, and it
is logged (`Borderless fullscreen: WxH at (x, y)`). If `glfwGetVideoMode` returns nothing the
request degrades to windowed with a warning.

Passing the monitor to `glfwCreateWindow` would request a real mode change, which bgfx does not
drive and which costs alt-tab friendliness for nothing at this scale. Exclusive fullscreen is out of
scope (see the roadmap's "not doing" list). Verified on Windows; Linux and macOS carry the same code
best-effort — an undecorated window is a hint a window manager may override on X11/Wayland, and on
macOS it sits under the menu bar rather than over it.

### Custom title bar

`WindowProps::CustomTitleBar` (from `ApplicationSpecification::CustomTitleBar`, default **false**)
creates an undecorated GLFW window so the editor can draw its own 40 px title bar.
GanymedRuntime leaves the flag off and keeps a normal OS frame.

Two platform paths, behind the same `Window` methods (`HasCustomTitleBar`, `SetTitleBarHitTest`,
`Minimize`, `ToggleMaximize`, `IsMaximized`):

- **Windows** subclasses the GLFW HWND (`SetWindowLongPtr(GWLP_WNDPROC)`). `WM_NCCALCSIZE`
  makes the client area cover the frame; when maximized it is clamped to the monitor work area
  (plus a 1 px top inset if the taskbar auto-hides, so Windows does not treat the window as
  fullscreen). `WM_NCHITTEST` returns `HTCAPTION` over the title strip except where the editor
  reported an interactive rect (Menu, min/max/close), and `HTLEFT`/`HTTOPLEFT`/… in a 6 DIP
  border. The OS then provides drag, Aero Snap, Win+Arrow, edge resize, double-click-maximize,
  and the DWM drop shadow (`DwmExtendFrameIntoClientArea`). Win11 rounded corners are turned
  off (`DWMWCP_DONOTROUND`) to match the rest of the square chrome.
- **Linux / macOS** use a manual `glfwSetWindowPos` drag from the same hit-test rects, plus
  double-click to maximize. That path does **not** get compositor snap or a native shadow.
- **Wayland** cannot move an undecorated window (`glfwSetWindowPos` is a no-op). If the flag is
  set, `LinuxWindow::Init` logs a warning, leaves `GLFW_DECORATED` on, and
  `HasCustomTitleBar()` returns false so the editor keeps the ImGui menu bar rather than
  shipping a window that cannot be moved.

`HasCustomTitleBar()` is the *realized* flag, not the spec request. Hit-test rects are client
pixels, reported every frame from the title bar after it draws; `WM_NCHITTEST` cannot ask ImGui,
so the exclusions are geometric rather than `IsAnyItemHovered()`.

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
  (async readback polls against it). `Environment::Bake` may also call `bgfx::frame()` between
  IBL stages on Intel + Vulkan; it reports those the same way so picking's frame counter does
  not skip. See [rendering.md](../engine/rendering.md#environment--ibl).
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
  pushed by `Application` **when `ApplicationSpecification::EnableImGui` is set** — a non-editor
  front-end runs with no ImGui at all, so nothing here initializes and
  `Application::GetImGuiLayer()` is null) owns the ImGui context: docking enabled,
  `StyleColorsDark()`, **built-in font only**. The engine does not own a brand palette.
  Editor fonts (Inter + Lucide) and `EditorTheme` are applied from `EditorLayer::OnAttach` after
  this layer attaches; the engine must not hold `ImFont*` values the editor then invalidates with
  `io.Fonts->Clear()`. `Begin()`/`End()` bracket each frame's UI;
  `OnEvent` marks events handled when ImGui wants the mouse/keyboard **unless**
  `BlockEvents(false)` — which the editor sets while the viewport is hovered/focused so camera and
  gizmo input reach the layers beneath. Platform half is stock `ImGui_ImplGlfw`, initialized with
  `InitForOther` (there is no GL context to assume). The atlas is rasterized by FreeType
  (`IMGUI_ENABLE_FREETYPE`, `imgui_freetype.cpp` compiled into the ImGui static lib); see
  [build-and-tooling.md](build-and-tooling.md).
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
