#pragma once

#include "GanymedE/Assets/AssetCompiler.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/MeshSource.h"

#include <filesystem>

namespace GanymedE {

	// glTF source -> the binary mesh blob the loader parses.
	//
	// This was `MeshCache`, an ad-hoc version of the same pipeline that owned its own file paths
	// under `assets/.assets/` and decided staleness from the source's mtime alone. The format is
	// unchanged; what moved out is path and invalidation policy, which `CompiledCache` now owns
	// for every asset type - so a touched-but-unedited `.glb` no longer reimports, and bumping
	// Version() below invalidates every mesh blob at once.
	//
	// **This compiler is GPU-free**, which it was not before Phase 5. `Compile` used to run the
	// importer's `Load`, which built a live `Mesh` - bgfx buffers, materials, textures - purely so
	// it could serialize it, so it had to run on the submit thread and created its GPU objects
	// twice on a cold import. `MeshSource` split that: the importer emits CPU data, this
	// serializes it, and `BuildMesh` is the only main-thread step. That is what makes mesh loading
	// asynchronous rather than "asynchronous except the expensive part".
	class MeshCompiler : public IAssetCompiler
	{
	public:
		const char* Name() const override { return "MeshCompiler"; }

		// The blob format version. 6 carried over from MeshCache; 7 drops the embedded source
		// timestamp, which the `.dep` epoch record owns now.
		uint32_t Version() const override { return 7; }

		bool Compile(const CompileInput& input, CompileOutput& output) const override;

		// Blob -> CPU-side mesh data. No bgfx call anywhere below it, so this is the manager's
		// Parse stage and runs on a worker; `BuildMesh` turns the result into a live Mesh.
		static bool Read(const std::vector<uint8_t>& blob,
			const std::filesystem::path& sourceRelativePath, MeshSource& out);
	};

}
