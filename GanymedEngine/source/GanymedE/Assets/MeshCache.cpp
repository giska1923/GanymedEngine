#include "gepch.h"
#include "GanymedE/Renderer/MeshShader.h"
#include "MeshCache.h"

#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/MeshImporter.h"
#include "GanymedE/Renderer/Shader.h"
#include "GanymedE/Renderer/Texture.h"

#include <stb_image.h>

#include <fstream>

namespace GanymedE {

	namespace {

		constexpr uint32_t MESH_CACHE_MAGIC = 0x48434D47; // 'GMCH'
		constexpr uint32_t MESH_CACHE_VERSION = 3; // v3: embedded texture bytes for pathless maps

		void WriteString(std::ostream& out, const std::string& str)
		{
			uint32_t size = (uint32_t)str.size();
			out.write(reinterpret_cast<const char*>(&size), sizeof(size));
			out.write(str.data(), size);
		}

		std::string ReadString(std::istream& in)
		{
			uint32_t size = 0;
			in.read(reinterpret_cast<char*>(&size), sizeof(size));
			std::string str(size, '\0');
			if (size > 0)
				in.read(str.data(), size);
			return str;
		}

		template<typename T>
		void WriteValue(std::ostream& out, const T& value)
		{
			out.write(reinterpret_cast<const char*>(&value), sizeof(T));
		}

		template<typename T>
		void ReadValue(std::istream& in, T& value)
		{
			in.read(reinterpret_cast<char*>(&value), sizeof(T));
		}

		void WriteBlob(std::ostream& out, const std::vector<uint8_t>& data)
		{
			uint32_t size = (uint32_t)data.size();
			WriteValue(out, size);
			if (size > 0)
				out.write(reinterpret_cast<const char*>(data.data()), size);
		}

		std::vector<uint8_t> ReadBlob(std::istream& in)
		{
			uint32_t size = 0;
			ReadValue(in, size);
			std::vector<uint8_t> data(size);
			if (size > 0)
				in.read(reinterpret_cast<char*>(data.data()), size);
			return data;
		}

		// Mirrors MeshImporter's embedded-image decode (compressed PNG/JPEG bytes -> RGBA texture)
		Ref<Texture2D> CreateTextureFromEmbedded(const std::vector<uint8_t>& data)
		{
			if (data.empty())
				return nullptr;

			stbi_set_flip_vertically_on_load(1);
			int width, height, channels;
			unsigned char* pixels = stbi_load_from_memory(data.data(), (int)data.size(), &width, &height, &channels, 4);
			if (!pixels)
				return nullptr;

			Ref<Texture2D> texture = Texture2D::Create((uint32_t)width, (uint32_t)height);
			texture->SetData(pixels, width * height * 4);
			stbi_image_free(pixels);
			return texture;
		}

		void WriteVector(std::ostream& out, const std::vector<MeshVertex>& vertices)
		{
			uint32_t count = (uint32_t)vertices.size();
			WriteValue(out, count);
			if (count > 0)
				out.write(reinterpret_cast<const char*>(vertices.data()), count * sizeof(MeshVertex));
		}

		void ReadVector(std::istream& in, std::vector<MeshVertex>& vertices)
		{
			uint32_t count = 0;
			ReadValue(in, count);
			vertices.resize(count);
			if (count > 0)
				in.read(reinterpret_cast<char*>(vertices.data()), count * sizeof(MeshVertex));
		}

		void WriteIndices(std::ostream& out, const std::vector<uint32_t>& indices)
		{
			uint32_t count = (uint32_t)indices.size();
			WriteValue(out, count);
			if (count > 0)
				out.write(reinterpret_cast<const char*>(indices.data()), count * sizeof(uint32_t));
		}

		void ReadIndices(std::istream& in, std::vector<uint32_t>& indices)
		{
			uint32_t count = 0;
			ReadValue(in, count);
			indices.resize(count);
			if (count > 0)
				in.read(reinterpret_cast<char*>(indices.data()), count * sizeof(uint32_t));
		}

