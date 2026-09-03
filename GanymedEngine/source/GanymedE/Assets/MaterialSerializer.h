#pragma once

#include "GanymedE/Core/Core.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace GanymedE {

	class Material;
	class Mesh;

	// Read/write for `.gmat`, the material asset - and the sidecar rules that produce them.
	//
	// The naming rule and the generator live in the same file on purpose: `MeshImporter`
	// computes sidecar paths at instantiation and the mesh loader writes them, so if those two
	// ever disagreed about a name, entities would author against files nothing generates.
	namespace MaterialSerializer {

		// Null on a missing or malformed file - logged, never throws. The shader is implicit:
		// every `.gmat` binds MeshShader::Get(), because Material::Bind asserts on a null
		// shader and shader variants do not exist yet.
		Ref<Material> Load(const std::filesystem::path& fullPath);

		// Writes the canonical key order. A write -> read -> write round trip is byte-identical,
		// the Phase 1 save discipline applied to a new format.
		bool Save(const Ref<Material>& material, const std::filesystem::path& fullPath);

		// `<meshdir>/<meshstem>_mat<index>_<name>.gmat`, relative to assets/.
		//
		// The *index* is the identity, not the name: glTF material names are optional,
		// non-unique and unsanitized, and `Submesh::MaterialIndex` already speaks index. The
		// name rides along for humans.
		std::filesystem::path SidecarPath(const std::filesystem::path& meshRelativePath,
			uint32_t materialIndex, const std::string& materialName);

		// Writes one `.gmat` per material slot of a freshly loaded mesh, and extracts any
		// textures embedded in the model file so those `.gmat`s can reference real files.
		//
		// Both halves are idempotent and **never overwrite**: a re-import, or a cache rebuild,
		// must not clobber a material a human has edited. No-op when the process may not write
		// into assets/ (AssetManager::IsRegistryWritable).
		void GenerateSidecars(const Ref<Mesh>& mesh, const std::filesystem::path& meshRelativePath);

	}

}
