#include "gepch.h"
#include "GanymedE/Renderer/MeshSource.h"

#include "GanymedE/Assets/TextureImporter.h"
#include "GanymedE/Core/JobSystem.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/MeshShader.h"
#include "GanymedE/Renderer/Texture.h"

namespace GanymedE {

	namespace {

		// A file path resolves through the asset manager, so two materials naming one image share
		// one decode and one GPU texture; embedded bytes have no file identity and are decoded
		// directly. Same rule the cold import and the blob replay have always shared.
		Ref<Texture2D> ResolveMap(const std::string& path, const std::vector<uint8_t>& embedded,
			AssetHandle& outHandle)
		{
			outHandle = InvalidAssetHandle;

			if (!path.empty())
				return TextureImporter::LoadMaterialMap(path, &outHandle);

			if (embedded.empty())
				return nullptr;

			// No file, so no handle and nothing to re-resolve: an embedded image is decoded here
			// and now, and is never pending.
			return TextureImporter::LoadFromMemory(embedded.data(), embedded.size(), true);
		}

	}

	Ref<Mesh> BuildMesh(const MeshSource& source)
	{
		GE_PROFILE_FUNCTION();

		// The one assert that matters on this path: everything below creates bgfx resources, and
		// a bgfx call from a worker is a corruption bug that manifests far from its cause.
		GE_CORE_ASSERT(JobSystem::IsMainThread(), "BuildMesh creates GPU resources - main thread only");

		if (!source.IsValid())
			return nullptr;

		Ref<Shader> shader = MeshShader::Get();

		std::vector<Ref<Material>> materials;
		materials.reserve(source.Materials.size());

		for (const MeshMaterialSource& desc : source.Materials)
		{
			Ref<Material> material = Material::Create(shader);
			material->SetName(desc.Name);
			material->SetAlbedoColor(desc.Albedo);
			material->SetMetallic(desc.Metallic);
			material->SetRoughness(desc.Roughness);
			material->SetTwoSided(desc.TwoSided);
			material->SetTransparent(desc.Transparent);

			material->SetAlbedoMapPath(desc.AlbedoMapPath);
			material->SetNormalMapPath(desc.NormalMapPath);
			material->SetMetallicRoughnessMapPath(desc.MetallicRoughnessMapPath);

			AssetHandle mapHandle = InvalidAssetHandle;

			material->SetAlbedoMap(ResolveMap(desc.AlbedoMapPath, desc.AlbedoEmbedded, mapHandle));
			material->SetAlbedoMapHandle(mapHandle);

			material->SetNormalMap(ResolveMap(desc.NormalMapPath, desc.NormalEmbedded, mapHandle));
			material->SetNormalMapHandle(mapHandle);

			material->SetMetallicRoughnessMap(ResolveMap(desc.MetallicRoughnessMapPath,
				desc.MetallicRoughnessEmbedded, mapHandle));
			material->SetMetallicRoughnessMapHandle(mapHandle);

			// Carried through so a re-serialize keeps the embedded image rather than dropping it,
			// and so MaterialSerializer::GenerateSidecars can extract it to a real file.
			material->SetAlbedoMapEmbeddedData(desc.AlbedoEmbedded);
			material->SetNormalMapEmbeddedData(desc.NormalEmbedded);
			material->SetMetallicRoughnessMapEmbeddedData(desc.MetallicRoughnessEmbedded);

			materials.push_back(std::move(material));
		}

		// A mesh with no material still needs one slot: Submesh::MaterialIndex indexes this list,
		// and Renderer3D falls back to entry 0.
		if (materials.empty())
			materials.push_back(Material::Create(shader));

		Ref<Mesh> mesh = Mesh::Create(source.Vertices, source.Indices, source.Submeshes, materials,
			source.SkinVertices, source.Skeleton, source.Clips);

		mesh->SetPath(source.RelativePath);
		return mesh;
	}

}
