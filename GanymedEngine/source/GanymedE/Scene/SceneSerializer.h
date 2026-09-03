#pragma once

#include "Scene.h"
#include "Entity.h"

#include <vector>

namespace YAML { class Emitter; class Node; }

namespace GanymedE {

	class SceneSerializer
	{
	public:
		SceneSerializer(const Ref<Scene>& scene);

		void Serialize(const std::string& filepath);
		void SerializeRuntime(const std::string& filepath);

		// False on a file that is missing, is not a scene, or fails to parse. Never throws:
		// a malformed scene is content to report, not a reason to take the process down.
		bool Deserialize(const std::string& filepath);
		bool DeserializeRuntime(const std::string& filepath);

		// ---- Per-entity halves, shared with the prefab format ----
		//
		// A .gprefab is the same entity blocks under a different root, so the reading and
		// writing of one entity has to be callable on its own. Both are static and take
		// their scene explicitly: a prefab is read into a scene the serializer does not own.

		// Writes one entity's component blocks. Key order here is the file's canonical
		// order - reordering these lines changes every saved scene.
		static void SerializeEntity(YAML::Emitter& out, Entity entity);

		// Creates one entity in `scene` from a serialized block, under a UUID the caller
		// chooses. The UUID is not read from the node because who owns that decision
		// differs per container: a scene keeps the file's UUID (remapping collisions),
		// a prefab instance always mints a fresh one.
		//
		// RelationshipComponent is written through verbatim, still holding the *file's*
		// UUIDs; whoever creates the entities is responsible for resolving them once they
		// all exist (see ResolveHierarchy).
		static Entity DeserializeEntity(const YAML::Node& entityNode, Scene& scene, UUID uuid);

		// Re-points Parent/Children from file UUIDs to the UUIDs the entities were
		// actually created with, after a batch of DeserializeEntity calls. `created` is in
		// file order; `fileUUIDs` is the UUID each entry carried in the file.
		static void ResolveHierarchy(Scene& scene, const std::vector<Entity>& created,
			const std::vector<UUID>& fileUUIDs);
	private:
		// The throwing half, split out only so Deserialize can wrap it in one try block
		// without re-indenting every component branch.
		bool DeserializeUnchecked(const std::string& filepath);
	private:
		Ref<Scene> m_Scene;
	};

}
