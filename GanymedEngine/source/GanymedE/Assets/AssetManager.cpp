#include "gepch.h"
#include "AssetManager.h"

#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/MaterialSerializer.h"
#include "GanymedE/Assets/MeshCache.h"
#include "GanymedE/Assets/TextureImporter.h"
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/MeshImporter.h"
#include "GanymedE/Renderer/Texture.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

namespace GanymedE {

	struct AssetManagerData
	{
		std::unordered_map<AssetHandle, AssetMetadata> Registry;
		std::unordered_map<std::string, AssetHandle> PathToHandle;

		std::unordered_map<AssetHandle, Ref<Mesh>> LoadedMeshes;
		std::unordered_map<AssetHandle, Ref<Environment>> LoadedEnvironments;
		std::unordered_map<AssetHandle, Ref<Texture2D>> LoadedTextures;
		std::unordered_map<AssetHandle, Ref<Material>> LoadedMaterials;

		bool Initialized = false;
		bool WritableRegistry = true;

		// Set by ImportAsset, cleared by a successful write. See FlushRegistry.
		bool RegistryDirty = false;

		// Handles already reported as unknown. RenderSystem re-fetches assets by handle
		// every frame per entity, so an unguarded warning here would arrive at frame
		// rate; once per handle is the AnimationSystem unknown-clip posture - loud, not
		// broken.
		std::unordered_set<AssetHandle> WarnedUnknownHandles;
	};

	static AssetManagerData s_Data;

	namespace {

		// A valid handle with no registry entry means the scene references an asset this
		// install does not know about - almost always a missing or stale AssetRegistry.gr.
		// Worth saying out loud: the consequence is an entity that renders nothing, which
		// is indistinguishable from a bad transform or an unlit material until you know.
		void WarnUnknownHandle(AssetHandle handle, const char* expected)
		{
			if (!s_Data.WarnedUnknownHandles.insert(handle).second)
				return;

			GE_CORE_WARN("Asset handle {0} is not in the registry (expected: {1}) - nothing "
				"will be loaded for it. A missing or stale assets/AssetRegistry.gr is the "
				"usual cause.", static_cast<uint64_t>(handle), expected);
		}

	}

	void AssetManager::Init(bool writableRegistry)
	{
		if (s_Data.Initialized)
			return;

		s_Data.WritableRegistry = writableRegistry;

		LoadRegistry();
		s_Data.Initialized = true;
		GE_CORE_INFO("AssetManager initialized ({0} registered assets, registry {1})",
			s_Data.Registry.size(), writableRegistry ? "writable" : "read-only");
	}

	void AssetManager::Shutdown()
	{
		if (!s_Data.Initialized)
			return;

		FlushRegistry();

		s_Data.Registry.clear();
		s_Data.PathToHandle.clear();
		s_Data.WarnedUnknownHandles.clear();
		// Runs from EditorLayer::OnDetach while Renderer::IsGpuAlive() is still true,
		// so the GPU-resource destructors release real bgfx handles rather than
		// tripping the is-alive guard.
		s_Data.LoadedMeshes.clear();
		s_Data.LoadedEnvironments.clear();
		s_Data.LoadedTextures.clear();
		s_Data.LoadedMaterials.clear();
		s_Data.Initialized = false;
	}

	AssetHandle AssetManager::ImportAsset(const std::filesystem::path& relativePath)
	{
		std::string pathKey = relativePath.generic_string();
		auto existing = s_Data.PathToHandle.find(pathKey);
		if (existing != s_Data.PathToHandle.end())
			return existing->second;

		AssetType type = AssetTypeFromExtension(relativePath.extension().string());
		if (type == AssetType::None)
		{
			GE_CORE_WARN("Unsupported asset type for '{0}'", pathKey);
			return InvalidAssetHandle;
		}

		AssetMetadata metadata;
		metadata.Handle = UUID();
		metadata.Type = type;
		metadata.FilePath = pathKey;

		s_Data.Registry[metadata.Handle] = metadata;
		s_Data.PathToHandle[pathKey] = metadata.Handle;

		GE_CORE_INFO("Imported asset '{0}' as {1} (handle {2})",
			pathKey, AssetTypeToString(type), static_cast<uint64_t>(metadata.Handle));

		s_Data.RegistryDirty = true;
		return metadata.Handle;
	}

	AssetHandle AssetManager::GetHandle(const std::filesystem::path& relativePath)
	{
		std::string pathKey = relativePath.generic_string();
		auto it = s_Data.PathToHandle.find(pathKey);
		if (it != s_Data.PathToHandle.end())
			return it->second;
		return InvalidAssetHandle;
	}

