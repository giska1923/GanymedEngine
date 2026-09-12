#pragma once

#include "GanymedE/Assets/TextureImporter.h"
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

		// The same images, decoded to RGBA8 by `DecodeEmbeddedMaps` on a worker thread. Empty
		// for a map that names a file - those resolve through the texture manager, which decodes
		// them on a worker of its own and shares one GPU texture between every material naming
		// the same image - and empty for one whose decode failed.
		//
		// **This is why an embedded texture no longer costs the main thread.** `BuildMesh` used
		// to hand the compressed bytes below to stb and decode them inline, which measured 62.8
		// ms for CesiumMan.glb and 52.3 ms for Fox.glb against 1.6 ms and 0.5 ms for the bgfx
		// buffer creation beside it - so the dominant cost of applying a mesh was CPU work with
		// no reason to be on the submit thread. The compressed bytes are still kept:
		// `MaterialSerializer::GenerateSidecars` extracts them to a real file on first import,
		// and re-encoding RGBA8 to get them back would be absurd.
		DecodedImage AlbedoDecoded;
		DecodedImage NormalDecoded;
		DecodedImage MetallicRoughnessDecoded;
	};

	struct MeshSource
	{
		std::vector<MeshVertex> Vertices;
		std::vector<uint32_t> Indices;
		std::vector<Submesh> Submeshes;
		std::vector<MeshMaterialSource> Materials;

		// Empty, or exactly parallel to Vertices - see SkinVertex.
		std::vector<SkinVertex> SkinVertices;
		// Qualified because the member name shadows the type name for the rest of this scope,
		// as in Components.h. Not a style choice: [basic.scope.class] requires a name to mean
		// one thing throughout a class, and GCC enforces it as an error where MSVC does not.
		GanymedE::Skeleton Skeleton;
		std::vector<AnimationClip> Clips;

		std::string RelativePath;

		bool IsValid() const { return !Vertices.empty() && !Indices.empty(); }
	};

	// Decode every embedded image in `source` into its `*Decoded` slot.
	//
	// **Worker thread**, from the mesh Parse stage - it is the reason that stage exists. Returns
	// false if the job was cancelled part way, in which case the source is left half-filled and
	// the caller must discard it. A map that fails to decode is left empty and does not fail the
	// mesh: a broken texture inside a model should cost that texture, not the model.
	bool DecodeEmbeddedMaps(MeshSource& source);

	// CPU data -> live `Mesh`: creates the bgfx vertex and index buffers, and resolves each
	// material's maps through the texture manager (or uploads one decoded during Parse).
	//
	// **Main thread only.** Every bgfx call on the mesh path is inside here, which is what lets
	// everything above it run on a worker.
	Ref<Mesh> BuildMesh(const MeshSource& source);

}
