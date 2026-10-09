#include "gepch.h"
#include "GanymedE/Online/Json.h"

#include <yaml-cpp/yaml.h>

#include <charconv>
#include <cmath>

namespace GanymedE {

	namespace {

		bool IsJsonInteger(const std::string& s)
		{
			size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
			if (i >= s.size())
				return false;
			if (s[i] == '0')
				return i + 1 == s.size();
			for (; i < s.size(); i++)
			{
				if (s[i] < '0' || s[i] > '9')
					return false;
			}
			return true;
		}

		bool IsJsonNumber(const std::string& s)
		{
			// -?int(.digits)?([eE][+-]?digits)? - the JSON grammar, which is stricter than YAML's:
			// no leading '+', no '.5', no 'inf', no hex.
			size_t i = 0;
			auto digits = [&](bool required)
			{
				const size_t start = i;
				while (i < s.size() && s[i] >= '0' && s[i] <= '9')
					i++;
				return !required || i > start;
			};

			if (i < s.size() && s[i] == '-')
				i++;
			if (i < s.size() && s[i] == '0')
				i++;
			else if (!digits(true))
				return false;
			if (i < s.size() && s[i] == '.')
			{
				i++;
				if (!digits(true))
					return false;
			}
			if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
			{
				i++;
				if (i < s.size() && (s[i] == '+' || s[i] == '-'))
					i++;
				if (!digits(true))
					return false;
			}
			return i == s.size();
		}

		// YAML is a superset of JSON, so the one thing to get right is telling a JSON string from
		// a JSON literal. yaml-cpp tags every quoted scalar "!" (non-specific) and every plain one
		// "?", so "123" and 123, "true" and true, "null" and null stay distinct. Only plain
		// scalars are interpreted, and only as JSON allows: YAML 1.1 would also read `yes`, `on`
		// and `0x1F`, none of which can appear unquoted in JSON.
		bool Convert(const YAML::Node& node, JsonValue& out, std::string& error, int depth)
		{
			if (depth > 128)
			{
				error = "nested too deeply";
				return false;
			}

			switch (node.Type())
			{
				case YAML::NodeType::Null:
					out = JsonValue::MakeNull();
					return true;

				case YAML::NodeType::Scalar:
				{
					const std::string& text = node.Scalar();
					if (node.Tag() == "!")
					{
						out = JsonValue::MakeString(text);
						return true;
					}

					if (text == "true" || text == "false")
					{
						out = JsonValue::MakeBool(text == "true");
						return true;
					}
					if (text == "null")
					{
						out = JsonValue::MakeNull();
						return true;
					}
					if (IsJsonInteger(text))
					{
						int64_t value = 0;
						auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
						if (ec == std::errc() && end == text.data() + text.size())
						{
							out = JsonValue::MakeInteger(value);
							return true;
						}
						// Out of int64's range: falls through to a double, which is what JSON says
						// a number is anyway.
					}
					if (IsJsonNumber(text))
					{
						double value = 0.0;
						auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
						if (ec == std::errc() && end == text.data() + text.size())
						{
							out = JsonValue::MakeNumber(value);
							return true;
						}
					}

					error = "'" + text + "' is not a JSON value";
					return false;
				}

				case YAML::NodeType::Sequence:
				{
					out = JsonValue::MakeArray();
					out.Items.reserve(node.size());
					for (const YAML::Node& item : node)
					{
						out.Items.emplace_back();
						if (!Convert(item, out.Items.back(), error, depth + 1))
							return false;
					}
					return true;
				}

				case YAML::NodeType::Map:
				{
					out = JsonValue::MakeObject();
					for (const auto& member : node)
					{
						if (!member.first.IsScalar())
						{
							error = "an object key is not a string";
							return false;
						}
						out.Members.emplace_back(member.first.Scalar(), JsonValue{});
						if (!Convert(member.second, out.Members.back().second, error, depth + 1))
							return false;
					}
					return true;
				}

				default:
					error = "empty document";
					return false;
			}
		}

