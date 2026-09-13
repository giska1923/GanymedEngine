#pragma once

// The YAML dialect the scene and prefab formats share.
//
// These conversions used to be file-local to SceneSerializer.cpp. PrefabSerializer reads the
// same blocks out of a different container, so the encoding of a vec3 now has exactly one
// definition rather than two that can drift.

#include "GanymedE/Assets/AssetRef.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Math/Curve.h"
#include "GanymedE/Reflection/Reflection.h"
#include "GanymedE/Scene/ReflectedValue.h"

#include <entt/entt.hpp>

#include <unordered_map>

#include <glm/glm.hpp>

#include <yaml-cpp/yaml.h>

namespace YAML {

	template<>
	struct convert<glm::vec3>
	{
		static Node encode(const glm::vec3& rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			return node;
		}

		static bool decode(const Node& node, glm::vec3& rhs)
		{
			if (!node.IsSequence() || node.size() != 3)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::vec4>
	{
		static Node encode(const glm::vec4& rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			node.push_back(rhs.w);
			return node;
		}

		static bool decode(const Node& node, glm::vec4& rhs)
		{
			if (!node.IsSequence() || node.size() != 4)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			rhs.w = node[3].as<float>();
			return true;
		}
	};

	// Flow sequence of [t, value] / [t, r, g, b, a] keys. Decode never returns false:
	// a scalar (or any non-sequence) warns and yields the default curve, so a bad
	// particle block cannot throw out of a scene load the way a bad vec3 still can.
	// Per-element IsSequence is the MaterialOverrides posture — skip the bad key,
	// keep the rest.
	template<>
	struct convert<GanymedE::FloatCurve>
	{
		static Node encode(const GanymedE::FloatCurve& rhs)
		{
			Node node;
			for (const GanymedE::FloatKey& key : rhs.Keys())
			{
				Node k;
				k.push_back(key.Time);
				k.push_back(key.Value);
				node.push_back(k);
			}
			return node;
		}

		static bool decode(const Node& node, GanymedE::FloatCurve& rhs)
		{
			rhs = GanymedE::FloatCurve();
			if (!node.IsSequence())
			{
				GE_CORE_WARN("FloatCurve YAML: expected a sequence of [t, value] keys, using default");
				return true;
			}

			std::vector<GanymedE::FloatKey> keys;
			keys.reserve(node.size());
			for (const auto& elem : node)
			{
				if (!elem.IsSequence() || elem.size() != 2)
				{
					GE_CORE_WARN("FloatCurve YAML: skipping malformed key (expected [t, value])");
					continue;
				}

				keys.push_back({ elem[0].as<float>(0.0f), elem[1].as<float>(0.0f) });
			}

			if (keys.empty())
			{
				if (node.size() != 0)
					GE_CORE_WARN("FloatCurve YAML: no valid keys, using default");
				return true;
			}

			rhs.ReplaceKeys(std::move(keys));
			return true;
		}
	};

	template<>
	struct convert<GanymedE::ColorGradient>
	{
		static Node encode(const GanymedE::ColorGradient& rhs)
		{
			Node node;
			for (const GanymedE::ColorKey& key : rhs.Keys())
			{
				Node k;
				k.push_back(key.Time);
				k.push_back(key.Value.x);
				k.push_back(key.Value.y);
				k.push_back(key.Value.z);
				k.push_back(key.Value.w);
				node.push_back(k);
			}
			return node;
		}

		static bool decode(const Node& node, GanymedE::ColorGradient& rhs)
		{
			rhs = GanymedE::ColorGradient();
			if (!node.IsSequence())
			{
				GE_CORE_WARN("ColorGradient YAML: expected a sequence of [t, r, g, b, a] keys, using default");
				return true;
			}

			std::vector<GanymedE::ColorKey> keys;
			keys.reserve(node.size());
			for (const auto& elem : node)
			{
				if (!elem.IsSequence() || elem.size() != 5)
				{
					GE_CORE_WARN("ColorGradient YAML: skipping malformed key (expected [t, r, g, b, a])");
					continue;
				}

				keys.push_back({
					elem[0].as<float>(0.0f),
					glm::vec4(
						elem[1].as<float>(0.0f),
						elem[2].as<float>(0.0f),
						elem[3].as<float>(0.0f),
						elem[4].as<float>(1.0f))
				});
			}

			if (keys.empty())
			{
				if (node.size() != 0)
					GE_CORE_WARN("ColorGradient YAML: no valid keys, using default");
				return true;
			}

			rhs.ReplaceKeys(std::move(keys));
			return true;
		}
	};

}
namespace GanymedE {

