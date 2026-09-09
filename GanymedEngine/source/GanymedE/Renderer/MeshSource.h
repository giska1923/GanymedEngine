#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Animation.h"
#include "GanymedE/Renderer/Mesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace GanymedE {

	// A mesh as CPU-side data: everything a `.glb` contains, and nothing the GPU owns.
	//
	// **This type is why mesh loading can be asynchronous.** The importer used to go straight from
	// cgltf to a live `Mesh` - bgfx vertex buffers, materials, textures - which meant the asset
	// compiler had to build GPU objects just to serialize them, and therefore had to run on the
	// submit thread. Phases 2 and 4 both named that as the debt Phase 5 could not skip; this is
	// the split that pays it. `MeshImporter::Import` and `MeshCompiler::Read` both produce one of
	// these on a worker thread, and `BuildMesh` is the only step that has to be on the main one.
	//
	// Materials are *descriptions* here, not `Material` objects: scalars, map paths, and the
	// compressed bytes of any texture embedded in the model file. That is exactly what the mesh
	// blob has always stored, so the on-disk format did not change when this type appeared.
	struct MeshMaterialSource
	{
		std::string Name = "Material";

		glm::vec4 Albedo{ 1.0f };
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		bool TwoSided = false;
		bool Transparent = false;

		// Asset-root-relative when the image is a file on disk; empty when it is embedded in the
		// model, in which case the compressed bytes below carry it.
		std::string AlbedoMapPath;
		std::string NormalMapPath;
		std::string MetallicRoughnessMapPath;

		std::vector<uint8_t> AlbedoEmbedded;
		std::vector<uint8_t> NormalEmbedded;
		std::vector<uint8_t> MetallicRoughnessEmbedded;
	};

	struct MeshSource
	{
		std::vector<MeshVertex> Vertices;
		std::vector<uint32_t> Indices;
		std::vector<Submesh> Submeshes;
		std::vector<MeshMaterialSource> Materials;

		// Empty, or exactly parallel to Vertices - see SkinVertex.
		std::vector<SkinVertex> SkinVertices;
		Skeleton Skeleton;
		std::vector<AnimationClip> Clips;

		std::string RelativePath;

		bool IsValid() const { return !Vertices.empty() && !Indices.empty(); }
	};

	// CPU data -> live `Mesh`: creates the bgfx vertex and index buffers, and resolves each
	// material's maps through the texture manager (or decodes its embedded bytes).
	//
	// **Main thread only.** Every bgfx call on the mesh path is inside here, which is what lets
	// everything above it run on a worker.
	Ref<Mesh> BuildMesh(const MeshSource& source);

}
