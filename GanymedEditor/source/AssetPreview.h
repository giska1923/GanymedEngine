#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <cstddef>
#include <cstdint>

namespace GanymedE {

	class Texture2D;

	// On-demand 3D preview of the Content Browser selection, plus a budgeted
	// thumbnail queue for the grid and map palette. One inspector SceneRenderer
	// and one 128×128 thumbnail SceneRenderer; they do not share a framebuffer.
	// See docs/editor/editor.md.
	class AssetPreview
	{
	public:
		static void Init();
		static void Shutdown();

		static void SetSelection(AssetHandle handle, AssetType type);
		static void MarkDirty();

		// After the main SceneRenderer::EndFrame. At most kRendersPerFrame
		// submits; inspector wins over thumbnails. A missing mesh stays dirty
		// and retries next frame.
		static void Tick();

		// Inspector widget: image + LMB orbit + wheel zoom. No-op when the
		// selection is not a mesh or has not rendered yet.
		static void DrawInspector(float width);

		static uint32_t RenderCount();

		// Content Browser / map palette. RequestVisible records a handle shown
		// this ImGui frame; Tick consumes last frame's set. GridIdle false
		// (scrolling) skips GPU renders so a fast scroll cannot queue hundreds.
		static void RequestVisible(AssetHandle handle);
		static void SetGridIdle(bool idle);
		static Ref<Texture2D> GetThumbnail(AssetHandle handle);

		static uint32_t ThumbnailRenderCount();
		static std::size_t ThumbnailResident();
		static uint64_t ThumbnailDiskBytes();
	};

}
