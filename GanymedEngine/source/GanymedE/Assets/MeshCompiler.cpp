#include "gepch.h"
#include "GanymedE/Renderer/MeshShader.h"
#include "MeshCompiler.h"

#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/TextureImporter.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/MeshImporter.h"
#include "GanymedE/Renderer/Shader.h"
#include "GanymedE/Renderer/Texture.h"

#include <sstream>

namespace GanymedE {

	namespace {

		constexpr uint32_t MESH_CACHE_MAGIC = 0x48434D47; // 'GMCH'
		// v5 and v6 have v4's layout; both bumps discard caches whose values are stale
		// rather than whose fields moved. v5: Skeleton::RootTransform written before it
		// accounted for the skinned mesh node's transform. v6: skinned submeshes written
		// with an identity LocalTransform, which drops that node transform entirely.
		// v7 drops the embedded source timestamp: the `.dep` epoch record decides staleness
		// now, and a blob that carried its own answer to that question could disagree with it.
		//
		// This has to stay in step with MeshCompiler::Version(), which is what CompiledCache
		// compares - the magic and version below are a second line of defence against a stale
		// file, not the primary one.
		constexpr uint32_t MESH_CACHE_VERSION = 7;

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

		// Trivially copyable element types only - the arrays are written as one blob.
		template<typename T>
		void WriteArray(std::ostream& out, const std::vector<T>& values)
		{
			static_assert(std::is_trivially_copyable<T>::value, "WriteArray writes a raw blob");
			uint32_t count = (uint32_t)values.size();
			WriteValue(out, count);
			if (count > 0)
				out.write(reinterpret_cast<const char*>(values.data()), count * sizeof(T));
		}

		template<typename T>
		void ReadArray(std::istream& in, std::vector<T>& values)
		{
			static_assert(std::is_trivially_copyable<T>::value, "ReadArray reads a raw blob");
			uint32_t count = 0;
			ReadValue(in, count);
			values.resize(count);
			if (count > 0)
				in.read(reinterpret_cast<char*>(values.data()), count * sizeof(T));
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
				WriteValue(out, submesh.IsSkinned);
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
				ReadValue(in, submesh.IsSkinned);
				submesh.Name = ReadString(in);
			}
		}

		void WriteSkeleton(std::ostream& out, const Skeleton& skeleton)
		{
			WriteArray(out, skeleton.ParentIndices);
			WriteArray(out, skeleton.InverseBind);
			WriteArray(out, skeleton.LocalRestPose);
			WriteValue(out, skeleton.RootTransform);

			WriteValue(out, (uint32_t)skeleton.JointNames.size());
			for (const std::string& name : skeleton.JointNames)
				WriteString(out, name);
		}

		Skeleton ReadSkeleton(std::istream& in)
		{
			Skeleton skeleton;
			ReadArray(in, skeleton.ParentIndices);
			ReadArray(in, skeleton.InverseBind);
			ReadArray(in, skeleton.LocalRestPose);
			ReadValue(in, skeleton.RootTransform);

			uint32_t nameCount = 0;
			ReadValue(in, nameCount);
			skeleton.JointNames.resize(nameCount);
			for (std::string& name : skeleton.JointNames)
				name = ReadString(in);

			return skeleton;
		}

		void WriteClips(std::ostream& out, const std::vector<AnimationClip>& clips)
		{
			WriteValue(out, (uint32_t)clips.size());
			for (const AnimationClip& clip : clips)
			{
				WriteString(out, clip.Name);
				WriteValue(out, clip.Duration);

				WriteValue(out, (uint32_t)clip.Channels.size());
				for (const AnimationClip::Channel& channel : clip.Channels)
				{
					WriteValue(out, channel.Joint);
					WriteValue(out, channel.Target);
					WriteValue(out, channel.Mode);
					WriteArray(out, channel.Times);
					WriteArray(out, channel.Values);
				}
			}
		}

