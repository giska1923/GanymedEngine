#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/UUID.h"
#include "GanymedE/Scene/Entity.h"

#include <filesystem>

// Declared, not included: the editor links this header and has no yaml-cpp include path, and a
// reference parameter does not need the definition. Only the engine-side callers that actually
// hold a document include <yaml-cpp/yaml.h>.
namespace YAML { class Node; }

namespace GanymedE {

	class Scene;
	struct TransformComponent;

	// Read/write for `.gprefab`, an authored entity subtree.
	//
	// The file is the *scene* format's entity list under a `Prefab:` root instead of a `Scene:`
	// one: same component blocks, written by SceneSerializer::SerializeEntity, read by
	// SceneSerializer::DeserializeEntity, ordered by the same hierarchy DFS. One schema, two
	// containers - which is what the Phase 1 serializer split was for.
	//
	// Scope, v1: a prefab is a subtree; an instance remembers its source; propagation is
	// explicit (Apply / Revert), whole-subtree, with no per-field overrides, no nesting, and no
	// auto-update of open scenes. The Unity/Unreal norm - override tracking and propagation on
	// apply - is a serialization-diff engine this does not have; this is the largest useful
	// subset that needs none of it. The upgrade path stays open because instances already
	// serialize their source handle.
	namespace PrefabSerializer {

		// Writes `root` and its descendants.
		//
		// **UUIDs in the file are canonical: 1..N in DFS order, not the instance's own.** With
		// preserved UUIDs, applying identical content from two different instances produces two
		// different files - a lie in the diff. With canonical ones, identical content means
		// identical bytes and Apply is diff-stable from anywhere, and the file carries no trace
		// of which scene birthed it. This is the fileID stability Unity gets from its own local
		// ID scheme, reached the cheap way. Cost, accepted: an Apply that only reorders children
		// renumbers the UUIDs below the moved block - but sibling order is content, so that is a
		// real change in the file.
		//
		// Only IDComponent and RelationshipComponent are renumbered. AssetHandle *is* UUID, so a
		// blanket remap would corrupt StaticMesh.Mesh, its MaterialOverrides,
		// SkyLight.Environment, Script.Script and AudioSource.Clip.
		//
		// `rootTransform`, when given, replaces the root's own transform in the file. Apply
		// passes the *file's existing* root transform so that re-applying never writes the
		// instance's placement into the asset - placement is per-instance (the Unity norm).
		bool Save(Scene& scene, Entity root, const std::filesystem::path& fullPath,
			const TransformComponent* rootTransform = nullptr);

		// The root transform stored in an existing `.gprefab`, or false when the file is missing
		// or unreadable. Apply needs it to leave placement alone.
		bool ReadRootTransform(const std::filesystem::path& fullPath, TransformComponent& out);

		struct InstantiateOptions
		{
			// 0 mints a fresh UUID for the root. Revert passes the instance's existing one so
			// that references to the instance root survive.
			UUID RootUUID = UUID{ 0 };

			// Null uses the transform stored in the file (the spawn default). Revert passes the
			// instance's current transform: everything below the root is file-owned, the root's
			// placement is not.
			const TransformComponent* RootTransform = nullptr;
		};

		// Creates the subtree in `scene` with fresh UUIDs and tags the root with
		// PrefabInstanceComponent{source}. Returns an invalid Entity on failure; never throws.
		//
		// **The document overload is the primary one**; the path overload reads the file and
		// calls it. They exist as a pair because the two callers want different things: an
		// editor gesture has a path and wants the answer now, while anything instantiating the
		// same prefab repeatedly should hold a `Ref<Prefab>` from the asset manager and pay the
		// read and parse once. See Prefab.h.
		Entity Instantiate(const YAML::Node& document, Scene& scene, AssetHandle source,
			const InstantiateOptions& options = {});

		Entity Instantiate(const std::filesystem::path& fullPath, Scene& scene, AssetHandle source,
			const InstantiateOptions& options = {});

		// Instantiate from the **asset manager's cached document**, which is what anything
		// instantiating the same prefab more than once should use: the read and the YAML parse
		// happen once, on a worker, and every later call reuses them.
		//
		// This exists rather than having callers fetch a `Ref<Prefab>` themselves because it is
		// what keeps yaml-cpp out of the editor. Every serializer in the engine hides YAML behind
		// its API and the editor has never seen the dependency; handing it a `Prefab` whose
		// accessor returns a `YAML::Node` would have ended that for one call site.
		//
		// **Blocks if the parse has not finished** - `AssetManager::WaitFor`, which pumps other
		// jobs while it waits. Instantiation is a gesture or a spawn, and "not yet" is not an
		// answer either can act on.
		Entity InstantiateFromAsset(AssetHandle source, Scene& scene,
			const InstantiateOptions& options = {});

		// Read and validate a `.gprefab` into `out`. False on anything unreadable, having said
		// why - malformed content is reported, not fatal.
		//
		// Exposed because the asset layer's Parse stage needs it: that is the one place this
		// runs on a worker thread, and it must be the same read and the same validation the
		// editor path uses or the two would drift.
		bool LoadDocument(const std::filesystem::path& fullPath, YAML::Node& out);

	}

}
