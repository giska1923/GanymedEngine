#pragma once

#include "GanymedE/Core/Core.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace GanymedE {

	class Material;
	class Mesh;

	// A `.gmat` as read off disk, before anything is resolved or created.
	//
	// The Parse/Apply seam for materials (ASSET_PIPELINE_ROADMAP.md decision 13): reading the
	// YAML is pure CPU work Phase 5 moves to a worker, while resolving the map paths pulls
	// textures through `AssetManager` and therefore creates bgfx resources. Defaults mirror
	// `Material`'s own, because a key absent from the file means "whatever the class starts at".
	struct MaterialDesc
	{
		std::string Name = "Material";
		glm::vec4 Albedo{ 1.0f };
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		bool TwoSided = false;
		bool Transparent = false;

		std::string AlbedoMapPath;
		std::string NormalMapPath;
		std::string MetallicRoughnessMapPath;
	};

	// Read/write for `.gmat`, the material asset - and the sidecar rules that produce them.
	//
	// The naming rule and the generator live in the same file on purpose: `MeshImporter`
	// computes sidecar paths at instantiation and the mesh loader writes them, so if those two
	// ever disagreed about a name, entities would author against files nothing generates.
	namespace MaterialSerializer {

		// The CPU half: file -> MaterialDesc. False on a missing or malformed file - logged,
		// never throws. Reachable from a worker thread; nothing below it touches bgfx.
		bool ReadDesc(const std::filesystem::path& fullPath, MaterialDesc& out);

		// The GPU half: MaterialDesc -> live Material, resolving each map path through
		// `TextureImporter::LoadMaterialMap` so two materials naming one image share a decode.
		// Main thread only. The shader is implicit: every `.gmat` binds MeshShader::Get(),
		// because Material::Bind asserts on a null shader and shader variants do not exist yet.
		Ref<Material> Build(const MaterialDesc& desc);

		// ReadDesc + Build. Null on a missing or malformed file.
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
		// into assets/ (AssetManager::IsAssetsWritable).
		void GenerateSidecars(const Ref<Mesh>& mesh, const std::filesystem::path& meshRelativePath);

	}

}
