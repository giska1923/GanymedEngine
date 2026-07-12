#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Mesh.h"

#include <filesystem>

namespace GanymedE {

	class Scene;
	class Entity;

	class MeshImporter
	{
	public:
		static Ref<Mesh> Load(const std::filesystem::path& path);
		static Entity Instantiate(Scene* scene, const std::filesystem::path& path);
	};

}
