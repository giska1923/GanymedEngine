#pragma once

#include "GanymedE/Assets/AssetTypes.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <type_traits>

// Member reflection over entt::meta.
//
// Ganymed already reflects TYPES well - ComponentList, ForEachType and ComponentTraits<T> in
// ECS/ComponentTraits.h drive Scene::Copy, the change-buffer hookup and EditorUndo's snapshot
// tuple. What it does not reflect is MEMBERS: the serializer, the inspector and the Lua bindings
// each hand-list every field. This is the member half, and nothing else.
//
// ---- The one discipline line that matters ----
//
// **No engine system may read a component through entt::meta.** meta_data::get returns meta_any
// BY VALUE; entt's small-buffer optimization covers a float or a bool, but a std::string, a
// glm::mat4 or a std::vector allocates. For an inspector (a few dozen per frame on one selected
// entity) that is irrelevant; for a save (thousands per file) it is fine because saving is not a
// frame-budget operation; for a system touching every entity every frame it is disqualifying.
// Systems use ComponentList/ForEachType and direct member access, as they do today.
//
// ---- Why entt::meta and not RTTR ----
//
// RTTR would mean a second reflection system beside one already shipped, by a different author,
// that knows nothing about entt's component storage - and "add component by type name",
// copy/paste-a-component and prefab property diffing all need to get from a reflected type back to
// real storage. What RTTR would buy (reflection across a DLL boundary, enumerating types it was
// not compiled against) matters for plugin architectures; Ganymed is one static library in one
// build. See docs/history/REFLECTION_ROADMAP.md decision 1.
namespace GanymedE::Reflection {

	// Valueless attributes, packed into the meta node itself: free to read, no allocation, no
	// lookup. Anything carrying a VALUE goes in Attr below instead.
	//
	// Hard ceiling: entt reserves the low 16 bits of a node's traits word for its own flags
	// (is_class, is_enum, ...) and shifts user traits into the upper half, so a user enum gets
	// exactly 16 bits (meta_traits::_user_defined_traits == 0xFFFF, meta/node.hpp). entt also
	// asserts value < 0xFFFF, so all sixteen set at once is rejected - academic, but real.
	// Eleven are spent below. Do not spend one on something that wants a number or a string.
	//
	// Every flag here exists because the current inspector or serializer measurably needs it.
	// ReadOnly is PrefabInstanceComponent, which draws its source and returns false. There is
	// deliberately no AdvancedOnly: nothing in the panel has an advanced section today, and an
	// unused flag is a knob nobody turns.
	enum class Trait : uint16_t
	{
		None = 0,

		// ---- Inspector ----
		Hidden        = 1 << 0,  // never drawn (derived data, structural state, C++ bindings)
		ReadOnly      = 1 << 1,  // drawn, not editable
		Color         = 1 << 2,  // a vec3/vec4 that wants ColorEdit, not DragFloat
		Radians       = 1 << 3,  // stored in radians, authored in degrees

		// ---- Serialization ----
		NotSerialized = 1 << 4,  // skipped by save and load
		OmitIfDefault = 1 << 5,  // written only when it differs from a default-constructed value
		Flatten       = 1 << 6,  // nested struct's fields are emitted as siblings, not a sub-map
		SerializeByName = 1 << 7, // type-level, on an enum: persisted as a name, not an ordinal

		// ---- Structural ----
		Component     = 1 << 8,  // type-level: this is an ECS component, not a supporting type

		// "A bespoke implementation owns this field", asked separately of each consumer.
		//
		// These were ONE flag until the serializer conversion finished, and the conflation was
		// load-bearing in the wrong direction: AnimatorComponent::Clip, SizeCurve and
		// ColorOverLifetime need a bespoke *widget* (a combo over the mesh's clip names, a curve
		// editor, a gradient editor) and nothing bespoke at all on disk - a string and two
		// sequences. Spelled as one flag they were locked out of the generic writer by a fact
		// about the inspector, which is exactly the coupling the roadmap's "the two consumers
		// convert independently" line rejects. One extra bit buys that independence back.
		CustomDrawer  = 1 << 9,  // the inspector's generic drawer must skip it
		CustomWriter  = 1 << 10, // the generic serializer must skip it

