#include "gepch.h"
#include "GanymedE/Assets/MaterialSerializer.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/TextureImporter.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/MeshShader.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

namespace GanymedE {

	namespace {

		YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec4& v)
		{
			out << YAML::Flow;
			out << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
			return out;
		}

		glm::vec4 ReadVec4(const YAML::Node& node, const glm::vec4& fallback)
		{
			if (!node || !node.IsSequence() || node.size() != 4)
				return fallback;

			return { node[0].as<float>(), node[1].as<float>(),
					 node[2].as<float>(), node[3].as<float>() };
		}

		// Filesystem-safe, and stable across platforms: anything outside [A-Za-z0-9_-] becomes
		// an underscore. glTF names routinely carry spaces, slashes and non-ASCII.
		std::string Sanitize(const std::string& name)
		{
			std::string out;
			out.reserve(name.size());

			for (char c : name)
			{
				const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
					|| (c >= '0' && c <= '9') || c == '_' || c == '-';
				out += safe ? c : '_';
			}

			if (out.empty())
				out = "Material";

			// Long enough to stay readable, short enough that a deep asset directory plus this
			// stays inside MAX_PATH.
			if (out.size() > 48)
				out.resize(48);

			return out;
		}

		// The embedded bytes are already a compressed image (that is how glTF stores them), so
		// extraction is a byte copy - no encoder involved. The extension has to match what is
		// actually in there, though: writing JPEG bytes into a .png would still *decode*,
		// because stb sniffs content, but the registry types assets by extension and the file
		// would be a lie on disk.
		const char* ImageExtension(const std::vector<uint8_t>& bytes)
		{
			if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')
				return ".png";
			if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
				return ".jpg";

			return nullptr;
		}

