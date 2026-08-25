#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Framebuffer.h"
#include "GanymedE/Renderer/Shader.h"

#include <glm/glm.hpp>

#include <vector>

namespace GanymedE {

	struct SceneRendererSettings
	{
		glm::vec4 ClearColor{ 0.1f, 0.1f, 0.1f, 1.0f };
		float Exposure = 1.0f;

		bool BloomEnabled = true;
		float BloomThreshold = 1.0f;   // luminance where bloom starts contributing
		float BloomKnee = 0.5f;        // soft-knee width as a fraction of the threshold
		float BloomIntensity = 0.35f;  // composite strength
		float BloomFilterRadius = 1.0f;

		bool FXAAEnabled = true;
	};

	// Render-graph-lite: owns the frame's render targets and formalizes the pass
	// order (scene HDR -> bloom chain -> tonemap -> FXAA -> composite) that used
	// to be hand-wired in EditorLayer::OnUpdate.
	class SceneRenderer
	{
	public:
		SceneRenderer(uint32_t width, uint32_t height);

		void SetViewportSize(uint32_t width, uint32_t height);

		// Send the post stack's FINAL pass to the backbuffer instead of the composite
		// framebuffer. For a front-end that owns the whole window (the standalone
		// runtime) rather than showing the scene inside a panel.
		//
		// This retargets the existing last pass - FXAA when enabled, tonemap when not -
		// rather than adding a present/blit pass. A dedicated present pass is the
		// production norm (Unity/Unreal both end on one) because it carries resolution
		// scaling and HDR-display duties; none of those exist here yet, and it would
		// cost a new view ID above RenderPass::UI plus a new shader, because
		// vs_Blit.sc wants a_texcoord0 while the fullscreen quad only supplies
		// a_Position. See docs/engine/rendering.md for the escape hatch.
		//
		// Contract: in backbuffer mode the host must keep SetViewportSize fed with the
		// WINDOW size, because the final view rect is taken from it. The intermediate
		// targets (HDR, bloom chain, tonemap) are still allocated at that size, so
		// there is no resolution-scaling knob hiding in here.
		void SetOutputToBackbuffer(bool enabled) { m_OutputToBackbuffer = enabled; }
		bool IsOutputToBackbuffer() const { return m_OutputToBackbuffer; }

		// Binds and clears the HDR scene target (color, depth, entity IDs).
		// Render the scene (Renderer3D/Renderer2D) between Begin and End.
		void BeginFrame();

		// Unbinds the scene target and runs the post stack into the composite target.
		void EndFrame();

		// Entity-ID readback from the scene target (attachment 1), viewport coords.
		//
		// bgfx cannot read a render target synchronously, so this is a
		// request/poll pair rather than the old blocking ReadEntityID: queue a
		// pick now, collect it a frame or two later. For hover highlighting that
		// latency is imperceptible. See docs/toDo&done/BGFX_MIGRATION.md §7.
		void RequestEntityID(int x, int y);

		// Returns true and fills outEntityID when a queued pick has landed.
		// Only the newest completed result is reported; older ones are dropped,
		// so a stale pixel never overwrites a newer one.
		bool PollEntityID(int& outEntityID);

		// Asserts in backbuffer mode: nothing writes the composite target there.
		uint32_t GetFinalImageRendererID() const;
		const Ref<Framebuffer>& GetSceneFramebuffer() const { return m_SceneFramebuffer; }

		// The LDR target the post stack resolves into, and what the viewport image
		// shows. Exposed so the game UI can composite into it: RenderPass::UI sorts
		// after Composite, so anything drawn there lands on the finished image in
		// display space rather than being tonemapped with the scene.
		//
		// Unused in backbuffer mode - a host in that mode passes nullptr to
		// UIEngine::SetTarget so the UI view lands on the backbuffer too.
		const Ref<Framebuffer>& GetCompositeFramebuffer() const { return m_CompositeFramebuffer; }

		SceneRendererSettings& GetSettings() { return m_Settings; }
		const SceneRendererSettings& GetSettings() const { return m_Settings; }

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
	private:
		void RebuildBloomChain();
		// Returns the framebuffer holding the final blurred bloom (half resolution)
		Ref<Framebuffer> RenderBloom();
	private:
		// Points a view at the backbuffer and sizes it to the window. The counterpart to
		// Framebuffer::BindToView, which does the same for an offscreen target.
		void BindFinalPassToBackbuffer(uint16_t viewId) const;
	private:
		uint32_t m_Width = 0, m_Height = 0;
		bool m_OutputToBackbuffer = false;
		SceneRendererSettings m_Settings;

		Ref<Framebuffer> m_SceneFramebuffer;     // HDR: RGBA16F + entity ID + depth
		Ref<Framebuffer> m_TonemapFramebuffer;   // LDR intermediate (FXAA input)
		Ref<Framebuffer> m_CompositeFramebuffer; // LDR final (shown in the viewport)

		std::vector<Ref<Framebuffer>> m_BloomMips; // RGBA16F, halved per level

		Ref<Shader> m_BloomDownsampleShader;
		Ref<Shader> m_BloomUpsampleShader;
		Ref<Shader> m_FXAAShader;

		// Picks in flight. A slot's Value is written by bgfx itself once the GPU
		// reaches ReadyFrame, so the storage must outlive the request - hence a
		// fixed array rather than a resizing container.
		struct PendingPick
		{
			bool InFlight = false;
			uint32_t ReadyFrame = 0;
			// R32I gives an int; the R32F fallback gives a float in the same bytes.
			union { int32_t AsInt; float AsFloat; } Value { -1 };
		};

		static constexpr uint32_t kMaxPicksInFlight = 4;
		PendingPick m_Picks[kMaxPicksInFlight];
	};

}
