#pragma once

// The YAML dialect the scene and prefab formats share.
//
// These conversions used to be file-local to SceneSerializer.cpp. PrefabSerializer reads the
// same blocks out of a different container, so the encoding of a vec3 now has exactly one
// definition rather than two that can drift.

#include "GanymedE/Core/Log.h"
#include "GanymedE/Math/Curve.h"
#include "GanymedE/Reflection/Reflection.h"

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

		struct ReflectedCodec
		{
			ReflectedWriter Write = nullptr;
			ReflectedReader Read = nullptr;
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
				}
			};
		}

		// Enums persist as their ordinal, which is why `AssetType` and friends are append-only.
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
					if (const E* typed = value.try_cast<E>())
						out << (int)*typed;
				},
				[](const YAML::Node& node, const entt::meta_data& field, entt::meta_any& instance)
				{
					return field.set(instance, (E)node.as<int>());
				}
			};
		}

		void RegisterReflectedCodecs();

	}

	// Emits every serialized field of `instance` into an already-open map. Skips `NotSerialized`
	// and `Custom`; `Flatten` writes a nested struct's fields as siblings, which is the shape a
	// collider's PhysicsMaterial has always had on disk.
	void WriteReflected(YAML::Emitter& out, const entt::meta_any& instance);

	// The inverse. A key that is absent leaves the field at its constructed value.
	void ReadReflected(const YAML::Node& node, entt::meta_any instance);

	template<typename T>
	void WriteReflectedComponent(YAML::Emitter& out, const char* key, const T& component)
	{
		out << YAML::Key << key;
		out << YAML::BeginMap;
		WriteReflected(out, entt::forward_as_meta(component));
		out << YAML::EndMap;
	}

	template<typename T>
	void ReadReflectedComponent(const YAML::Node& node, T& component)
	{
		entt::meta_any instance = entt::forward_as_meta(component);
		ReadReflected(node, instance.as_ref());
	}


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

}