		// Writes `bytes` to `<meshdir>/<meshstem>_textures/<map>_<index>.<ext>` unless that file
		// already exists, and returns the asset-relative path to reference from the `.gmat`.
		// Empty when there was nothing to extract.
		std::string ExtractEmbedded(const std::vector<uint8_t>& bytes,
			const std::filesystem::path& meshRelativePath, const char* mapName, uint32_t materialIndex)
		{
			if (bytes.empty())
				return {};

			const char* extension = ImageExtension(bytes);
			if (!extension)
			{
				GE_CORE_WARN("Embedded {0} map of '{1}' material {2} is not PNG or JPEG - not extracted",
					mapName, meshRelativePath.generic_string(), materialIndex);
				return {};
			}

			const std::filesystem::path directory = meshRelativePath.parent_path()
				/ (meshRelativePath.stem().string() + "_textures");
			const std::filesystem::path relativePath = directory
				/ (std::string(mapName) + "_" + std::to_string(materialIndex) + extension);

			const std::filesystem::path fullPath = GetAssetRoot() / relativePath;
			if (std::filesystem::exists(fullPath))
				return relativePath.generic_string();   // already extracted; leave it alone

			std::error_code ec;
			std::filesystem::create_directories(GetAssetRoot() / directory, ec);

			std::ofstream out(fullPath, std::ios::binary);
			if (!out)
			{
				GE_CORE_WARN("Could not write extracted texture '{0}'", fullPath.string());
				return {};
			}

			out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
			out.close();

			GE_CORE_INFO("Extracted embedded {0} map to '{1}'", mapName, relativePath.generic_string());
			return relativePath.generic_string();
		}

	}

	namespace MaterialSerializer {

		Ref<Material> Load(const std::filesystem::path& fullPath)
		{
			if (!std::filesystem::exists(fullPath))
			{
				GE_CORE_WARN("Material '{0}' does not exist", fullPath.string());
				return nullptr;
			}

			YAML::Node root;
			try
			{
				std::ifstream stream(fullPath);
				std::stringstream buffer;
				buffer << stream.rdbuf();
				root = YAML::Load(buffer.str());
			}
			catch (const YAML::Exception& e)
			{
				// The SceneSerializer posture: malformed content is reported, not fatal.
				GE_CORE_ERROR("Material '{0}' failed to parse: {1}", fullPath.string(), e.what());
				return nullptr;
			}

			YAML::Node node = root["Material"];
			if (!node)
			{
				GE_CORE_ERROR("Material '{0}' has no Material node", fullPath.string());
				return nullptr;
			}

			// MeshShader::Get() is injected rather than named in the file: Material::Bind asserts
			// on a null shader, and there is nothing else to choose until shader variants exist.
			// This is the MeshCache::ReadMaterials precedent.
			Ref<Material> material = Material::Create(MeshShader::Get());

			try
			{
				if (node["Name"])
					material->SetName(node["Name"].as<std::string>());

				material->SetAlbedoColor(ReadVec4(node["Albedo"], glm::vec4(1.0f)));

				if (node["Metallic"])
					material->SetMetallic(node["Metallic"].as<float>());
				if (node["Roughness"])
					material->SetRoughness(node["Roughness"].as<float>());
				if (node["TwoSided"])
					material->SetTwoSided(node["TwoSided"].as<bool>());
				if (node["Transparent"])
					material->SetTransparent(node["Transparent"].as<bool>());

				// Maps are stored as asset-root-relative *paths*, not handles. A `.gmat` has to
				// be self-describing and hand-mergeable: a bare handle means nothing without the
				// registry that minted it, while a path survives a fresh clone. Handles stay the
				// scene-to-registry currency; paths are the asset-to-asset one.
				struct MapField
				{
					const char* Key;
					void (Material::*SetPath)(const std::string&);
					void (Material::*SetTexture)(const Ref<Texture2D>&);
				};

				const MapField maps[] = {
					{ "AlbedoMap",            &Material::SetAlbedoMapPath,            &Material::SetAlbedoMap },
					{ "NormalMap",            &Material::SetNormalMapPath,            &Material::SetNormalMap },
					{ "MetallicRoughnessMap", &Material::SetMetallicRoughnessMapPath, &Material::SetMetallicRoughnessMap },
				};

				for (const MapField& map : maps)
				{
					if (!node[map.Key])
						continue;

					const std::string path = node[map.Key].as<std::string>();
					if (path.empty())
						continue;

					(material.get()->*map.SetPath)(path);
					// De-duplicated through the registry, so two materials naming one image
					// share a single bgfx texture.
					(material.get()->*map.SetTexture)(TextureImporter::LoadMaterialMap(path));
				}
			}
			catch (const YAML::Exception& e)
			{
				GE_CORE_ERROR("Material '{0}' has a malformed field: {1} - loading what was read",
					fullPath.string(), e.what());
			}

			return material;
		}

		bool Save(const Ref<Material>& material, const std::filesystem::path& fullPath)
		{
			if (!material)
				return false;

			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "Material" << YAML::Value << YAML::BeginMap;

			// Fixed order. Reordering these lines rewrites every `.gmat` in every project.
			out << YAML::Key << "Name" << YAML::Value << material->GetName();
			out << YAML::Key << "Albedo" << YAML::Value << material->GetAlbedoColor();
			out << YAML::Key << "Metallic" << YAML::Value << material->GetMetallic();
			out << YAML::Key << "Roughness" << YAML::Value << material->GetRoughness();

			// Omitted rather than written empty: an empty scalar reads back as a null node and
			// as<std::string>() throws on those (the AnimatorComponent::Clip lesson).
			if (!material->GetAlbedoMapPath().empty())
				out << YAML::Key << "AlbedoMap" << YAML::Value << material->GetAlbedoMapPath();
			if (!material->GetNormalMapPath().empty())
				out << YAML::Key << "NormalMap" << YAML::Value << material->GetNormalMapPath();
			if (!material->GetMetallicRoughnessMapPath().empty())
				out << YAML::Key << "MetallicRoughnessMap" << YAML::Value << material->GetMetallicRoughnessMapPath();

			out << YAML::Key << "TwoSided" << YAML::Value << material->IsTwoSided();
			out << YAML::Key << "Transparent" << YAML::Value << material->IsTransparent();

			out << YAML::EndMap;
			out << YAML::EndMap;

			std::error_code ec;
			std::filesystem::create_directories(fullPath.parent_path(), ec);

			std::ofstream fout(fullPath);
			if (!fout)
			{
				GE_CORE_ERROR("Could not open '{0}' for writing", fullPath.string());
				return false;
			}

			fout << out.c_str();
			return true;
		}

		std::filesystem::path SidecarPath(const std::filesystem::path& meshRelativePath,
			uint32_t materialIndex, const std::string& materialName)
		{
			const std::string filename = meshRelativePath.stem().string()
				+ "_mat" + std::to_string(materialIndex)
				+ "_" + Sanitize(materialName) + ".gmat";

			return meshRelativePath.parent_path() / filename;
		}

		void GenerateSidecars(const Ref<Mesh>& mesh, const std::filesystem::path& meshRelativePath)
		{
			// The runtime never writes into assets/. One flag, one meaning, shared with the
			// registry writer - see docs/engine/assets.md.
			if (!mesh || !AssetManager::IsRegistryWritable())
				return;

			const auto& materials = mesh->GetMaterials();
			for (uint32_t i = 0; i < (uint32_t)materials.size(); i++)
			{
				const Ref<Material>& material = materials[i];
				if (!material)
					continue;

				const std::filesystem::path relativePath = SidecarPath(meshRelativePath, i, material->GetName());
				const std::filesystem::path fullPath = GetAssetRoot() / relativePath;

				// Registered either way: the file may already exist from a previous session, and
				// the entity that instantiates this mesh needs a handle for it regardless.
				const bool exists = std::filesystem::exists(fullPath);

				if (!exists)
				{
					// Extraction happens only on the write path, so a mesh whose sidecars are
					// already authored never re-touches its texture directory.
					//
					// Extracting is what makes the .gmat self-describing: a material that
					// referenced bytes living inside another asset's blob could be neither
					// hand-edited nor re-pointed at a different image.
					const std::string albedo = ExtractEmbedded(material->GetAlbedoMapEmbeddedData(),
						meshRelativePath, "albedo", i);
					const std::string normal = ExtractEmbedded(material->GetNormalMapEmbeddedData(),
						meshRelativePath, "normal", i);
					const std::string metallicRoughness = ExtractEmbedded(
						material->GetMetallicRoughnessMapEmbeddedData(),
						meshRelativePath, "metallicRoughness", i);

					// The sidecar is a *copy* of the imported material with its embedded maps
					// swapped for the files just written. The mesh's own material is left
					// untouched - .gmat is an additive layer, not a replacement.
					Ref<Material> sidecar = Material::Create(MeshShader::Get());
					sidecar->SetName(material->GetName());
					sidecar->SetAlbedoColor(material->GetAlbedoColor());
					sidecar->SetMetallic(material->GetMetallic());
					sidecar->SetRoughness(material->GetRoughness());
					sidecar->SetTwoSided(material->IsTwoSided());
					sidecar->SetTransparent(material->IsTransparent());
					sidecar->SetAlbedoMapPath(albedo.empty() ? material->GetAlbedoMapPath() : albedo);
					sidecar->SetNormalMapPath(normal.empty() ? material->GetNormalMapPath() : normal);
					sidecar->SetMetallicRoughnessMapPath(metallicRoughness.empty()
						? material->GetMetallicRoughnessMapPath() : metallicRoughness);

					if (Save(sidecar, fullPath))
						GE_CORE_INFO("Generated material sidecar '{0}'", relativePath.generic_string());

					// Extracted images are assets like any other.
					for (const std::string* path : { &albedo, &normal, &metallicRoughness })
					{
						if (!path->empty())
							AssetManager::ImportAsset(*path);
					}
				}

				AssetManager::ImportAsset(relativePath);
			}
		}

	}

}
