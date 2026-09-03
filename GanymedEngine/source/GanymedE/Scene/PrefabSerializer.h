#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/UUID.h"
#include "GanymedE/Scene/Entity.h"

#include <filesystem>

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
		Entity Instantiate(const std::filesystem::path& fullPath, Scene& scene, AssetHandle source,
			const InstantiateOptions& options = {});

	}

}
