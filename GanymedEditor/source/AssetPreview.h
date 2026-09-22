#pragma once

#include "GanymedE/Assets/AssetTypes.h"

namespace GanymedE {

	// On-demand 3D preview of the Content Browser selection. One SceneRenderer,
	// one mesh submitted through Renderer3D, no scratch Scene. Created lazily,
	// evicted when the selection is not a mesh. See docs/editor/editor.md.
	class AssetPreview
	{
	public:
		static void Init();
		static void Shutdown();

		static void SetSelection(AssetHandle handle, AssetType type);
		static void MarkDirty();

		// After the main SceneRenderer::EndFrame. At most kRendersPerFrame
		// submits; a missing mesh stays dirty and retries next frame.
		static void Tick();

		// Inspector widget: image + LMB orbit + wheel zoom. No-op when the
		// selection is not a mesh or has not rendered yet.
		static void DrawInspector(float width);

		static uint32_t RenderCount();
	};

}
