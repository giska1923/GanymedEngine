# Game UI (RmlUi)

HTML/CSS-style game UI on **RmlUi 6.2**, rendered through a custom bgfx backend. This is the
*game's* UI — the editor's own UI is ImGui and stays that way.

Plan of record: [`Scripting-And-UI-Integration.md`](../toDo&done/Scripting-And-UI-Integration.md).

## Layout

| Piece | File | Owns |
|---|---|---|
| `UIEngine` | [`UI/UIEngine.h`](../../GanymedEngine/source/GanymedE/UI/UIEngine.h) | The `Rml::Context`, documents, lifecycle |
| Render backend | [`Platform/RmlUi/RmlUiRendererBgfx.h`](../../GanymedEngine/source/Platform/RmlUi/RmlUiRendererBgfx.h) | `Rml::RenderInterface` on bgfx |
| System interface | [`Platform/RmlUi/RmlUiSystemInterface.h`](../../GanymedEngine/source/Platform/RmlUi/RmlUiSystemInterface.h) | Clock + log routing |
| Shaders | `assets/shaders/src/{vs,fs}_RmlUi.sc` + `varying.RmlUi.def.sc` | One program, RmlUi's vertex layout |
| Example | `GanymedEditor/assets/ui/hud.{rml,rcss}` | A static HUD |

`UIEngine.h` carries no RmlUi types beyond forward declarations, the same discipline
`ScriptEngine.h` applies to sol2 — which is why the editor project has no RmlUi include path at
all. Anything the editor needs goes through `UIEngine`.

## Where it lands in the frame

`RenderPass::UI = 28`, and that single constant is the whole ordering story. bgfx executes views in
ID order, so sitting after `Composite` (26) means the UI draws onto the tonemapped, anti-aliased
image in **display space** and is never itself tonemapped — regardless of where in the frame the
submit calls happen. It also appears inside the editor's viewport image for free, because that
image *is* the composite attachment.

The editor points the UI at `SceneRenderer::GetCompositeFramebuffer()`. A shipped game passes
`nullptr`, meaning the backbuffer.

> `SceneRenderer::SetViewportSize` rebuilds the post-stack targets, so the composite framebuffer is
> a *different object* afterwards. The editor re-calls `UIEngine::SetTarget` on every resize; miss
> that and the UI composites into a destroyed framebuffer.

## Render backend notes

Modeled on [`ImGuiRendererBgfx`](../../GanymedEngine/source/Platform/Bgfx/ImGuiRendererBgfx.cpp),
with three differences that come from RmlUi rather than from taste:

- **Geometry is compiled, not transient.** RmlUi hands geometry over once and re-renders it for as
  long as the element lives — that is the point of the compiled-geometry model in 6.x. So these are
  static vertex/index buffers with an explicit release, not per-frame transients like ImGui's.
- **Premultiplied alpha.** `Rml::Vertex` colours *and* RmlUi's textures are premultiplied, so the
  draw state is `BLEND_FUNC(ONE, INV_SRC_ALPHA)`, not the usual `(SRC_ALPHA, INV_SRC_ALPHA)`.
  Ordinary alpha blending double-applies alpha and shows up as dark fringing on every glyph edge.
  `LoadTexture` premultiplies what stb_image decodes; `GenerateTexture` must not, because RmlUi
  has already done it.
- **32-bit indices.** RmlUi hands out `int` indices, so the index buffer needs
  `BGFX_BUFFER_INDEX32`. The bgfx default is 16-bit and would silently halve every index.

Shared with the ImGui backend: a dedicated view in **`Sequential`** mode (UI paints back-to-front;
bgfx must not reorder), an ortho matrix built by hand from `bgfx::getCaps()->homogeneousDepth`
(the compile-time `GLM_FORCE_DEPTH_ZERO_TO_ONE` cannot adapt per backend), and an **explicit view
id on every `bgfx::submit`** — never `RenderCommand::SetViewId`, whose sticky current-view state
would then need restoring.

The vertex layout must match `Rml::Vertex` field-for-field: `vec2 position`, RGBA8 colour, `vec2
tex_coord` — note the colour/texcoord order is the reverse of ImGui's, which is why RmlUi needs its
own `varying.RmlUi.def.sc`. `Init` asserts `getStride() == sizeof(Rml::Vertex)` so a future layout
edit fails loudly instead of reinterpreting the data.

## Two things RmlUi does not give you

Both cost real debugging time; neither is obvious from the docs.

**1. The FreeType font engine is compile-gated.** `RMLUI_FONT_ENGINE_FREETYPE` must be defined when
building `Source/Core`. CMake sets it from its `RMLUI_FONT_ENGINE` option (default `freetype`), so
a hand-written build script has to supply it. Without it everything compiles and links, and
`Rml::Initialise()` fails at runtime with `No font engine interface set!`.

**2. There is no user-agent stylesheet.** Every element starts as `display: inline` — including
`div`. An inline element ignores `width` and `height`, so a styled box lays out at zero width,
renders nothing, and logs nothing. RmlUi ships the defaults as an *asset*, not as library
behaviour: `Samples/assets/rml.rcss`, copied here to
`GanymedEditor/assets/ui/rml.rcss`. **Every document must `<link>` it before its own stylesheet.**

Related: RCSS is not CSS. `@font-face` is rejected outright (faces are registered in C++ via
`Rml::LoadFontFace`), and on a bad at-rule the parser recovers by skipping to the next `}` — which
silently swallows the rule that follows. Watch the log for `Invalid at-rule identifier`.

## Lifecycle

```
Renderer::Init -> ScriptEngine::Init -> UIEngine::Init
   ... UIEngine::Shutdown -> ScriptEngine::Shutdown -> Renderer::Shutdown
```

Both ends are load-bearing. `UIEngine::Init` needs live bgfx (the font engine builds atlases
through the render interface the moment `Rml::Initialise` runs) and needs `ScriptEngine`'s
`lua_State`, which it shares with the RmlUi Lua plugin so UI and gameplay scripts see the same
globals. `UIEngine::Shutdown` must run *before* `ScriptEngine::Shutdown` (the plugin holds
references into that state) and *while* `Renderer::IsGpuAlive()` (`Rml::Shutdown` releases
textures). All three live in the `Application` constructor/destructor.

Per frame in Play state: `Scene::OnUpdateRuntime` (scripts set values) → `UIEngine::OnUpdate`
(layout/animation) → `UIEngine::OnRender`. The update order matters; the render order does not,
because the view ID decides when it draws.

## Editor integration

Documents load on **Play** and close on **Stop**. The path is hard-coded to `assets/ui/hud.rml` for
now — making it a scene property wants a UI-document asset type to hang off.

RmlUi's own inspector (element tree, computed RCSS, event log) toggles from **View → Game UI
Debugger** or **Ctrl+U**. Debug builds only; the Debugger sources are not compiled in Release/Dist.
When a document renders nothing, that inspector is usually the fastest way to find out why —
`display: inline` and a zero-width box look identical to "not loaded" from the outside.

## Not done yet

Input routing (mouse/keyboard into `Context::Process*`, with viewport-relative coordinate
translation and `Handled` propagation) and C++ ↔ UI data bindings are milestone 6. Until then the
HUD is static and does not respond to the mouse.