	const AssetMetadata* AssetManager::GetMetadata(AssetHandle handle)
	{
		auto it = s_Data.Registry.find(handle);
		if (it == s_Data.Registry.end())
			return nullptr;
		return &it->second;
	}

	AssetType AssetManager::GetAssetType(AssetHandle handle)
	{
		const AssetMetadata* metadata = GetMetadata(handle);
		return metadata ? metadata->Type : AssetType::None;
	}

	Ref<Mesh> AssetManager::LoadMesh(AssetHandle handle)
	{
		if (!IsAssetHandleValid(handle))
			return nullptr;

		auto cached = s_Data.LoadedMeshes.find(handle);
		if (cached != s_Data.LoadedMeshes.end())
			return cached->second;

		const AssetMetadata* metadata = GetMetadata(handle);
		if (!metadata)
		{
			WarnUnknownHandle(handle, "static mesh");
			return nullptr;
		}
		if (metadata->Type != AssetType::StaticMesh)
			return nullptr;

		std::filesystem::path relativePath = metadata->FilePath;
		std::filesystem::path fullPath = GetAssetRoot() / relativePath;

		Ref<Mesh> mesh = MeshCache::TryLoad(relativePath, fullPath);
		if (!mesh)
		{
			mesh = MeshImporter::Load(fullPath);
			if (mesh)
				MeshCache::Write(mesh, relativePath, fullPath);
		}

		if (mesh)
		{
			s_Data.LoadedMeshes[handle] = mesh;

			// Here rather than inside MeshImporter, because this is the one point both the cold
			// import and the cache replay pass through - a cached mesh must still get its
			// sidecars, or deleting .meshcache would be the only way to regenerate them.
			MaterialSerializer::GenerateSidecars(mesh, relativePath);
		}

		return mesh;
	}

	// Cached, not path-resolved (the Script/Audio pattern), and the caching is load-bearing
	// rather than a performance choice: instancing merges draws on Ref<Material> *identity*, so
	// two entities sharing one .gmat handle must receive the same Ref or every batch shatters.
	Ref<Material> AssetManager::LoadMaterial(AssetHandle handle)
	{
		if (!IsAssetHandleValid(handle))
			return nullptr;

		auto cached = s_Data.LoadedMaterials.find(handle);
		if (cached != s_Data.LoadedMaterials.end())
			return cached->second;

		const AssetMetadata* metadata = GetMetadata(handle);
		if (!metadata)
		{
			WarnUnknownHandle(handle, "material");
			return nullptr;
		}
		if (metadata->Type != AssetType::Material)
			return nullptr;

		Ref<Material> material = MaterialSerializer::Load(GetAssetRoot() / metadata->FilePath);
		if (material)
			s_Data.LoadedMaterials[handle] = material;

		return material;
	}

	Ref<Environment> AssetManager::LoadEnvironment(AssetHandle handle)
	{
		if (!IsAssetHandleValid(handle))
			return nullptr;

		auto cached = s_Data.LoadedEnvironments.find(handle);
		if (cached != s_Data.LoadedEnvironments.end())
			return cached->second;

		const AssetMetadata* metadata = GetMetadata(handle);
		if (!metadata)
		{
			WarnUnknownHandle(handle, "environment");
			return nullptr;
		}
		if (metadata->Type != AssetType::Environment)
			return nullptr;

		std::filesystem::path fullPath = GetAssetRoot() / metadata->FilePath;
		Ref<Environment> environment = Environment::Create(fullPath.string());
		if (environment)
			s_Data.LoadedEnvironments[handle] = environment;

		return environment;
	}

	Ref<Texture2D> AssetManager::LoadTexture(AssetHandle handle)
	{
		if (!IsAssetHandleValid(handle))
			return nullptr;

		// Plain map lookup on the hit path: RenderSystem re-fetches assets by handle
		// every frame per entity, and texture consumers may end up there too.
		auto cached = s_Data.LoadedTextures.find(handle);
		if (cached != s_Data.LoadedTextures.end())
		{
			GE_CORE_TRACE("Texture cache hit (handle {0})", static_cast<uint64_t>(handle));
			return cached->second;
		}

		const AssetMetadata* metadata = GetMetadata(handle);
		if (!metadata)
		{
			WarnUnknownHandle(handle, "texture");
			return nullptr;
		}
		if (metadata->Type != AssetType::Texture)
			return nullptr;

		std::filesystem::path fullPath = GetAssetRoot() / metadata->FilePath;
		Ref<Texture2D> texture = TextureImporter::LoadFromFile(fullPath, false);
		if (texture)
			s_Data.LoadedTextures[handle] = texture;

		return texture;
	}