	// ---------------------------------------------------------------------------------------
	// Emitter overloads for the types the scene dialect writes as inline sequences.
	//
	// **These must stay above every template that uses them.** A dependent call like
	// `out << value` inside a template is resolved by ordinary lookup at the template's
	// DEFINITION point plus ADL at instantiation; ADL searches YAML and glm, and neither
	// declares these, so a template defined above them has no candidate at all. MSVC accepts
	// it regardless - it defers the whole lookup to instantiation - which is why the wrong
	// order survived until the first GCC build.

	inline YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec3& v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
		return out;
	}

	inline YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec4& v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
		return out;
	}

	inline YAML::Emitter& operator<<(YAML::Emitter& out, const FloatCurve& curve)
	{
		out << YAML::Flow << YAML::BeginSeq;
		for (const FloatKey& key : curve.Keys())
			out << YAML::Flow << YAML::BeginSeq << key.Time << key.Value << YAML::EndSeq;
		out << YAML::EndSeq;
		return out;
	}

	inline YAML::Emitter& operator<<(YAML::Emitter& out, const ColorGradient& gradient)
	{
		out << YAML::Flow << YAML::BeginSeq;
		for (const ColorKey& key : gradient.Keys())
		{
			out << YAML::Flow << YAML::BeginSeq
				<< key.Time << key.Value.x << key.Value.y << key.Value.z << key.Value.w
				<< YAML::EndSeq;
		}
		out << YAML::EndSeq;
		return out;
	}

	// ---------------------------------------------------------------------------------------
	// Reflected read/write: a component's YAML from what `entt::meta` knows about it.
	//
	// REFLECTION_ROADMAP.md R3. The gate is **byte-identical output**, not merely a working
	// round-trip: every committed `.ganymede` and `.gprefab` is a file people diff, and a
	// serializer that reorders a key or reformats a float invalidates all of them at once.
	//
	// Three things make byte-identity achievable rather than hopeful:
	//
	//  1. `meta_type::data()` iterates in **registration order** (verified in R2), and
	//     `ComponentReflection.cpp` registers fields in the order the hand-written writer emitted
	//     them. Key order therefore matches by construction.
	//  2. The registered field name **is** the YAML key - decision 3 of the roadmap, which is why
	//     names are written out by hand rather than stringified from the C++ token.
	//  3. Values go through the very same `operator<<` overloads above. A `float` written from a
	//     `meta_any` and one written from the member reach yaml-cpp identically.
	//
	// Reading is deliberately **more tolerant than the hand-written loader**, and this is not
	// optional: the old code did `c.Field = node["Field"].as<T>()` unguarded, so a missing key
	// threw. A field omitted because it equalled its default (`OmitIfDefault`) has to read back as
	// that default, so a missing key must leave the constructed value alone.
	namespace Detail {

		using ReflectedWriter = void (*)(YAML::Emitter&, const entt::meta_any&);
		using ReflectedReader = bool (*)(const YAML::Node&, const entt::meta_data&, entt::meta_any&);
		using ReflectedEquals = bool (*)(const entt::meta_any&, const entt::meta_any&);

		struct ReflectedCodec
		{
			ReflectedWriter Write = nullptr;
			ReflectedReader Read = nullptr;

			// What `OmitIfDefault` compares with. It lives here rather than on `entt::meta_type`
			// because entt has no equality it can synthesize: `meta_any::operator==` compares the
			// ADDRESSES two anys point at unless the type registered a comparison function, and an
			// address compare between a live component and a default-constructed one is false for
			// every field every time - which would have made OmitIfDefault a silent no-op rather
			// than a loud one.
			ReflectedEquals Equal = nullptr;
		};

		inline std::unordered_map<entt::id_type, ReflectedCodec>& ReflectedCodecs()
		{
			static std::unordered_map<entt::id_type, ReflectedCodec> s_Codecs;
			return s_Codecs;
		}

		template<typename T>
		void RegisterReflectedCodec()
		{
			const entt::meta_type type = entt::resolve<T>();
			if (!type)
				return;

			ReflectedCodecs()[type.id()] = {
				[](YAML::Emitter& out, const entt::meta_any& value)
				{
					if (const T* typed = value.try_cast<T>())
						out << *typed;
				},
				[](const YAML::Node& node, const entt::meta_data& field, entt::meta_any& instance)
				{
					return field.set(instance, node.as<T>());
				},
				[](const entt::meta_any& a, const entt::meta_any& b)
				{
					const T* x = a.try_cast<T>();
					const T* y = b.try_cast<T>();
					return x && y && *x == *y;
				}
			};
		}

