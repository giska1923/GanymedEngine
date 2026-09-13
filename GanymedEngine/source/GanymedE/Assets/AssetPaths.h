#pragma once

#include <filesystem>

namespace GanymedE {

	// ---- The project root ----
	//
	// Every path stored in a scene, a `.meta` sidecar or the registry is *relative to the
	// project root*, and this is the one place that turns such a path back into a real one.
	// It is deliberately not the process working directory: the engine's own assets (shaders,
	// the UI font) and the editor's own assets (its fonts, the checkerboard, its HUD document)
	// still resolve against the working directory, because they ship with the executable rather
	// than with the project. Only content the project owns moves when this moves.
	//
	// **Set once, before AssetManager::Init, and never again.** The parse stage reads this from
	// worker threads - MeshImporter, CompiledCache and MaterialSerializer all compose paths off
	// it - so a write after startup is a data race with no lock to take. Init is the only
	// intended caller; a second call with a different root asserts.
	void SetAssetRoot(const std::filesystem::path& root);

	const std::filesystem::path& GetAssetRoot();

	// Absolute (or working-directory-relative) path -> the project-relative form that goes into
	// a sidecar or a scene. Falls back to the input when the path is outside the root, which the
	// caller is expected to reject rather than store: see TextureImporter's IsInsideAssetRoot.
	inline std::filesystem::path MakeAssetRelative(const std::filesystem::path& path)
	{
		std::error_code ec;
		auto relative = std::filesystem::relative(path, GetAssetRoot(), ec);
		if (!ec)
			return relative;
		return path;
	}

}
