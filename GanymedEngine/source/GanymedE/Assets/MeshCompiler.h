#pragma once

#include "GanymedE/Assets/AssetCompiler.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Mesh.h"

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
	// **This compiler does not honour the interface's "pure function, no GPU" contract**, and
	// that is a known debt rather than an oversight. `Compile` runs `MeshImporter::Load`, which
	// builds a live `Mesh` - bgfx vertex buffers and all - and then serializes it, because
	// nothing in the importer can produce CPU-side mesh data on its own yet. The consequences:
	// a cold import creates its GPU buffers twice (once for the object that gets serialized and
	// thrown away, once for the one parsed back out of the blob), and this compiler must run on
	// the submit thread. The warm path - which is every load after the first - pays neither.
	// Splitting `MeshImporter` so it can emit a CPU-side description is the change that fixes
	// both, and it is the one Phase 5 cannot skip.
	class MeshCompiler : public IAssetCompiler
	{
	public:
		const char* Name() const override { return "MeshCompiler"; }

		// The blob format version. 6 carried over from MeshCache; 7 drops the embedded source
		// timestamp, which the `.dep` epoch record owns now.
		uint32_t Version() const override { return 7; }

		bool Compile(const CompileInput& input, CompileOutput& output) const override;

		// Blob -> live Mesh. The Apply half of the mesh manager's load: this is where the bgfx
		// buffers and the material textures are created, so it is main-thread only.
		static Ref<Mesh> Read(const std::vector<uint8_t>& blob,
			const std::filesystem::path& sourceRelativePath);
	};

}
