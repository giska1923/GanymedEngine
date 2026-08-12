#pragma once

#include "GanymedE/Core/Core.h"

#include <cstdint>
#include <filesystem>

namespace GanymedE {

	class Texture2D;

	// One decode path for every asset-layer texture load. stb's flip flag is global
	// state, so both entry points set it explicitly immediately before decoding -
	// no call site depends on what a previous load left behind.
	//
	// Editor chrome (icons, the checkerboard) deliberately stays on the
	// Texture2D(path) constructor: those live outside the asset cache and are
	// addressed by hard-coded resources/ paths.
	class TextureImporter
	{
	public:
		// File-based material textures load unflipped, matching Texture2D(path):
		// bgfx normalises texture origin to top-left on every backend.
		static Ref<Texture2D> LoadFromFile(const std::filesystem::path& fullPath,
			bool flipVertically = false);

		// Images embedded in a glTF buffer view. Flipped by default only because
		// that is what MeshImporter/MeshCache did before this consolidation - see
		// the flip note in docs/engine/assets.md.
		static Ref<Texture2D> LoadFromMemory(const uint8_t* bytes, size_t size,
			bool flipVertically = true);

		// A material map recorded as a path relative to assets/. Routed through the
		// registry when the path stays inside the asset root, so meshes sharing an
		// external image decode once and share one bgfx texture. Paths that escape
		// the root have no registry identity and are decoded directly.
		//
		// Shared by cold import (MeshImporter) and cache replay (MeshCache) so the
		// de-duplication rule lives in exactly one place.
		static Ref<Texture2D> LoadMaterialMap(const std::filesystem::path& relativePath);
	};

}
