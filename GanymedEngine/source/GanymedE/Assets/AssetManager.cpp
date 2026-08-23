#include "gepch.h"
#include "AssetManager.h"

#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/MeshCache.h"
#include "GanymedE/Assets/TextureImporter.h"
#include "GanymedE/Renderer/Environment.h"
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

		bool Initialized = false;
		bool WritableRegistry = true;

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

		SaveRegistry();

		s_Data.Registry.clear();
		s_Data.PathToHandle.clear();
		s_Data.WarnedUnknownHandles.clear();
		// Runs from EditorLayer::OnDetach while Renderer::IsGpuAlive() is still true,
		// so the GPU-resource destructors release real bgfx handles rather than
		// tripping the is-alive guard.
		s_Data.LoadedMeshes.clear();
		s_Data.LoadedEnvironments.clear();
		s_Data.LoadedTextures.clear();
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

		SaveRegistry();
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
			s_Data.LoadedMeshes[handle] = mesh;

		return mesh;
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
				// Material/Scene/Script have no GetAsset cache to evict.
				return;
		}

		GE_CORE_INFO("Reloading asset '{0}'", metadata->FilePath);
	}

	void AssetManager::LoadRegistry()
	{
		std::filesystem::path registryPath = GetAssetRoot() / "AssetRegistry.gr";
		if (!std::filesystem::exists(registryPath))
			return;

		std::ifstream stream(registryPath);
		if (!stream)
			return;

		std::stringstream buffer;
		buffer << stream.rdbuf();

		YAML::Node root = YAML::Load(buffer.str());
		YAML::Node assets = root["Assets"];
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

			s_Data.Registry[metadata.Handle] = metadata;
			s_Data.PathToHandle[metadata.FilePath] = metadata.Handle;
		}
	}

	void AssetManager::SaveRegistry()
	{
		// Guarded here rather than at the call sites so no future caller can bypass it.
		// ImportAsset saves too, and it runs during scene deserialization for path-based
		// components - which is how a read-only install would otherwise have written its
		// registry long before Shutdown ever asked.
		if (!s_Data.WritableRegistry)
			return;

		std::filesystem::path registryPath = GetAssetRoot() / "AssetRegistry.gr";
		std::error_code ec;
		std::filesystem::create_directories(registryPath.parent_path(), ec);

		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Assets" << YAML::Value << YAML::BeginSeq;

		for (const auto& [handle, metadata] : s_Data.Registry)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Handle" << YAML::Value << static_cast<uint64_t>(handle);
			out << YAML::Key << "Type" << YAML::Value << (uint16_t)metadata.Type;
			out << YAML::Key << "FilePath" << YAML::Value << metadata.FilePath;
			out << YAML::EndMap;
		}

		out << YAML::EndSeq;
		out << YAML::EndMap;

		std::ofstream fout(registryPath);
		fout << out.c_str();
	}

}
