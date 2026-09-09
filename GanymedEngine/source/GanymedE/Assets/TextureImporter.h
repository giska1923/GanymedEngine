#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace GanymedE {

	class Texture2D;

	// A decoded image still on the CPU: RGBA8, tightly packed, owning stb's buffer.
	//
	// This is the seam between Parse and Apply for textures - decoding is pure CPU work that
	// Phase 5 moves to a worker thread, while the bgfx texture it becomes must be created on
	// the submit thread (ASSET_PIPELINE_ROADMAP.md decision 13). Move-only, because there is
	// exactly one owner of the pixels at a time.
	struct DecodedImage
	{
		// stbi_image_free, kept out of this header so stb_image does not leak into every TU
		// that mentions a texture.
		struct PixelDeleter
		{
			void operator()(uint8_t* pixels) const;
		};

		std::unique_ptr<uint8_t, PixelDeleter> Pixels;
		uint32_t Width = 0;
		uint32_t Height = 0;

		explicit operator bool() const { return Pixels != nullptr; }
	};

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
		// The CPU half. No bgfx call reachable from either - that is the contract Phase 5
		// depends on, and it is why the LoadFrom* pair below is expressed as Decode + Upload
		// rather than the other way around.
		static DecodedImage Decode(const std::filesystem::path& fullPath,
			bool flipVertically = false);
		static DecodedImage DecodeFromMemory(const uint8_t* bytes, size_t size,
			bool flipVertically = true);

		// The GPU half. Main thread only.
		static Ref<Texture2D> Upload(const DecodedImage& image);

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
		// Shared by BuildMesh and MaterialSerializer so the de-duplication rule lives in
		// exactly one place.
		//
		// **Null is now a normal answer, not just a failure**: loading is asynchronous, so a
		// texture that has only just been asked for is not here yet. `outHandle` gets the asset
		// identity when there is one, which is what lets Material re-ask on a later frame
		// instead of keeping a permanent hole - see Material::Bind.
		static Ref<Texture2D> LoadMaterialMap(const std::filesystem::path& relativePath,
			AssetHandle* outHandle = nullptr);
	};

}
