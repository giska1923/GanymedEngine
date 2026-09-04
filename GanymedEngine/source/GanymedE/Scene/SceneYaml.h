#pragma once

// The YAML dialect the scene and prefab formats share.
//
// These conversions used to be file-local to SceneSerializer.cpp. PrefabSerializer reads the
// same blocks out of a different container, so the encoding of a vec3 now has exactly one
// definition rather than two that can drift.

#include "GanymedE/Core/Log.h"
#include "GanymedE/Math/Curve.h"

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
