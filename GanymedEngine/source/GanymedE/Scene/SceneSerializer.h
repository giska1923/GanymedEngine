#pragma once

#include "Scene.h"

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
	private:
		// The throwing half, split out only so Deserialize can wrap it in one try block
		// without re-indenting every component branch.
		bool DeserializeUnchecked(const std::string& filepath);
	private:
		Ref<Scene> m_Scene;
	};

}
