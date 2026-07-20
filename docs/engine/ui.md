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

## Input routing

`UIEngine::OnEvent` translates engine events into `Context::Process*` calls. The editor forwards
events to it only while playing *and* while the viewport owns the pointer, then bails out early if
the event came back `Handled`.

Mouse positions arrive in window coordinates and RmlUi wants viewport-local pixels, so
`SetViewportOrigin` (fed from `m_ViewportBounds[0]`, the same origin picking subtracts) shifts
them. Like picking, there is **no Y flip** — both address from the viewport's top-left.

Key translation lives in
[`Platform/RmlUi/RmlUiInput.h`](../../GanymedEngine/source/Platform/RmlUi/RmlUiInput.h). It is
`RmlGLFW::ConvertKey` extracted verbatim from the reference backend and still switching on
`GLFW_KEY_*`, because the engine's `KeyCodes.h` *is* GLFW's numbering — keeping the original
constant names means it can be re-extracted on an RmlUi upgrade instead of being hand-translated
and quietly drifting. Modifiers are polled from `Input` rather than taken from a callback bitfield,
so the lock keys (`KM_CAPSLOCK`/`KM_NUMLOCK`/`KM_SCROLLLOCK`) are never reported; nothing depends
on them.

### What counts as "the UI took it"

**Do not use `Context::Process*`'s return value for pointer events.** A HUD document's `<body>`
fills the whole viewport, so RmlUi reports *every* mouse move as consumed — which swallows camera
orbit and gameplay clicks the moment any HUD is on screen. Verified: with the naive rule, a probe
over empty sky came back `handled=true`.

Pointer events instead consult the hover element, and are claimed only when it is not the document
root:

| Event | Claimed when |
|---|---|
| Mouse move / button / wheel | The hover element exists and is not the document root |
| Key down/up, text input | `Context::Process*` says it was consumed — i.e. something has **focus** |

The asymmetry is deliberate: pointer events belong to whatever is under the cursor, keyboard events
to whatever has focus. With nothing focused, editor shortcuts and gameplay keys keep working while
a HUD is up.

## Data binding

`UIEngine::Init` creates a data model named `hud` bound to a `HudData { float Health; int Score; }`
living in the engine data block. **The model must exist before any document declaring
`data-model="hud"` loads** — RmlUi resolves bindings at parse time, and a document that arrives
first renders its `{{expressions}}` as literal text.

Setting a value is not enough on its own; RmlUi only re-evaluates expressions once the variable is
marked dirty, which `SetHudHealth`/`SetHudScore` do.

Scripts drive it through the `UI` table — `UI.SetHealth(n)`, `UI.SetScore(n)`, plus getters. The
example `Player.ts` drains health and ticks score, and the bar's width follows via
`data-style-width="health + '%'"` without the script knowing anything about RML or RCSS.

> Fixed setters rather than a general `UI.Set(name, value)`: RmlUi data models bind to real C++
> addresses declared up front, so an arbitrary property bag needs a different mechanism entirely
> (`BindFunc`, or a bound map type). Worth doing when a second HUD needs it — not before.

## Sharing one Lua VM has a cost

`Rml::Lua::Initialise` installs globals of its own, and one of them is **`Log`** — a usertype whose
interface is `Log.Message(Log.logtype.info, ...)`. Because the plugin loads *after*
`ScriptEngine::Init`, it silently replaced the engine's `Log` table, and every script calling
`Log.Info` died with "attempt to call a nil value (field 'Info')".

`UIEngine::Init` therefore calls `ScriptEngine::ReinstallGlobals()` straight after loading the
plugin, so engine names win. The plugin's `rmlui` global (documents, elements, event listeners) is
untouched and still available to UI scripts.

Worth remembering when adding either a binding or an RmlUi version bump: the two namespaces are
genuinely shared, and collisions are silent.

## Not done yet

UI logic written in TypeScript against the `rmlui` global, and making the HUD document a scene
property rather than a hard-coded path.