		void WriteSubmeshes(std::ostream& out, const std::vector<Submesh>& submeshes)
		{
			uint32_t count = (uint32_t)submeshes.size();
			WriteValue(out, count);
			for (const auto& submesh : submeshes)
			{
				WriteValue(out, submesh.BaseVertex);
				WriteValue(out, submesh.BaseIndex);
				WriteValue(out, submesh.IndexCount);
				WriteValue(out, submesh.MaterialIndex);
				WriteValue(out, submesh.LocalTransform);
				WriteString(out, submesh.Name);
			}
		}

		void ReadSubmeshes(std::istream& in, std::vector<Submesh>& submeshes)
		{
			uint32_t count = 0;
			ReadValue(in, count);
			submeshes.resize(count);
			for (auto& submesh : submeshes)
			{
				ReadValue(in, submesh.BaseVertex);
				ReadValue(in, submesh.BaseIndex);
				ReadValue(in, submesh.IndexCount);
				ReadValue(in, submesh.MaterialIndex);
				ReadValue(in, submesh.LocalTransform);
				submesh.Name = ReadString(in);
			}
		}

		void WriteMaterials(std::ostream& out, const std::vector<Ref<Material>>& materials)
		{
			uint32_t count = (uint32_t)materials.size();
			WriteValue(out, count);
			for (const auto& material : materials)
			{
				if (!material)
				{
					WriteString(out, "");
					WriteValue(out, glm::vec4(1.0f));
					WriteValue(out, 0.0f);
					WriteValue(out, 0.5f);
					WriteValue(out, false);
					WriteValue(out, false);
					WriteString(out, "");
					WriteBlob(out, {});
					WriteString(out, "");
					WriteBlob(out, {});
					WriteString(out, "");
					WriteBlob(out, {});
					continue;
				}

				WriteString(out, material->GetName());
				WriteValue(out, material->GetAlbedoColor());
				WriteValue(out, material->GetMetallic());
				WriteValue(out, material->GetRoughness());
				WriteValue(out, material->IsTwoSided());
				WriteValue(out, material->IsTransparent());
				WriteString(out, material->GetAlbedoMapPath());
				WriteBlob(out, material->GetAlbedoMapEmbeddedData());
				WriteString(out, material->GetNormalMapPath());
				WriteBlob(out, material->GetNormalMapEmbeddedData());
				WriteString(out, material->GetMetallicRoughnessMapPath());
				WriteBlob(out, material->GetMetallicRoughnessMapEmbeddedData());
			}
		}

		std::vector<Ref<Material>> ReadMaterials(std::istream& in)
		{
			uint32_t count = 0;
			ReadValue(in, count);

			Ref<Shader> shader = MeshShader::Get();
			std::vector<Ref<Material>> materials;
			materials.reserve(count);

			for (uint32_t i = 0; i < count; i++)
			{
				Ref<Material> material = Material::Create(shader);
				material->SetName(ReadString(in));

				glm::vec4 albedo;
				float metallic, roughness;
				bool twoSided, transparent;
				ReadValue(in, albedo);
				ReadValue(in, metallic);
				ReadValue(in, roughness);
				ReadValue(in, twoSided);
				ReadValue(in, transparent);

				material->SetAlbedoColor(albedo);
				material->SetMetallic(metallic);
				material->SetRoughness(roughness);
				material->SetTwoSided(twoSided);
				material->SetTransparent(transparent);

				std::string albedoPath = ReadString(in);
				std::vector<uint8_t> albedoEmbedded = ReadBlob(in);
				std::string normalPath = ReadString(in);
				std::vector<uint8_t> normalEmbedded = ReadBlob(in);
				std::string mrPath = ReadString(in);
				std::vector<uint8_t> mrEmbedded = ReadBlob(in);

				material->SetAlbedoMapPath(albedoPath);
				material->SetNormalMapPath(normalPath);
				material->SetMetallicRoughnessMapPath(mrPath);

				if (!albedoPath.empty())
					material->SetAlbedoMap(Texture2D::Create((GetAssetRoot() / albedoPath).string()));
				else
					material->SetAlbedoMap(CreateTextureFromEmbedded(albedoEmbedded));

				if (!normalPath.empty())
					material->SetNormalMap(Texture2D::Create((GetAssetRoot() / normalPath).string()));
				else
					material->SetNormalMap(CreateTextureFromEmbedded(normalEmbedded));

				if (!mrPath.empty())
					material->SetMetallicRoughnessMap(Texture2D::Create((GetAssetRoot() / mrPath).string()));
				else
					material->SetMetallicRoughnessMap(CreateTextureFromEmbedded(mrEmbedded));

				// Keep the bytes so a future cache rewrite doesn't drop the textures
				material->SetAlbedoMapEmbeddedData(std::move(albedoEmbedded));
				material->SetNormalMapEmbeddedData(std::move(normalEmbedded));
				material->SetMetallicRoughnessMapEmbeddedData(std::move(mrEmbedded));

				materials.push_back(material);
			}

			if (materials.empty())
				materials.push_back(Material::Create(shader));

			return materials;
		}

	}

