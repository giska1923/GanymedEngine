#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace GanymedE {

	// A JSON value: what a backend response parses into and what a request body is written from.
	//
	// A plain struct rather than a variant on purpose - it is recursive (arrays and objects of
	// JsonValue), it is small, and every consumer switches on Type anyway. Objects keep their
	// members in document order, which is what a reader of a logged body expects.
	struct JsonValue
	{
		enum class Type { Null, Bool, Integer, Number, String, Array, Object };

		Type Kind = Type::Null;
		bool Bool = false;
		int64_t Integer = 0;   // a JSON number with no fraction or exponent that fits int64
		double Number = 0.0;   // any other JSON number
		std::string String;
		std::vector<JsonValue> Items;                              // Array
		std::vector<std::pair<std::string, JsonValue>> Members;    // Object

		static JsonValue MakeNull() { return {}; }
		static JsonValue MakeBool(bool v) { JsonValue j; j.Kind = Type::Bool; j.Bool = v; return j; }
		static JsonValue MakeInteger(int64_t v) { JsonValue j; j.Kind = Type::Integer; j.Integer = v; return j; }
		static JsonValue MakeNumber(double v) { JsonValue j; j.Kind = Type::Number; j.Number = v; return j; }
		static JsonValue MakeString(std::string v) { JsonValue j; j.Kind = Type::String; j.String = std::move(v); return j; }
		static JsonValue MakeArray() { JsonValue j; j.Kind = Type::Array; return j; }
		static JsonValue MakeObject() { JsonValue j; j.Kind = Type::Object; return j; }

		// The member's value, or null if this is not an object or has no such member.
		const JsonValue* Find(const std::string& key) const;
	};

	// Parses with yaml-cpp, which is already vendored: JSON as a Go service emits it is YAML 1.2
	// flow syntax. Returns nullopt on anything that is not JSON, with the reason in `error`.
	std::optional<JsonValue> ParseJson(const std::string& text, std::string* error = nullptr);

	// Compact JSON. Fails, with the reason in `error`, on a value JSON cannot carry exactly: a
	// non-finite number, or a whole-number double outside +-(2^53 - 1) (see Json.cpp).
	bool WriteJson(const JsonValue& value, std::string& out, std::string* error = nullptr);
}
