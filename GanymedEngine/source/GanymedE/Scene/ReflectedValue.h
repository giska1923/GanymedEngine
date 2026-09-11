#pragma once

#include <entt/entt.hpp>

#include <string>

namespace GanymedE {

	// The YAML text one reflected field would be written as, or empty when its type has no codec.
	//
	// Declared apart from SceneYaml.h, which is where it is implemented, purely so a consumer can
	// ask the question without taking a yaml-cpp include with it. The editor's prefab-override
	// diff is that consumer: it needs "what would this field serialize to" and nothing else about
	// the YAML layer, and the editor project has no reason to compile against yaml-cpp.
	//
	// Why this is the comparison used for overrides: **a field that would serialize identically is
	// not an override.** That keeps "overridden" and "would be written differently" the same
	// statement, which a hand-written operator== per type would eventually stop being.
	std::string EmitReflectedValue(const entt::meta_any& instance, const entt::meta_data& field);

}