	std::filesystem::path MeshCache::GetCachePath(const std::filesystem::path& sourceRelativePath)
	{
		std::string filename = sourceRelativePath.generic_string();
		std::replace(filename.begin(), filename.end(), '/', '_');
		std::replace(filename.begin(), filename.end(), '\\', '_');
		return GetAssetRoot() / ".assets" / "meshes" / (filename + ".meshcache");
	}

	uint64_t MeshCache::GetFileTimestamp(const std::filesystem::path& path)
	{
		std::error_code ec;
		auto time = std::filesystem::last_write_time(path, ec);
		if (ec)
			return 0;
		return (uint64_t)time.time_since_epoch().count();
	}

	Ref<Mesh> MeshCache::TryLoad(const std::filesystem::path& sourceRelativePath,
		const std::filesystem::path& sourceFullPath)
	{
		std::filesystem::path cachePath = GetCachePath(sourceRelativePath);
		if (!std::filesystem::exists(cachePath))
			return nullptr;

		uint64_t sourceTimestamp = GetFileTimestamp(sourceFullPath);
		if (sourceTimestamp == 0)
			return nullptr;

		std::ifstream in(cachePath, std::ios::binary);
		if (!in)
			return nullptr;

		uint32_t magic = 0, version = 0;
		uint64_t cachedTimestamp = 0;
		ReadValue(in, magic);
		ReadValue(in, version);
		ReadValue(in, cachedTimestamp);

		if (magic != MESH_CACHE_MAGIC || version != MESH_CACHE_VERSION || cachedTimestamp != sourceTimestamp)
			return nullptr;

		std::string storedPath = ReadString(in);
		if (storedPath != sourceRelativePath.generic_string())
			return nullptr;

		std::vector<MeshVertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Submesh> submeshes;

		ReadVector(in, vertices);
		ReadIndices(in, indices);
		ReadSubmeshes(in, submeshes);
		std::vector<Ref<Material>> materials = ReadMaterials(in);

		if (vertices.empty() || indices.empty())
			return nullptr;

		Ref<Mesh> mesh = Mesh::Create(vertices, indices, submeshes, materials);
		mesh->SetPath(sourceRelativePath.generic_string());
		GE_CORE_INFO("Loaded mesh cache '{0}'", cachePath.filename().string());
		return mesh;
	}

	bool MeshCache::Write(const Ref<Mesh>& mesh, const std::filesystem::path& sourceRelativePath,
		const std::filesystem::path& sourceFullPath)
	{
		if (!mesh)
			return false;

		uint64_t sourceTimestamp = GetFileTimestamp(sourceFullPath);
		if (sourceTimestamp == 0)
			return false;

		std::filesystem::path cachePath = GetCachePath(sourceRelativePath);
		std::error_code ec;
		std::filesystem::create_directories(cachePath.parent_path(), ec);

		std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;

		WriteValue(out, MESH_CACHE_MAGIC);
		WriteValue(out, MESH_CACHE_VERSION);
		WriteValue(out, sourceTimestamp);
		WriteString(out, sourceRelativePath.generic_string());
		WriteVector(out, mesh->GetVertices());
		WriteIndices(out, mesh->GetIndices());
		WriteSubmeshes(out, mesh->GetSubmeshes());
		WriteMaterials(out, mesh->GetMaterials());

		GE_CORE_INFO("Wrote mesh cache '{0}'", cachePath.filename().string());
		return true;
	}

}
