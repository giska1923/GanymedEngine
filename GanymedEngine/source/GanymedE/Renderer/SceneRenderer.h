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

		uint32_t GetFinalImageRendererID() const;
		const Ref<Framebuffer>& GetSceneFramebuffer() const { return m_SceneFramebuffer; }

		SceneRendererSettings& GetSettings() { return m_Settings; }
		const SceneRendererSettings& GetSettings() const { return m_Settings; }

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
	private:
		void RebuildBloomChain();
		// Returns the framebuffer holding the final blurred bloom (half resolution)
		Ref<Framebuffer> RenderBloom();
	private:
		uint32_t m_Width = 0, m_Height = 0;
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
