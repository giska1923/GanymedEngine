#pragma once

#include "GanymedE/Assets/AssetCompiler.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/MeshSource.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GanymedE {

	class Scene;
	class Entity;

	// Facts the importer already knows and currently only logs. Parsed from the glTF JSON
	// without loading buffers, so this is cheap relative to Import and does not build a Mesh.
	// The Asset Inspector asks once per selection; a cache replay has already dropped the
	// warnings, and bumping the mesh blob to carry them would invalidate every compiled mesh
	// for three UI strings.
	struct MeshSourceInspect
	{
		uint32_t SkinCount = 0;
		bool AnyPrimitiveMissingTangent = false;
		bool AnyNormalMap = false;
	};

	// Defaults MeshImporter and the Asset Inspector both read. Unset means these values,
	// which is today's hard-coded behaviour — so an empty sidecar does not change a mesh.
	struct MeshImportSettings
	{
		static constexpr float ImportScale = 1.0f;
		static constexpr const char* TangentPolicy = "WhenMissing";
		static constexpr const char* UpAxis = "Y";
	};

	class MeshImporter
	{
	public:
		// glTF JSON only — skins, attributes, material slots. False when the file will not parse.
		static bool InspectSource(const std::filesystem::path& path, MeshSourceInspect& out);

		// glTF -> CPU-side mesh data. **No bgfx call anywhere below this**, which is what lets
		// the asset compiler run it on a worker thread; `BuildMesh` is the main-thread half.
		//
		// `outDependencies`, when given, collects the *other source files* this mesh was built
		// from, asset-root-relative: a `.gltf`'s external `.bin` buffers and its external image
		// files. That is what MeshCompiler records in the epoch so editing a `.bin` invalidates
		// the mesh blob built from it. A self-contained `.glb` reports nothing, which is why the
		// sample content exercises the machinery but not the outcome.
		static bool Import(const std::filesystem::path& path, MeshSource& out,
			std::vector<std::string>* outDependencies = nullptr,
			const AssetConfig* config = nullptr);
		static Entity Instantiate(Scene* scene, const std::filesystem::path& path);
	};

}
