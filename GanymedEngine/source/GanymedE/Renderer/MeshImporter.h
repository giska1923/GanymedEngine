#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Mesh.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GanymedE {

	class Scene;
	class Entity;

	class MeshImporter
	{
	public:
		// `outDependencies`, when given, collects the *other source files* this mesh was built
		// from, asset-root-relative: a `.gltf`'s external `.bin` buffers and its external image
		// files. That is what MeshCompiler records in the epoch so editing a `.bin` invalidates
		// the mesh blob built from it. A self-contained `.glb` reports nothing, which is why the
		// sample content exercises the machinery but not the outcome.
		static Ref<Mesh> Load(const std::filesystem::path& path,
			std::vector<std::string>* outDependencies = nullptr);
		static Entity Instantiate(Scene* scene, const std::filesystem::path& path);
	};

}
