#pragma once

#include "GanymedE/Reflection/Reflection.h"

#include <entt/entt.hpp>

namespace GanymedE::EditorUI {

	// The generic inspector: draw a component from what `entt::meta` knows about it, instead of
	// from a hand-written lambda listing every field.
	//
	// ---- Where this sits relative to undo, which is the part worth reading ----
	//
	// It does not touch the undo protocol at all, and that surprised the plan.
	// REFLECTION_ROADMAP.md R2 warned that "the generic drawer has to own `ActiveId` across a drag
	// exactly as the hand-written path does". It does not need to: `SceneHierarchyPanel::
	// DrawComponent<T>` owns the whole commit boundary - it copies the component, reads
	// `GetActiveID()` before and after the *section*, and hands both to `TrackCommitBoundary<T>`.
	// The body's only obligation is the house rule already written on that boundary:
	//
	//     **mutate the component only when a widget actually reported an edit, and return true
	//     when it does.**
	//
	// Every drawer below obeys that, so a converted section keeps one-command-per-gesture for
	// free. That is why R2 could convert sections without editing EditorUndo at all.
	//
	// ---- Why the registry is editor-side ----
	//
	// R1 settled it: `.custom<>` holds exactly one payload per meta object, so a second
	// registration pass from the editor would *overwrite* the engine's attributes rather than add
	// to them. Drawer function pointers are editor-only knowledge and live in this map, keyed by
	// `meta_type`, instead of in the engine's registration file.

	// One field. `instance` is a reference-any onto the live component, so `Field.set` writes
	// through to it.
	struct PropertyContext
	{
		const char* Label = nullptr;
		entt::meta_any* Instance = nullptr;
		entt::meta_data Field;

		// Null when the field carries no valued attributes.
		const Reflection::Attr* Attributes = nullptr;
		Reflection::Trait Traits = Reflection::Trait::None;

		// Convenience: the registered range/speed, or the fallback when none was registered.
		float Speed(float fallback) const;
		bool Range(float& min, float& max) const;
	};

	// Returns true only on a frame a widget reported an edit. See the house rule above.
	using PropertyDrawer = bool (*)(const PropertyContext&);

	// Later registrations replace earlier ones for the same type, so a bespoke drawer can
	// override a default.
	void RegisterPropertyDrawer(const entt::meta_type& type, PropertyDrawer drawer);

	// Installs drawers for the primitives, glm vectors, registered enums and `AssetRef<T>`.
	// Called once from the editor layer; safe to call again.
	void InitPropertyDrawers();

	// Draw every reflected field of `instance`, in registration order (which `meta_type::data()`
	// preserves - verified, not assumed; it is what lets a converted section keep its field
	// order). Fields marked `Hidden` or `Custom` are skipped, `ReadOnly` fields are drawn
	// disabled, and a `Flatten`ed struct member has its own fields drawn as siblings.
	//
	// A field whose type has no registered drawer is skipped and named once in the log, rather
	// than silently vanishing - a missing drawer should be loud.
	bool DrawReflectedProperties(entt::meta_any instance);

	// The same for one field, for a hand-written section that wants the generic treatment for
	// part of itself.
	bool DrawProperty(entt::meta_any& instance, const entt::meta_data& field);

	// `DrawReflectedProperties` for the component `T` on this entity, plus the type-level Note
	// rendered underneath as the hand-written sections render theirs.
	template<typename T>
	bool DrawReflectedComponent(T& component)
	{
		entt::meta_any instance = entt::forward_as_meta(component);
		const bool edited = DrawReflectedProperties(instance.as_ref());

		if (const Reflection::Attr* attr = Reflection::Attributes(entt::resolve<T>()))
		{
			if (attr->Note)
				ImGui::TextDisabled("%s", attr->Note);
		}

		return edited;
	}

}
