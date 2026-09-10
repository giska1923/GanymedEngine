#pragma once

#include "GanymedE/Reflection/Reflection.h"

#include <entt/entt.hpp>

#include <functional>

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
	// What a per-field prefab override affordance needs, or all-null when the component being
	// drawn is not part of a prefab instance. Kept as one opaque hook so the drawers themselves
	// stay ignorant of prefabs.
	struct OverrideHook
	{
		// "Does this field differ from the prefab", and "put the prefab's value back". Both are
		// templates on the component type at the call site, bound here as plain function objects.
		bool (*IsOverridden)(void* owner, const entt::meta_data&) = nullptr;
		bool (*Revert)(void* owner, const entt::meta_data&) = nullptr;
		void* Owner = nullptr;

		explicit operator bool() const { return IsOverridden != nullptr; }
	};

	// Multi-entity editing. Separate from OverrideHook rather than folded into it: the two answer
	// different questions about the same field and a component can be in both states at once.
	struct MultiEditHook
	{
		// "Do the selected entities disagree about this field."
		bool (*IsMixed)(void* owner, const entt::meta_data&) = nullptr;

		// Called after a widget reported an edit: copy the value the primary now holds onto the
		// rest of the selection. Propagating *after* the fact rather than driving N widgets is
		// what keeps every drawer single-entity and unaware that multi-edit exists.
		void (*Propagate)(void* owner, const entt::meta_data&) = nullptr;

		void* Owner = nullptr;

		explicit operator bool() const { return IsMixed != nullptr; }
	};

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

	// "Should this field be drawn at all", asked per field before anything is submitted.
	//
	// This is how a section whose field *visibility* depends on another field's value converts.
	// A clamp can be applied after the generic drawer has run (Spot Light does that), but
	// visibility cannot - you cannot un-draw a field - so it has to be decided first. Deliberately
	// a predicate in editor C++ rather than an attribute: "show this when that other field equals
	// X" is a small expression language, and the vocabulary is not the place for one. The section
	// keeps its cross-field rule; it just stops hand-drawing every widget around it.
	using FieldFilter = std::function<bool(const entt::meta_data&)>;

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
	bool DrawReflectedProperties(entt::meta_any instance, const OverrideHook& overrides = {},
		const MultiEditHook& multi = {}, const FieldFilter& filter = {});

	// The same for one field, for a hand-written section that wants the generic treatment for
	// part of itself.
	bool DrawProperty(entt::meta_any& instance, const entt::meta_data& field,
		const OverrideHook& overrides = {}, const MultiEditHook& multi = {},
		const FieldFilter& filter = {});

	// `DrawReflectedProperties` for the component `T` on this entity, plus the type-level Note
	// rendered underneath as the hand-written sections render theirs.
	template<typename T>
	bool DrawReflectedComponent(T& component, const OverrideHook& overrides = {},
		const MultiEditHook& multi = {}, const FieldFilter& filter = {})
	{
		entt::meta_any instance = entt::forward_as_meta(component);
		const bool edited = DrawReflectedProperties(instance.as_ref(), overrides, multi, filter);

		if (const Reflection::Attr* attr = Reflection::Attributes(entt::resolve<T>()))
		{
			if (attr->Note)
				ImGui::TextDisabled("%s", attr->Note);
		}

		return edited;
	}

}