		// A UUID is a uint64 on disk and an opaque identity in memory. It gets a hand-written
		// codec rather than riding on the generic one above because that one needs a yaml-cpp
		// `operator<<` and a `convert<>` for its type, and giving UUID both would make every
		// `out << someUUID` in the codebase compile - including the ones that mean "write the
		// entity key" and deliberately spell the cast themselves.
		inline void RegisterReflectedUUIDCodec()
		{
			const entt::meta_type type = entt::resolve<UUID>();
			if (!type)
				return;

			ReflectedCodecs()[type.id()] = {
				[](YAML::Emitter& out, const entt::meta_any& value)
				{
					if (const UUID* typed = value.try_cast<UUID>())
						out << static_cast<uint64_t>(*typed);
				},
				[](const YAML::Node& node, const entt::meta_data& field, entt::meta_any& instance)
				{
					return field.set(instance, UUID{ node.as<uint64_t>() });
				},
				[](const entt::meta_any& a, const entt::meta_any& b)
				{
					const UUID* x = a.try_cast<UUID>();
					const UUID* y = b.try_cast<UUID>();
					return x && y && *x == *y;
				}
			};
		}

		// A type persisted as the handle it wraps. `AssetRef<T>` writes and reads exactly the
		// uint64 a bare `AssetHandle` field does, which is why every scene survived the asset
		// milestone unchanged - see the comment on AssetRef itself.
		//
		// Note what the reader does NOT do: resolve. A codec that called `.Get()` would warm every
		// referenced asset at load, including an `AssetRef<Environment>` whose resolve runs a full
		// IBL bake. The one field that genuinely wants warming - StaticMeshComponent::Mesh, where
		// the weak cache collects a dropped warm load before anything uses it - asks for it
		// explicitly at its own call site in SceneSerializer.
		template<typename T>
		void RegisterReflectedAssetRefCodec()
		{
			const entt::meta_type type = entt::resolve<AssetRef<T>>();
			if (!type)
				return;

			ReflectedCodecs()[type.id()] = {
				[](YAML::Emitter& out, const entt::meta_any& value)
				{
					if (const AssetRef<T>* typed = value.try_cast<AssetRef<T>>())
						out << static_cast<uint64_t>(typed->Handle());
				},
				[](const YAML::Node& node, const entt::meta_data& field, entt::meta_any& instance)
				{
					return field.set(instance, AssetRef<T>(AssetHandle(node.as<uint64_t>())));
				},
				[](const entt::meta_any& a, const entt::meta_any& b)
				{
					const AssetRef<T>* x = a.try_cast<AssetRef<T>>();
					const AssetRef<T>* y = b.try_cast<AssetRef<T>>();
					return x && y && *x == *y;   // identity, not object equality
				}
			};
		}

		// The name registered for one enumerator, or nullptr. The values were registered on the
		// enum TYPE (`.data<AudioGroup::Music>("Music")`), so this reads the same list the
		// inspector builds its combo from - one spelling of the names rather than two.
		template<typename E>
		const char* EnumeratorName(E value)
		{
			for (auto&& [id, field] : entt::resolve<E>().data())
			{
				const entt::meta_any registered = field.get({});
				const E* typed = registered.try_cast<E>();
				if (typed && *typed == value)
					return field.name();
			}

			return nullptr;
		}

		template<typename E>
		bool EnumeratorValue(const std::string& name, E& out)
		{
			for (auto&& [id, field] : entt::resolve<E>().data())
			{
				if (!field.name() || name != field.name())
					continue;

				const entt::meta_any registered = field.get({});
				if (const E* typed = registered.try_cast<E>())
				{
					out = *typed;
					return true;
				}
			}

			return false;
		}

