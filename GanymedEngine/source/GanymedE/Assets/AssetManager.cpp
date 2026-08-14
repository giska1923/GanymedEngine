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
		bool SaveRegistryOnShutdown = true;
	};

	static AssetManagerData s_Data;

	void AssetManager::Init(bool saveRegistryOnShutdown)
	{
		if (s_Data.Initialized)
			return;

		s_Data.SaveRegistryOnShutdown = saveRegistryOnShutdown;

		LoadRegistry();
		s_Data.Initialized = true;
		GE_CORE_INFO("AssetManager initialized ({0} registered assets, save-on-shutdown {1})",
			s_Data.Registry.size(), saveRegistryOnShutdown ? "on" : "off");
	}

	void AssetManager::Shutdown()
	{
		if (!s_Data.Initialized)
			return;

		if (s_Data.SaveRegistryOnShutdown)
			SaveRegistry();

		s_Data.Registry.clear();
		s_Data.PathToHandle.clear();
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
		if (!metadata || metadata->Type != AssetType::StaticMesh)
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
		if (!metadata || metadata->Type != AssetType::Environment)
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
		if (!metadata || metadata->Type != AssetType::Texture)
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
