#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

namespace GanymedE {

	// Linear keyframe types. Storage is array-of-structs (natural for an authoring type);
	// the sampler is the same algorithm as AnimationClip::Channel's FindKeys (upper_bound →
	// lerp) in AnimationSystem.cpp. Not unified with Channel: that one is glTF-shaped
	// parallel Times/Values arrays with Step mode and a vec3/quat pipeline behind it.

	struct FloatKey
	{
		float Time = 0.0f;
		float Value = 0.0f;
	};

	inline bool operator==(const FloatKey& a, const FloatKey& b)
	{
		return a.Time == b.Time && a.Value == b.Value;
	}

	inline bool operator!=(const FloatKey& a, const FloatKey& b) { return !(a == b); }

	struct ColorKey
	{
		float Time = 0.0f;
		glm::vec4 Value{ 1.0f };
	};

	inline bool operator==(const ColorKey& a, const ColorKey& b)
	{
		return a.Time == b.Time && a.Value == b.Value;
	}

	inline bool operator!=(const ColorKey& a, const ColorKey& b) { return !(a == b); }

	class FloatCurve
	{
	public:
		FloatCurve(); // one key {0, 1} — identity multiplier

		float Sample(float t) const; // clamped at ends; linear between keys

		// Sorted-invariant editing API. The keys vector is never exposed mutably.
		size_t AddKey(float time, float value);          // insert-sorted; returns new index
		bool RemoveKey(size_t index);                    // false (and a warning) on the last key
		void SetKey(size_t index, float time, float value); // re-sorts if Time moved

		const std::vector<FloatKey>& Keys() const { return m_Keys; }
		bool IsDefault() const; // exactly the constructed state — serializer omit-guard

		// Deserialize path. Sorts by Time. Empty input becomes the identity default.
		void ReplaceKeys(std::vector<FloatKey> keys);

	private:
		std::vector<FloatKey> m_Keys; // invariant: sorted by Time, size >= 1
	};

	class ColorGradient
	{
	public:
		ColorGradient(); // one key {0, white}

		glm::vec4 Sample(float t) const; // clamped at ends; linear between keys

		size_t AddKey(float time, const glm::vec4& value);
		bool RemoveKey(size_t index);
		void SetKey(size_t index, float time, const glm::vec4& value);

		const std::vector<ColorKey>& Keys() const { return m_Keys; }
		bool IsDefault() const;

		void ReplaceKeys(std::vector<ColorKey> keys);

	private:
		std::vector<ColorKey> m_Keys; // invariant: sorted by Time, size >= 1
	};

	// Exact key-for-key equality, which is what the generic serializer's OmitIfDefault needs and
	// what IsDefault() has always meant one instance of. Deliberately NOT an epsilon compare: two
	// curves that differ by a float ulp are different authored data, and a save that quietly
	// dropped the difference would be worse than a noisy diff.
	inline bool operator==(const FloatCurve& a, const FloatCurve& b) { return a.Keys() == b.Keys(); }
	inline bool operator!=(const FloatCurve& a, const FloatCurve& b) { return !(a == b); }

	inline bool operator==(const ColorGradient& a, const ColorGradient& b) { return a.Keys() == b.Keys(); }
	inline bool operator!=(const ColorGradient& a, const ColorGradient& b) { return !(a == b); }

}
