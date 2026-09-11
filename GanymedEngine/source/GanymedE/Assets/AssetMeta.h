#pragma once

#include "GanymedE/Assets/AssetTypes.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace GanymedE {

	// The `.meta` sidecar: `foo.png` -> `foo.png.meta`, holding the identity of `foo.png`.
	//
	// This is the file that replaced the single `assets/AssetRegistry.gr`. The registry was one
	// un-mergeable YAML per app holding every handle->path pair, which meant a scene and the
	// registry that resolves its handles had to travel together, two people importing assets on
	// two branches produced a guaranteed conflict, and a hand-edited scene could name a handle no
	// registry knew. Identity now lives beside the asset it identifies and is **committed**, so it
	// merges file-by-file, survives a `git mv`, and cannot go missing without the asset going with
	// it. `AssetManager`'s in-memory registry is a derived index rebuilt by scanning `assets/`.
	//
	// This is Unity's model. Unreal's alternative - path *is* identity, plus redirector objects
	// when something moves - needs every asset to be an engine-owned serialized object so the
	// engine can rewrite references, which Ganymed does not have: a `.png` is a `.png`.
	struct AssetMeta
	{
		AssetHandle Handle = InvalidAssetHandle;
		AssetType Type = AssetType::None;

		// Bumped when the *meaning* of a key in Config changes, so Phase 4's importers can
		// migrate old settings in a switch instead of guessing. Config is empty until then.
		int ImportConfigVersion = 0;

		// Flat scalar key/value, because that is the shape import settings actually take
		// (format, quality, generate-mips, sRGB, wrap mode). Ordered so emission is
		// deterministic and a rewrite is a clean diff. Unrecognized keys are read and written
		// back untouched - a sidecar from a newer engine, or one a human annotated, must not be
		// silently truncated by an older one. Nested config is deliberately not representable;
		// when something needs it, that is a format version bump, not a `YAML::Node` member in
		// a public header.
		std::map<std::string, std::string> Config;
	};

	// The version a sidecar written today carries.
	inline constexpr int CurrentImportConfigVersion = 1;

	// Read/write for `.meta`. Named for the file, not the struct, following MaterialSerializer.
	namespace AssetMetaSerializer {

		enum class ReadResult
		{
			Ok,
			Missing,   // no sidecar - the normal case for a newly added asset, not an error
			Corrupt    // present and unparseable; the caller decides whether to quarantine
		};

		// `foo.png` -> `foo.png.meta`. Appended, not substituted: two assets differing only by
		// extension (`tree.gltf` and `tree.png`) must not share one sidecar.
		std::filesystem::path SidecarPath(const std::filesystem::path& assetPath);

		// True for a path this scanner must never treat as an asset in its own right.
		bool IsSidecarPath(const std::filesystem::path& path);

		// Side-effect free, so a read-only session (a shipped game) can call it safely. It does
		// not quarantine and does not repair; `out` is untouched unless the result is Ok.
		ReadResult Read(const std::filesystem::path& assetFullPath, AssetMeta& out);

		// Emit to `<sidecar>.tmp`, then rename over it. `std::ofstream` truncates on open, so a
		// crash between the open and the flush would otherwise leave a zero-length sidecar - an
		// asset whose identity is gone, indistinguishable from one never imported. The rename is
		// atomic on NTFS and POSIX, so the file is either the old one or the new one. The same
		// discipline `SaveRegistry` used, now applied per asset.
		bool Write(const std::filesystem::path& assetFullPath, const AssetMeta& meta);

		// `foo.png.meta` -> `foo.png.meta.bad`. Moving an unreadable sidecar aside rather than
		// overwriting it is the point: the handle a hand-repair could have recovered is the only
		// link between this file and every scene referencing it.
		bool Quarantine(const std::filesystem::path& assetFullPath);

	}

}