		// Runtime state a system owns: not authored, not saved, not shown. A composite rather
		// than a flag of its own, because "Transient" would carry no information the other two
		// do not already carry, and flags are a budget.
		Runtime = Hidden | NotSerialized,

		// Both consumers at once - the common case, and the spelling every existing registration
		// used before the split, so nothing that meant "nobody touches this generically" moved.
		Custom = CustomDrawer | CustomWriter
	};

	constexpr Trait operator|(Trait a, Trait b)
	{
		return static_cast<Trait>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
	}

	constexpr Trait operator&(Trait a, Trait b)
	{
		return static_cast<Trait>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
	}

	// True when ANY bit of `flags` is set. There is no All() because no call site wants one yet.
	constexpr bool Has(Trait set, Trait flags)
	{
		return static_cast<uint16_t>(set & flags) != 0;
	}

	// Valued attributes. ONE payload struct rather than one struct per attribute kind, because
	// entt's .custom<> holds a single payload per meta object - a second .custom<> call replaces
	// the first rather than appending. That is also why registration is engine-side only: an
	// editor-side second pass would silently overwrite everything here.
	//
	// Built by chaining, because C++17 has no designated initializers and eight positional
	// aggregate members would be unreadable and easy to transpose:
	//
	//   .custom<Attr>(Attr{}.Label("Vertical FOV").Range(1.0f, 179.0f).Speed(0.5f))
	//
	// The chain returns a reference into a temporary that lives to the end of the full
	// expression, and .custom<> copies it into a shared_ptr immediately, so this is safe.
	struct Attr
	{
		// nullptr means "derive it from the registered name at display time" (R2's job).
		const char* Display = nullptr;

		// CollapsingHeader the field belongs under. Only the particle emitter groups today
		// ("Emission", "Initial", "Over Lifetime", "Rendering").
		const char* Section = nullptr;

		// One line of explanatory text. Several call sites render this as TextDisabled next to
		// the widget rather than as a hover tooltip; which of the two is R2's decision.
		const char* Note = nullptr;

		// Soft clamp for the widget. For a FloatCurve this is the Y range the curve editor draws.
		float Min = 0.0f;
		float Max = 0.0f;
		bool  HasRange = false;

		// DragFloat step. 0 means "the drawer's default for this type" - the values here are the
		// ones the hand-written panel actually passes, which differ per field for good reasons
		// (0.01 for damping, 0.1 for a radius, 0.05 for an intensity).
		float DragSpeed = 0.0f;

		// What the X/Y/Z row's coloured reset buttons set the component to. Zero for a position
		// or an offset, which is why it defaults there; a box collider's half-extents reset to
		// 0.5 and a scale to 1. Added in R2 because it is not derivable and not expressible any
		// other way - the widget takes it as an argument and the type cannot supply it.
		//
		// Used by exactly one registered field today (BoxColliderComponent::HalfExtents). It
		// earns the slot anyway: without it the collider sections cannot go through the generic
		// drawer without changing what their reset buttons do.
		float ResetValue = 0.0f;

		// What a Flatten'ed field prefixes its children's YAML keys with. Empty for the shape
		// PhysicsMaterial has always had on disk (Friction and Restitution, bare), set to
		// "Lifetime" for the RangeF that must keep writing LifetimeMin and LifetimeMax.
		//
		// A per-field value rather than a trait on RangeF itself, even though every RangeF field
		// wants one: a type-level rule is invisible at the point a reader is looking at the
		// field, which is the same objection the particle block already records against an
		// inherited "all my fields omit" flag. It is also not derivable - the prefix HAPPENS to
		// equal the field name for all five today, and encoding that coincidence as a rule would
		// make the on-disk key a function of the C++ member name, which decision 3 forbids.
		const char* KeyPrefix = nullptr;

