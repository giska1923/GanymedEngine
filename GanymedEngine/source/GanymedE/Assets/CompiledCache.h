#pragma once

#include "GanymedE/Assets/AssetCompiler.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace GanymedE {

	// The source -> compiled artifact layer: `foo.png` on disk becomes a BC7 DDS under
	// `assets/.compiled/`, and `foo.glb` becomes the binary mesh blob that used to live in
	// `assets/.assets/`.
	//
	// This generalizes `MeshCache`, which was a single-purpose version of the same thing with a
	// weaker staleness rule (source mtime only). The tree is **gitignored derived output**: it
	// can be deleted at any time and the next load rebuilds it.

	// Which parts of the epoch record moved. Reported in the recompile log line, because
	// "why did this recompile" is the question an invalidation bug makes you ask.
	enum class EpochDiff : uint32_t
	{
		None            = 0,
		CompilerVersion = BIT(0),
		SourceSize      = BIT(1),
		SourceMtime     = BIT(2),
		SourceHash      = BIT(3),
		Dependencies    = BIT(4),
		Config          = BIT(5),
	};

	inline EpochDiff operator|(EpochDiff a, EpochDiff b)
	{
		return (EpochDiff)((uint32_t)a | (uint32_t)b);
	}

	inline EpochDiff& operator|=(EpochDiff& a, EpochDiff b) { a = a | b; return a; }
	inline bool HasDiff(EpochDiff set, EpochDiff flag) { return ((uint32_t)set & (uint32_t)flag) != 0; }

	std::string EpochDiffToString(EpochDiff diff);

	class CompiledCache
	{
	public:
		// One compiler per asset type, installed at AssetManager::Init. A type with no compiler
		// is not an error: Open falls back to handing back the source bytes, which is the right
		// answer for `.hdr` environments (the expensive part is a GPU bake that cannot be
		// precomputed into bytes) and for anything else that has no offline step yet.
		static void RegisterCompiler(AssetType type, Scope<IAssetCompiler> compiler);
		static void Shutdown();

		static const IAssetCompiler* CompilerFor(AssetType type);

		// The bytes a loader should parse: the compiled artifact, compiled first if the epoch
		// record says it is stale. False when the source is missing or the compiler failed.
		//
		// Blocking, and on a cold tree it is *slow* - BC7 is seconds per 2K texture. Phase 5 is
		// what makes it non-blocking; until then the editor says so rather than pretending.
		static bool Open(const AssetMetadata& metadata, std::vector<uint8_t>& out);

		// Delete this asset's compiled output and its epoch record, so the next Open recompiles.
		// This is "reimport now" - the epoch already catches an edited source on its own.
		static bool Invalidate(const AssetMetadata& metadata);

		// `assets/.compiled/<h0h1>/<h>.gres`, where h is the hash of the asset-root-relative
		// source path. Hashing the path keeps the tree flat and bounded regardless of how deep
		// `assets/` gets, and it is one function, so adding a platform tag to the key the day a
		// second build target exists is a one-line change (roadmap decision 9).
		static std::filesystem::path OutputPath(const std::string& relativeSourcePath);

		// Compiles performed this session, and the wall-clock they cost. The editor shows these;
		// they are also what the threading milestone's before/after measurement reads.
		struct Stats
		{
			uint32_t Compiles = 0;
			uint32_t CacheHits = 0;
			double TotalCompileMs = 0.0;
		};

		// **Open is called from worker threads.** Everything it touches is either local, passed
		// in by value, or atomic; the compilers themselves are pure functions of their input,
		// which is the contract IAssetCompiler states. Two loads of the same asset cannot race
		// here because TypedAssetManager::Load dedupes by handle on the main thread before ever
		// queueing a parse.

		// By value, not by reference: the counters are written from worker threads now, so
		// handing out a reference would be handing out a race.
		static Stats GetStats();
		static void ResetStats();

		// Compiles running right now, across every worker. Zero when the tree is warm, which is
		// what the editor's status line keys on.
		static uint32_t CompilesInFlight();
	};

}
