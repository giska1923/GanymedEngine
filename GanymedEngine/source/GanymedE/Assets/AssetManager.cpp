#include "gepch.h"
#include "AssetManager.h"

#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/MeshCache.h"
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Renderer/MeshImporter.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

namespace GanymedE {

	struct AssetManagerData
	{
		std::unordered_map<AssetHandle, AssetMetadata> Registry;
		std::unordered_map<std::string, AssetHandle> PathToHandle;

		std::unordered_map<AssetHandle, Ref<Mesh>> LoadedMeshes;
		std::unordered_map<AssetHandle, Ref<Environment>> LoadedEnvironments;

		bool Initialized = false;
	};

	static AssetManagerData s_Data;

	void AssetManager::Init()
	{
		if (s_Data.Initialized)
			return;

		LoadRegistry();
		s_Data.Initialized = true;
		GE_CORE_INFO("AssetManager initialized ({0} registered assets)", s_Data.Registry.size());
	}

	void AssetManager::Shutdown()
	{
		if (!s_Data.Initialized)
			return;

		SaveRegistry();
		s_Data.Registry.clear();
		s_Data.PathToHandle.clear();
		s_Data.LoadedMeshes.clear();
		s_Data.LoadedEnvironments.clear();
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
