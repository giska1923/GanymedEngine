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
		// one decode and one GPU texture; an embedded image has no file identity and was decoded
		// into the source during Parse. Same rule the cold import and the blob replay have always
		// shared.
		Ref<Texture2D> ResolveMap(const std::string& path, const DecodedImage& decoded,
			AssetHandle& outHandle)
		{
			outHandle = InvalidAssetHandle;

			if (!path.empty())
				return TextureImporter::LoadMaterialMap(path, &outHandle);

			if (!decoded)
				return nullptr;

			// The upload and nothing else - the decode happened on a worker. No handle, so
			// nothing to re-resolve: an embedded image is never pending.
			return TextureImporter::Upload(decoded);
		}

	}

	bool DecodeEmbeddedMaps(MeshSource& source)
	{
		GE_PROFILE_FUNCTION();

		// Flipped, because that is what LoadFromMemory did when this decode lived in BuildMesh -
		// see the flip note in docs/engine/assets.md. Moving work between threads must not move
		// the image.
		auto decode = [](const std::string& path, const std::vector<uint8_t>& bytes) -> DecodedImage
		{
			if (!path.empty() || bytes.empty())
				return {};

			return TextureImporter::DecodeFromMemory(bytes.data(), bytes.size(), true);
		};

		for (MeshMaterialSource& material : source.Materials)
		{
			// Between materials rather than inside one: a decode cannot be interrupted, and this
			// is the granularity the threading milestone asks a long body to poll at.
			if (JobSystem::IsCurrentJobCancelled())
				return false;

			material.AlbedoDecoded = decode(material.AlbedoMapPath, material.AlbedoEmbedded);
			material.NormalDecoded = decode(material.NormalMapPath, material.NormalEmbedded);
			material.MetallicRoughnessDecoded =
				decode(material.MetallicRoughnessMapPath, material.MetallicRoughnessEmbedded);
		}

		return true;
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

			material->SetAlbedoMap(ResolveMap(desc.AlbedoMapPath, desc.AlbedoDecoded, mapHandle));
			material->SetAlbedoMapHandle(mapHandle);

			material->SetNormalMap(ResolveMap(desc.NormalMapPath, desc.NormalDecoded, mapHandle));
			material->SetNormalMapHandle(mapHandle);

			material->SetMetallicRoughnessMap(ResolveMap(desc.MetallicRoughnessMapPath,
				desc.MetallicRoughnessDecoded, mapHandle));
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