		// Enums persist as their ordinal, which is why `AssetType` and friends are append-only -
		// unless the enum carries `SerializeByName`, the escape hatch for one that wants to stay
		// reorderable (AudioGroup) at the cost of a wider key.
		//
		// One registration line per enum rather than a generic path: getting from a `meta_any`
		// holding an enum to its underlying integer needs the concrete type, and an explicit line
		// beside the component that uses it is cheaper than a conversion registration nobody else
		// would read.
		template<typename E>
		void RegisterReflectedEnumCodec()
		{
			const entt::meta_type type = entt::resolve<E>();
			if (!type)
				return;

			ReflectedCodecs()[type.id()] = {
				[](YAML::Emitter& out, const entt::meta_any& value)
				{
					const E* typed = value.try_cast<E>();
					if (!typed)
						return;

					if (!Reflection::Has(entt::resolve<E>(), Reflection::Trait::SerializeByName))
					{
						out << (int)*typed;
						return;
					}

					// An unregistered enumerator falls back to its ordinal rather than writing
					// nothing: a key that vanished would read back as the default and silently
					// retype the field, where a number still round-trips through the reader below
					// and shows up in a diff.
					if (const char* name = EnumeratorName(*typed))
						out << name;
					else
						out << (int)*typed;
				},
				[](const YAML::Node& node, const entt::meta_data& field, entt::meta_any& instance)
				{
					if (!Reflection::Has(entt::resolve<E>(), Reflection::Trait::SerializeByName))
						return field.set(instance, (E)node.as<int>());

					// Tolerant in both directions, because a by-name enum is exactly the one a
					// human hand-edits: an unknown name keeps the constructed default and warns,
					// which is what AudioGroupFromString did before this replaced it.
					const std::string name = node.as<std::string>(std::string{});

					E parsed{};
					if (EnumeratorValue(name, parsed))
						return field.set(instance, parsed);

					GE_CORE_WARN("Reflected enum: '{0}' is not a value of {1}, keeping the default",
						name, entt::resolve<E>().name());
					return false;
				},
				[](const entt::meta_any& a, const entt::meta_any& b)
				{
					const E* x = a.try_cast<E>();
					const E* y = b.try_cast<E>();
					return x && y && *x == *y;
				}
			};
		}

		void RegisterReflectedCodecs();

	}

	// Emits every serialized field of `instance` into an already-open map, skipping
	// `NotSerialized` and `CustomWriter`.
	//
	// `defaults` is a default-constructed instance of the SAME type, and it is what `OmitIfDefault`
	// compares against - which is why it is a parameter rather than something built here. Getting
	// a default-constructed `T` out of an `entt::meta_type` needs `.ctor<>()` registered on every
	// component; passing one down from `WriteReflectedComponent<T>`, which already knows `T`,
	// needs nothing and cannot be forgotten for one type. An empty `defaults` is legal and means
	// "write every field", which is what the nested-map recursion below uses when a sub-struct
	// has no counterpart on the default side.
	//
	// Field shapes, in the order the writer tests them:
	//   Flatten            - the nested struct's fields become SIBLINGS, optionally under a key
	//                        prefix (Attr::KeyPrefix). PhysicsMaterial flattens bare; a RangeF
	//                        flattens as LifetimeMin/LifetimeMax.
	//   a registered codec - one scalar/sequence key, written by that codec.
	//   a reflected struct - a nested MAP under the field's own key. CameraComponent::Camera is
	//                        the only one, and its sub-map is the shape every scene already has.
	void WriteReflected(YAML::Emitter& out, const entt::meta_any& instance,
		const entt::meta_any& defaults = {});

	// The inverse. A key that is absent leaves the field at its constructed value.
	void ReadReflected(const YAML::Node& node, entt::meta_any instance);

	// EmitReflectedValue is declared in ReflectedValue.h and implemented in SceneYaml.cpp - split
	// so the editor can ask "what would this field serialize to" without compiling against
	// yaml-cpp. See that header for why prefab overrides are defined in terms of it.

	// The default `OmitIfDefault` measures against. Function-local and const: constructing a
	// ParticleEmitterComponent allocates (two curves and a pool vector), and a save walks one per
	// entity. Never mutated, and saving is main-thread, so the lazy init race C++11 already closes
	// is the only thread question there is.
	template<typename T>
	const T& ReflectedDefault()
	{
		static const T s_Default{};
		return s_Default;
	}

	// Opens the component's map and writes its reflected fields. A component with fields the
	// generic path cannot own (`CustomWriter`) calls WriteReflected itself between its own
	// BeginMap/EndMap instead - see StaticMeshComponent and ScriptComponent in SceneSerializer.
	template<typename T>
	void WriteReflectedComponent(YAML::Emitter& out, const char* key, const T& component)
	{
		out << YAML::Key << key;
		out << YAML::BeginMap;
		WriteReflected(out, entt::forward_as_meta(component),
			entt::forward_as_meta(ReflectedDefault<T>()));
		out << YAML::EndMap;
	}

	template<typename T>
	void ReadReflectedComponent(const YAML::Node& node, T& component)
	{
		entt::meta_any instance = entt::forward_as_meta(component);
		ReadReflected(node, instance.as_ref());
	}


}
