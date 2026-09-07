#include "gepch.h"
#include "Curve.h"

#include "GanymedE/Core/Log.h"

namespace GanymedE {

	namespace {

		// Same shape as AnimationSystem::FindKeys: sorted times, upper_bound, lerp alpha.
		// Keys is never empty (enforced by FloatCurve / ColorGradient).
		template<typename Key>
		struct KeySpan
		{
			size_t Lower = 0;
			size_t Upper = 0;
			float Alpha = 0.0f;
		};

		template<typename Key>
		KeySpan<Key> FindKeys(const std::vector<Key>& keys, float time)
		{
			KeySpan<Key> span;

			if (keys.size() < 2 || time <= keys.front().Time)
				return span;

			if (time >= keys.back().Time)
			{
				span.Lower = span.Upper = keys.size() - 1;
				return span;
			}

			auto it = std::upper_bound(keys.begin(), keys.end(), time,
				[](float t, const Key& key) { return t < key.Time; });
			span.Upper = static_cast<size_t>(it - keys.begin());
			span.Lower = span.Upper - 1;

			const float dt = keys[span.Upper].Time - keys[span.Lower].Time;
			span.Alpha = dt > 0.0f ? (time - keys[span.Lower].Time) / dt : 0.0f;
			return span;
		}

		template<typename Key>
		size_t InsertSorted(std::vector<Key>& keys, Key key)
		{
			auto it = std::upper_bound(keys.begin(), keys.end(), key.Time,
				[](float t, const Key& k) { return t < k.Time; });
			auto inserted = keys.insert(it, key);
			return static_cast<size_t>(inserted - keys.begin());
		}

		template<typename Key>
		void SortByTime(std::vector<Key>& keys)
		{
			std::stable_sort(keys.begin(), keys.end(),
				[](const Key& a, const Key& b) { return a.Time < b.Time; });
		}

	}

	FloatCurve::FloatCurve()
	{
		m_Keys.push_back({ 0.0f, 1.0f });
	}

	float FloatCurve::Sample(float t) const
	{
		const KeySpan<FloatKey> span = FindKeys(m_Keys, t);
		const float a = m_Keys[span.Lower].Value;
		const float b = m_Keys[span.Upper].Value;
		return a + (b - a) * span.Alpha;
	}

	size_t FloatCurve::AddKey(float time, float value)
	{
		return InsertSorted(m_Keys, FloatKey{ time, value });
	}

	bool FloatCurve::RemoveKey(size_t index)
	{
		if (index >= m_Keys.size())
		{
			GE_CORE_ASSERT(false, "FloatCurve::RemoveKey: index out of range");
			return false;
		}

		if (m_Keys.size() == 1)
		{
			GE_CORE_WARN("FloatCurve::RemoveKey: refusing to delete the last key");
			return false;
		}

		m_Keys.erase(m_Keys.begin() + static_cast<std::ptrdiff_t>(index));
		return true;
	}

	void FloatCurve::SetKey(size_t index, float time, float value)
	{
		if (index >= m_Keys.size())
		{
			GE_CORE_ASSERT(false, "FloatCurve::SetKey: index out of range");
			return;
		}

		m_Keys[index] = { time, value };
		SortByTime(m_Keys);
	}

	bool FloatCurve::IsDefault() const
	{
		return m_Keys.size() == 1 && m_Keys[0].Time == 0.0f && m_Keys[0].Value == 1.0f;
	}

	void FloatCurve::ReplaceKeys(std::vector<FloatKey> keys)
	{
		if (keys.empty())
		{
			m_Keys = { { 0.0f, 1.0f } };
			return;
		}

		m_Keys = std::move(keys);
		SortByTime(m_Keys);
	}

	ColorGradient::ColorGradient()
	{
		m_Keys.push_back({ 0.0f, glm::vec4(1.0f) });
	}

	glm::vec4 ColorGradient::Sample(float t) const
	{
		const KeySpan<ColorKey> span = FindKeys(m_Keys, t);
		return glm::mix(m_Keys[span.Lower].Value, m_Keys[span.Upper].Value, span.Alpha);
	}

	size_t ColorGradient::AddKey(float time, const glm::vec4& value)
	{
		return InsertSorted(m_Keys, ColorKey{ time, value });
	}

	bool ColorGradient::RemoveKey(size_t index)
	{
		if (index >= m_Keys.size())
		{
			GE_CORE_ASSERT(false, "ColorGradient::RemoveKey: index out of range");
			return false;
		}

		if (m_Keys.size() == 1)
		{
			GE_CORE_WARN("ColorGradient::RemoveKey: refusing to delete the last key");
			return false;
		}

		m_Keys.erase(m_Keys.begin() + static_cast<std::ptrdiff_t>(index));
		return true;
	}

	void ColorGradient::SetKey(size_t index, float time, const glm::vec4& value)
	{
		if (index >= m_Keys.size())
		{
			GE_CORE_ASSERT(false, "ColorGradient::SetKey: index out of range");
			return;
		}

		m_Keys[index] = { time, value };
		SortByTime(m_Keys);
	}

	bool ColorGradient::IsDefault() const
	{
		return m_Keys.size() == 1 && m_Keys[0].Time == 0.0f && m_Keys[0].Value == glm::vec4(1.0f);
	}

	void ColorGradient::ReplaceKeys(std::vector<ColorKey> keys)
	{
		if (keys.empty())
		{
			m_Keys = { { 0.0f, glm::vec4(1.0f) } };
			return;
		}

		m_Keys = std::move(keys);
		SortByTime(m_Keys);
	}

}
