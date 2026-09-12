#pragma once

#include "GanymedE/Reflection/Reflection.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/ReflectedValue.h"

#include <entt/entt.hpp>

#include <string>

namespace GanymedE::EditorUI {

	// Per-property prefab overrides: which fields of a prefab instance differ from the prefab it
	// came from, and how to put one back.
	//
	// ---- Overrides are computed, not stored, and that is the design ----
	//
	// Unity records an override list on the instance. Ganymed diffs the instance against the
	// prefab instead, because a recorded list is a second source of truth that can disagree with
	// the first: it goes stale when the prefab changes, it needs migrating when a field is
	// renamed, and every edit path has to remember to maintain it. A diff cannot be stale - it is
	// recomputed from the two things it compares - and it needs no format change beyond the
	// canonical link below.
	//
	// The comparison is `SceneYaml`'s `EmitReflectedValue`: **a field that would serialize
	// identically is not an override.** Reusing the serializer's own notion of a value keeps
	// "overridden" and "would be written differently" the same statement, which a hand-written
	// `operator==` per type would eventually stop being. It also means a type with no YAML codec
	// reports "not overridden" rather than guessing - "cannot tell" must not become a claim.
	//
	// ---- What makes it possible ----
	//
	// `PrefabMemberComponent::CanonicalID` on every instantiated entity, recorded by
	// `PrefabSerializer::Instantiate` while it still knows the pairing. A prefab file numbers its
	// entities 1..N in DFS order, so that id is stable across instantiations; an instance's own
	// UUIDs are fresh every time and say nothing about which prefab object they came from.
	//
	// Entities added by hand inside an instance carry no `PrefabMemberComponent` and take part in
	// no diff, which keeps the "structural freedom inside an instance is allowed and unmarked"
	// rule the prefab milestone established.
	//
	// ---- Queries are templates on the component type ----
	//
	// Not `meta_type`-keyed, because getting from a `meta_type` to a component's storage needs raw
	// `entt::registry` access that neither `Entity` nor `Scene` exposes, and every call site knows
	// the concrete type anyway - the inspector reaches these from `DrawComponent<T>`.

	// The entity in the cached prefab template that `entity` was instantiated from, or a falsy
	// Entity when it has no canonical link, its prefab is missing, or the id is not in the file.
	//
	// The template is a detached `Scene` holding the prefab, instantiated once per source handle
	// and cached. Deserialization only stores asset *handles*, so building one loads no meshes or
	// textures.
	Entity FindPrefabTemplate(Entity entity, Scene& scene);

	// Drops every cached template. Call when the scene changes, or when a `.gprefab` may have been
	// rewritten - the cache has no way to notice that on its own.
	void InvalidatePrefabTemplates();

	// True when this field of `T` differs from the prefab.
	template<typename T>
	bool IsPropertyOverridden(Entity entity, Scene& scene, const entt::meta_data& field)
	{
		Entity source = FindPrefabTemplate(entity, scene);
		if (!source || !entity.HasComponent<T>() || !source.HasComponent<T>())
			return false;

		const std::string live = EmitReflectedValue(
			entt::forward_as_meta(entity.GetComponent<T>()), field);

		if (live.empty())
			return false;

		return live != EmitReflectedValue(
			entt::forward_as_meta(source.GetComponent<T>()), field);
	}

	// Any field of `T` overridden. This is what a *hand-written* inspector section can still show,
	// since the per-field affordance lives inside the reflected property drawer.
	template<typename T>
	bool IsComponentOverridden(Entity entity, Scene& scene)
	{
		Entity source = FindPrefabTemplate(entity, scene);
		if (!source || !entity.HasComponent<T>() || !source.HasComponent<T>())
			return false;

		const entt::meta_type type = entt::resolve<T>();
		if (!Reflection::IsReflected(type))
			return false;

		entt::meta_any live = entt::forward_as_meta(entity.GetComponent<T>());
		entt::meta_any templ = entt::forward_as_meta(source.GetComponent<T>());

		for (auto&& [id, field] : type.data())
		{
			// The DRAWER half: a field with no per-field revert affordance still counts towards
			// "this component is overridden", which is the whole point of the section marker.
			// `field` comes from `entt::resolve<T>()`, so its type is dependent and `traits` needs
			// the disambiguator - without it `<` parses as less-than. MSVC accepts it either way.
			if (Reflection::Has(field.template traits<Reflection::Trait>(), Reflection::Trait::CustomDrawer))
				continue;

			const std::string a = EmitReflectedValue(live, field);
			if (!a.empty() && a != EmitReflectedValue(templ, field))
				return true;
		}

		return false;
	}

	// Copy the prefab's value for one field back onto the entity. False when there was nothing to
	// revert to.
	template<typename T>
	bool RevertProperty(Entity entity, Scene& scene, const entt::meta_data& field)
	{
		Entity source = FindPrefabTemplate(entity, scene);
		if (!source || !entity.HasComponent<T>() || !source.HasComponent<T>())
			return false;

		entt::meta_any live = entt::forward_as_meta(entity.GetComponent<T>());

		// Named rather than passed inline: `meta_data::get` takes `Instance&&` and builds a
		// `meta_handle` from it, which binds `Type&` - so a temporary cannot bind. MSVC allows it
		// as an extension (C4239); GCC and Clang reject it.
		entt::meta_any templ = entt::forward_as_meta(source.GetComponent<T>());
		entt::meta_any value = field.get(templ);
		if (!value)
			return false;

		return field.set(live, value);
	}

}