		// What an AssetHandle field accepts. AssetHandle is a plain UUID alias, identical to the
		// type of RelationshipComponent::Parent, so the TYPE cannot express this - which is
		// exactly why the two-tier vocabulary exists.
		AssetType Slot = AssetType::None;

		Attr& Label(const char* text) { Display = text; return *this; }
		Attr& In(const char* section) { Section = section; return *this; }
		Attr& Tip(const char* text) { Note = text; return *this; }
		Attr& Range(float lo, float hi) { Min = lo; Max = hi; HasRange = true; return *this; }
		Attr& Speed(float step) { DragSpeed = step; return *this; }
		Attr& Reset(float value) { ResetValue = value; return *this; }
		Attr& Keys(const char* prefix) { KeyPrefix = prefix; return *this; }
		Attr& Asset(AssetType type) { Slot = type; return *this; }
	};

	// Attributes of a field or a type, or nullptr when none were registered. The cast is
	// type-hash-checked inside entt, so a payload of some other type reads back as nullptr
	// rather than as garbage.
	inline const Attr* Attributes(const entt::meta_data& field) { return field.custom(); }
	inline const Attr* Attributes(const entt::meta_type& type) { return type.custom(); }

	inline bool Has(const entt::meta_data& field, Trait flags) { return Has(field.traits<Trait>(), flags); }
	inline bool Has(const entt::meta_type& type, Trait flags) { return Has(type.traits<Trait>(), flags); }

	// Whether a type went through GE_REFLECT_*.
	//
	// NOT the same as `entt::resolve<T>()` being truthy: entt synthesizes a node for any type it
	// is asked about, from a function-local static, so resolve<T>() is ALWAYS truthy and would
	// report an entirely empty registration file as healthy. `name` is only ever assigned by
	// .type(id, name), which makes it the honest signal.
	inline bool IsReflected(const entt::meta_type& type) { return type && type.name() != nullptr; }

	template<typename T>
	inline bool IsReflected() { return IsReflected(entt::resolve<T>()); }

	// Registers every component and supporting type. Idempotent - entt's factory keeps the
	// existing node and replaces same-id members in place - so a tool that cannot be sure
	// Application ran may call it defensively.
	//
	// Explicit, and named as such, rather than file-scope static registration objects: in a
	// static library the linker drops a TU nothing references, which loses reflection for that
	// type in Release or in the runtime while the Debug editor keeps working. See the Risks
	// section of docs/history/REFLECTION_ROADMAP.md.
	void Init();

	// Checks what the registration file cannot: that every ComponentList entry is actually
	// registered, that no field was left nameless, and that valued/flag attributes match the
	// type they were put on (a Color on a float, a Slot on something that is not an AssetHandle).
	// Runs from Init in Debug builds. Returns false and logs rather than throwing.
	//
	// What it CANNOT check, stated plainly: whether a type's member list is COMPLETE. The true
	// member set is exactly the thing that is not reflected. The size sentinels in
	// ComponentReflection.cpp are the only forcing function for that.
	bool Validate();
}

// The registration entry points. Two spellings because the distinction is real and consumed:
// GE_REFLECT_COMPONENT marks the type so "which meta types can be added to an entity" is
// answerable without a hand-maintained list beside ComponentList.
//
// The type name IS a serialized key ("TransformComponent:" in every .gscene), so stringifying it
// couples that key to the C++ token - the coupling decision 3 forbids for FIELD names. It is
// accepted here and not there for two reasons: a component-type rename is loud (the component
// vanishes from every entity in the editor immediately, in every scene) where a field rename
// silently drops one value, and the type token already has a second binding in ComponentList that
// a rename must satisfy anyway. Field names stay written out by hand.
#define GE_REFLECT_COMPONENT(Type) \
	entt::meta_factory<Type>{}.type(#Type).traits(::GanymedE::Reflection::Trait::Component)

#define GE_REFLECT_TYPE(Type) \
	entt::meta_factory<Type>{}.type(#Type)