	template<>
	Ref<Mesh> AssetManager::GetAsset<Mesh>(AssetHandle handle)
	{
		return LoadMesh(handle);
	}

	template<>
	Ref<Environment> AssetManager::GetAsset<Environment>(AssetHandle handle)
	{
		return LoadEnvironment(handle);
	}

	template<>
	Ref<Texture2D> AssetManager::GetAsset<Texture2D>(AssetHandle handle)
	{
		return LoadTexture(handle);
	}

	template<>
	Ref<Material> AssetManager::GetAsset<Material>(AssetHandle handle)
	{
		return LoadMaterial(handle);
	}

	void AssetManager::Reload(AssetHandle handle)
	{
		const AssetMetadata* metadata = GetMetadata(handle);
		if (!metadata)
			return;

		switch (metadata->Type)
		{
			case AssetType::Texture:
				s_Data.LoadedTextures.erase(handle);
				break;

			case AssetType::Environment:
				s_Data.LoadedEnvironments.erase(handle);
				break;

			case AssetType::Material:
			{
				// Textures first, for the same reason the mesh branch does it: the reloaded
				// material would otherwise rebind the stale cached ones through LoadMaterialMap.
				auto cached = s_Data.LoadedMaterials.find(handle);
				if (cached != s_Data.LoadedMaterials.end() && cached->second)
				{
					for (const std::string* mapPath : {
						&cached->second->GetAlbedoMapPath(),
						&cached->second->GetNormalMapPath(),
						&cached->second->GetMetallicRoughnessMapPath() })
					{
						if (mapPath->empty())
							continue;

						AssetHandle textureHandle = GetHandle(*mapPath);
						if (IsAssetHandleValid(textureHandle))
							s_Data.LoadedTextures.erase(textureHandle);
					}
				}

				s_Data.LoadedMaterials.erase(handle);
				break;
			}

			case AssetType::StaticMesh:
			{
				// Order matters: the mesh's textures have to go first, or the reimported
				// mesh rebinds the stale cached ones through LoadMaterialMap.
				auto cached = s_Data.LoadedMeshes.find(handle);
				if (cached != s_Data.LoadedMeshes.end() && cached->second)
				{
					for (const auto& material : cached->second->GetMaterials())
					{
						if (!material)
							continue;

						for (const std::string* mapPath : {
							&material->GetAlbedoMapPath(),
							&material->GetNormalMapPath(),
							&material->GetMetallicRoughnessMapPath() })
						{
							if (mapPath->empty())
								continue;

							AssetHandle textureHandle = GetHandle(*mapPath);
							if (IsAssetHandleValid(textureHandle))
								s_Data.LoadedTextures.erase(textureHandle);
						}
					}
				}

				s_Data.LoadedMeshes.erase(handle);
				MeshCache::Invalidate(metadata->FilePath);
				break;
			}

			default:
				// Scene/Script/Audio have no GetAsset cache to evict.
				return;
		}

		GE_CORE_INFO("Reloading asset '{0}'", metadata->FilePath);
	}

	void AssetManager::LoadRegistry()
	{
		std::filesystem::path registryPath = GetAssetRoot() / "AssetRegistry.gr";
		if (!std::filesystem::exists(registryPath))
			return;

		std::stringstream buffer;
		{
			// Scoped so the handle is closed before the catch below can rename the file:
			// Windows refuses to rename a file that is still open.
			std::ifstream stream(registryPath);
			if (!stream)
				return;

			buffer << stream.rdbuf();
		}

		YAML::Node assets;
		try
		{
			// Same posture as SceneSerializer::Deserialize: a malformed registry is content
			// to report, not a reason to take the process down. Unhandled, a truncated .gr
			// terminated the process out of Init - before a window existed to say why.
			YAML::Node root = YAML::Load(buffer.str());
			assets = root["Assets"];
		}
		catch (const YAML::Exception& e)
		{
			// Move the unreadable file aside rather than leave it in place. The first
			// import of the session would otherwise flush a nearly-empty registry over
			// it, and the handle mappings a hand-repair could have recovered are gone -
			// every scene in the project pointing at assets nothing can resolve.
			std::filesystem::path quarantine = registryPath;
			quarantine += ".bad";

			std::error_code ec;
			std::filesystem::rename(registryPath, quarantine, ec);

			GE_CORE_ERROR("Asset registry '{0}' failed to parse: {1} - continuing with an "
				"empty registry. Assets referenced by handle will not resolve until it is "
				"repaired or regenerated; the unreadable file was kept as '{2}'{3}.",
				registryPath.string(), e.what(), quarantine.string(),
				ec ? " (rename failed - it will be overwritten by the next import)" : "");
			return;
		}

		if (!assets)
			return;

		for (auto assetNode : assets)
		{
			if (!assetNode["Handle"] || !assetNode["Type"] || !assetNode["FilePath"])
				continue;

			AssetMetadata metadata;
			metadata.Handle = assetNode["Handle"].as<uint64_t>();
			metadata.Type = (AssetType)assetNode["Type"].as<uint16_t>();
			metadata.FilePath = assetNode["FilePath"].as<std::string>();

			// Two handles for one path means one of them is unreachable through
			// GetHandle/ImportAsset - the losing entry is dead weight that only ever
			// surfaces as an asset that silently fails to resolve. Keep the first and
			// say so; do not drop the entry, since a scene may still reference it.
			auto existing = s_Data.PathToHandle.find(metadata.FilePath);
			if (existing != s_Data.PathToHandle.end())
			{
				GE_CORE_WARN("Asset registry has duplicate entries for '{0}' (handles {1} and "
					"{2}); keeping {1}", metadata.FilePath,
					static_cast<uint64_t>(existing->second), static_cast<uint64_t>(metadata.Handle));
				s_Data.Registry[metadata.Handle] = metadata;
				continue;
			}

			s_Data.Registry[metadata.Handle] = metadata;
			s_Data.PathToHandle[metadata.FilePath] = metadata.Handle;
		}
	}