		std::vector<AnimationClip> ReadClips(std::istream& in)
		{
			uint32_t clipCount = 0;
			ReadValue(in, clipCount);

			std::vector<AnimationClip> clips(clipCount);
			for (AnimationClip& clip : clips)
			{
				clip.Name = ReadString(in);
				ReadValue(in, clip.Duration);

				uint32_t channelCount = 0;
				ReadValue(in, channelCount);
				clip.Channels.resize(channelCount);
				for (AnimationClip::Channel& channel : clip.Channels)
				{
					ReadValue(in, channel.Joint);
					ReadValue(in, channel.Target);
					ReadValue(in, channel.Mode);
					ReadArray(in, channel.Times);
					ReadArray(in, channel.Values);
				}
			}

			return clips;
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

				// Same resolve the cold-import path uses, so a cache load shares the
				// manager's texture cache instead of decoding its own copies.
				if (!albedoPath.empty())
					material->SetAlbedoMap(TextureImporter::LoadMaterialMap(albedoPath));
				else
					material->SetAlbedoMap(TextureImporter::LoadFromMemory(albedoEmbedded.data(), albedoEmbedded.size(), true));

				if (!normalPath.empty())
					material->SetNormalMap(TextureImporter::LoadMaterialMap(normalPath));
				else
					material->SetNormalMap(TextureImporter::LoadFromMemory(normalEmbedded.data(), normalEmbedded.size(), true));

				if (!mrPath.empty())
					material->SetMetallicRoughnessMap(TextureImporter::LoadMaterialMap(mrPath));
				else
					material->SetMetallicRoughnessMap(TextureImporter::LoadFromMemory(mrEmbedded.data(), mrEmbedded.size(), true));

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

	bool MeshCompiler::Compile(const CompileInput& input, CompileOutput& output) const
	{
		GE_PROFILE_FUNCTION();

		// See the note on the class: this reaches the GPU, which the compiler contract says it
		// should not. It is what the importer can do today.
		Ref<Mesh> mesh = MeshImporter::Load(input.SourceFullPath, &output.Dependencies);
		if (!mesh)
			return false;

		const std::string relativePath = input.Metadata->FilePath;

		std::ostringstream out(std::ios::binary);
		WriteValue(out, MESH_CACHE_MAGIC);
		WriteValue(out, MESH_CACHE_VERSION);
		WriteString(out, relativePath);
		WriteArray(out, mesh->GetVertices());
		WriteArray(out, mesh->GetIndices());
		WriteSubmeshes(out, mesh->GetSubmeshes());
		WriteMaterials(out, mesh->GetMaterials());
		WriteArray(out, mesh->GetSkinVertices());
		WriteSkeleton(out, mesh->GetSkeleton());
		WriteClips(out, mesh->GetClips());

		const std::string blob = out.str();
		output.Bytes.assign(blob.begin(), blob.end());
		return !output.Bytes.empty();
	}

	Ref<Mesh> MeshCompiler::Read(const std::vector<uint8_t>& blob,
		const std::filesystem::path& sourceRelativePath)
	{
		GE_PROFILE_FUNCTION();

		std::istringstream in(std::string(blob.begin(), blob.end()), std::ios::binary);

		uint32_t magic = 0, version = 0;
		ReadValue(in, magic);
		ReadValue(in, version);

		if (magic != MESH_CACHE_MAGIC || version != MESH_CACHE_VERSION)
		{
			GE_CORE_WARN("Mesh blob for '{0}' is not v{1} - the compiled output is stale in a way "
				"the epoch record did not catch", sourceRelativePath.generic_string(), MESH_CACHE_VERSION);
			return nullptr;
		}

		// Written and checked because the output tree is keyed by a hash of this path: a
		// collision would otherwise hand back a completely unrelated mesh.
		std::string storedPath = ReadString(in);
		if (storedPath != sourceRelativePath.generic_string())
		{
			GE_CORE_ERROR("Mesh blob claims to be '{0}' but was opened for '{1}'",
				storedPath, sourceRelativePath.generic_string());
			return nullptr;
		}

		std::vector<MeshVertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Submesh> submeshes;

		ReadArray(in, vertices);
		ReadArray(in, indices);
		ReadSubmeshes(in, submeshes);
		std::vector<Ref<Material>> materials = ReadMaterials(in);

		std::vector<SkinVertex> skinVertices;
		ReadArray(in, skinVertices);
		Skeleton skeleton = ReadSkeleton(in);
		std::vector<AnimationClip> clips = ReadClips(in);

		if (vertices.empty() || indices.empty())
			return nullptr;

		Ref<Mesh> mesh = Mesh::Create(vertices, indices, submeshes, materials,
			std::move(skinVertices), std::move(skeleton), std::move(clips));
		mesh->SetPath(sourceRelativePath.generic_string());
		return mesh;
	}

}
