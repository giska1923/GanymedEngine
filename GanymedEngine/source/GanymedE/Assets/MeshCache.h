#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Mesh.h"

#include <filesystem>

namespace GanymedE {

	// Fast binary cache for imported glTF meshes (stored under assets/.assets/).
	class MeshCache
	{
	public:
		static Ref<Mesh> TryLoad(const std::filesystem::path& sourceRelativePath,
			const std::filesystem::path& sourceFullPath);

		static bool Write(const Ref<Mesh>& mesh, const std::filesystem::path& sourceRelativePath,
			const std::filesystem::path& sourceFullPath);

	private:
		static std::filesystem::path GetCachePath(const std::filesystem::path& sourceRelativePath);
		static uint64_t GetFileTimestamp(const std::filesystem::path& path);
	};

}