	void AssetManager::SaveRegistry()
	{
		// Guarded here rather than at the call sites so no future caller can bypass it.
		// FlushRegistry saves too, and it runs at the end of scene deserialization for
		// path-based components - which is how a read-only install would otherwise have
		// written its registry long before Shutdown ever asked.
		if (!s_Data.WritableRegistry)
			return;

		std::filesystem::path registryPath = GetAssetRoot() / "AssetRegistry.gr";
		std::error_code ec;
		std::filesystem::create_directories(registryPath.parent_path(), ec);

		// Sorted by path, not by the map's iteration order: Registry is keyed on a random
		// uint64 under an identity hash, so a rehash reorders every entry and rewrites the
		// whole file for one import. Sorted output makes the file a stable diff and makes
		// "save twice, compare" a usable check.
		std::vector<const AssetMetadata*> entries;
		entries.reserve(s_Data.Registry.size());
		for (const auto& [handle, metadata] : s_Data.Registry)
			entries.push_back(&metadata);

		std::sort(entries.begin(), entries.end(),
			[](const AssetMetadata* a, const AssetMetadata* b)
			{
				if (a->FilePath != b->FilePath)
					return a->FilePath < b->FilePath;
				// duplicate paths still order deterministically (AssetHandle has no operator<)
				return static_cast<uint64_t>(a->Handle) < static_cast<uint64_t>(b->Handle);
			});

		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Assets" << YAML::Value << YAML::BeginSeq;

		for (const AssetMetadata* metadata : entries)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Handle" << YAML::Value << static_cast<uint64_t>(metadata->Handle);
			out << YAML::Key << "Type" << YAML::Value << (uint16_t)metadata->Type;
			out << YAML::Key << "FilePath" << YAML::Value << metadata->FilePath;
			out << YAML::EndMap;
		}

		out << YAML::EndSeq;
		out << YAML::EndMap;

		// Write-then-rename. std::ofstream truncates on open, so a crash between the open
		// and the flush used to leave a zero-length registry - every handle in every scene
		// dead, with no way to tell that from "never imported anything". The rename is
		// atomic on both NTFS and POSIX, so the file is either the old one or the new one.
		std::filesystem::path tempPath = registryPath;
		tempPath += ".tmp";

		{
			std::ofstream fout(tempPath);
			if (!fout)
			{
				GE_CORE_ERROR("Could not open '{0}' for writing - asset registry not saved",
					tempPath.string());
				return;
			}

			fout << out.c_str();
		}

		std::filesystem::rename(tempPath, registryPath, ec);
		if (ec)
		{
			GE_CORE_ERROR("Could not replace '{0}' - asset registry not saved: {1}",
				registryPath.string(), ec.message());
			std::filesystem::remove(tempPath, ec);
			return;
		}

		s_Data.RegistryDirty = false;
		GE_CORE_TRACE("Asset registry written ({0} entries)", entries.size());
	}

	void AssetManager::FlushRegistry()
	{
		if (!s_Data.RegistryDirty)
			return;

		SaveRegistry();
	}

	bool AssetManager::IsRegistryWritable()
	{
		return s_Data.WritableRegistry;
	}

}