		void WriteString(const std::string& s, std::string& out)
		{
			out += '"';
			for (const char c : s)
			{
				switch (c)
				{
					case '"':  out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					case '\b': out += "\\b"; break;
					case '\f': out += "\\f"; break;
					default:
						if (static_cast<unsigned char>(c) < 0x20)
						{
							char escape[8];
							std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(c));
							out += escape;
						}
						else
							out += c;   // UTF-8 passes through: JSON is UTF-8, nothing to escape
				}
			}
			out += '"';
		}

		// 2^53 - 1: the largest n for which every integer in [-n, n] has an exact double.
		constexpr double kMaxExactDouble = 9007199254740991.0;

		bool Write(const JsonValue& value, std::string& out, std::string& error, int depth)
		{
			if (depth > 128)
			{
				error = "nested too deeply";
				return false;
			}

			switch (value.Kind)
			{
				case JsonValue::Type::Null:    out += "null"; return true;
				case JsonValue::Type::Bool:    out += value.Bool ? "true" : "false"; return true;
				case JsonValue::Type::Integer: out += std::to_string(value.Integer); return true;

				case JsonValue::Type::Number:
				{
					const double n = value.Number;
					if (!std::isfinite(n))
					{
						error = "a number is NaN or infinite, which JSON cannot carry";
						return false;
					}

					// A whole-number double is written as an integer: `1234`, never `1234.0` or
					// `1.234e3`. Lua numbers from script properties are floats, and the backend
					// decodes integer fields into int64, which refuses a fraction with a 400. Past
					// 2^53 - 1 the double may already be a rounded neighbour of what the script
					// computed, so it is refused rather than sent as a different number.
					if (std::floor(n) == n)
					{
						if (std::fabs(n) > kMaxExactDouble)
						{
							error = "the whole number " + std::to_string(n) + " is beyond 2^53 - 1 and cannot be sent exactly";
							return false;
						}
						out += std::to_string(static_cast<int64_t>(n));
						return true;
					}

					char buffer[32];
					auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), n);   // shortest round-trip
					if (ec != std::errc())
					{
						error = "could not format a number";
						return false;
					}
					out.append(buffer, end);
					return true;
				}

				case JsonValue::Type::String:
					WriteString(value.String, out);
					return true;

				case JsonValue::Type::Array:
				{
					out += '[';
					for (size_t i = 0; i < value.Items.size(); i++)
					{
						if (i > 0)
							out += ',';
						if (!Write(value.Items[i], out, error, depth + 1))
							return false;
					}
					out += ']';
					return true;
				}

				case JsonValue::Type::Object:
				{
					out += '{';
					for (size_t i = 0; i < value.Members.size(); i++)
					{
						if (i > 0)
							out += ',';
						WriteString(value.Members[i].first, out);
						out += ':';
						if (!Write(value.Members[i].second, out, error, depth + 1))
							return false;
					}
					out += '}';
					return true;
				}
			}
			return false;
		}
	}

	const JsonValue* JsonValue::Find(const std::string& key) const
	{
		if (Kind != Type::Object)
			return nullptr;
		for (const auto& [name, value] : Members)
		{
			if (name == key)
				return &value;
		}
		return nullptr;
	}

	std::optional<JsonValue> ParseJson(const std::string& text, std::string* error)
	{
		std::string reason;

		// YAML reads an empty document as null; JSON says it is not a document at all.
		if (text.find_first_not_of(" \t\r\n") == std::string::npos)
		{
			if (error)
				*error = "empty body";
			return std::nullopt;
		}

		try
		{
			const YAML::Node root = YAML::Load(text);
			JsonValue value;
			if (Convert(root, value, reason, 0))
				return value;
		}
		catch (const YAML::Exception& e)
		{
			reason = e.what();
		}

		if (error)
			*error = reason;
		return std::nullopt;
	}

	bool WriteJson(const JsonValue& value, std::string& out, std::string* error)
	{
		std::string reason;
		std::string text;
		if (!Write(value, text, reason, 0))
		{
			if (error)
				*error = reason;
			return false;
		}
		out = std::move(text);
		return true;
	}
}
