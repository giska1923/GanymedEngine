#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Timestep.h"

#include <filesystem>

namespace Rml { class Context; class ElementDocument; }

namespace GanymedE {

	class Event;
	class Framebuffer;

	// Game UI (RmlUi), the counterpart to ScriptEngine on the UI side: a global
	// owning the one Rml::Context, its documents, and the bgfx render backend.
	//
	// This is the GAME's UI, not the editor's - the editor is ImGui and stays that
	// way. RmlUi renders into RenderPass::UI, which sorts after the post stack, so
	// it lands on the tonemapped image in display space.
	//
	// No RmlUi types in this header beyond forward declarations, matching how
	// ScriptEngine keeps sol2 out of its own.
	class UIEngine
	{
	public:
		// Must run AFTER Renderer::Init (needs a live bgfx and compiled shaders)
		// and AFTER ScriptEngine::Init (it shares that lua_State).
		static void Init(uint32_t width, uint32_t height);

		// Must run BEFORE ScriptEngine::Shutdown (the Lua plugin holds references
		// into the shared VM) and WHILE Renderer::IsGpuAlive() (Rml::Shutdown
		// releases textures, which after bgfx::shutdown would touch a dead context).
		static void Shutdown();

		static bool IsInitialized();

		static Rml::ElementDocument* LoadDocument(const std::filesystem::path& rmlPath);
		static void CloseAllDocuments();

		// Where the UI view composites. The editor passes the SceneRenderer's
		// composite target; a shipped game passes nullptr for the backbuffer.
		static void SetTarget(const Ref<Framebuffer>& target);

		static void OnUpdate(Timestep ts);   // Context::Update - layout and animation
		static void OnRender();              // submits draw lists to RenderPass::UI
		static void SetViewport(uint32_t width, uint32_t height);

		// Where the UI's top-left sits in window coordinates. The editor renders the
		// game into a panel, so incoming mouse positions are window-relative and have
		// to be shifted before RmlUi sees them; a shipped game leaves this at (0,0).
		// Kept here rather than passed per-event so the editor never needs RmlUi.
		static void SetViewportOrigin(float x, float y);

		// Translates engine events into Context::Process* calls.
		//
		// Marks the event Handled when RmlUi consumed it, so gameplay input does not
		// also fire. Note the inverted sense: Context::Process* returns TRUE when the
		// event should keep propagating, i.e. when the UI did NOT take it.
		static void OnEvent(Event& e);

		// ---- HUD data model ----
		// Deliberately a fixed pair rather than a general property bag: RmlUi data
		// models bind to real C++ variables declared up front, so "arbitrary values
		// from script" would need a different mechanism (see ui.md).
		static void SetHudHealth(float health);   // 0..100
		static void SetHudScore(int score);
		static float GetHudHealth();
		static int GetHudScore();

		static Rml::Context* GetContext();

		// Visual document inspector: element tree, computed RCSS, event log.
		// Debug builds only - RmlUi's Debugger sources are not compiled otherwise.
		static void SetDebuggerVisible(bool visible);
		static bool IsDebuggerVisible();
	};
}
