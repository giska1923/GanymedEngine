#pragma once

#include <yaml-cpp/yaml.h>

namespace GanymedE {

	// A parsed `.gprefab`, cached by the asset manager.
	//
	// ---- Why this type exists, and why it did not before ----
	//
	// Prefab was path-resolved by design, alongside Script and Audio: `AssetManager` answered
	// handle -> path and the consumer loaded itself. The reason recorded for those three is an
	// **external owner** - Lua owns its chunks, miniaudio owns decoded audio - so a manager cache
	// would be a second ref-counted owner of the same resource. That reason never applied to
	// Prefab: nothing else owns a parsed prefab, and the editor already kept one by hand.
	//
	// What kept it path-resolved anyway was that it cost nothing: every
	// `PrefabSerializer::Instantiate` call site was an editor gesture, so re-reading the file per
	// call was a read per drag-drop. Runtime spawning is what changes the arithmetic - a script
	// firing projectiles would re-read and re-parse the same file per spawn. See
	// docs/history/RUNTIME_PREFAB_SPAWNING.md.
	//
	// ---- What is cached is the document, not a Scene ----
	//
	// The alternative was a detached `Scene` holding instantiated entities, which is what the
	// editor's override-template cache keeps. That serves the editor and nothing else: a spawner
	// would have to deep-copy entities out of it, which is a different mechanism with different
	// identity rules. The document is what **both** consumers instantiate *from*, and it is the
	// expensive half - the file read and the YAML parse, both of which now happen once on a
	// worker instead of per call on the main thread.
	class Prefab
	{
	public:
		explicit Prefab(YAML::Node document)
			: m_Document(std::move(document))
		{
		}

		// The `.gprefab` document, already validated as one by the parse stage: `Prefab` and
		// `Entities` are present and `Entities` is a sequence. Instantiation still guards the
		// fields inside each entity, because a hand-edited file is a normal event.
		const YAML::Node& Document() const { return m_Document; }

	private:
		YAML::Node m_Document;
	};

}
