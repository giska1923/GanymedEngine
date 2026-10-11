#include "gepch.h"
#include "GanymedE/Online/ConnectToken.h"

#include "GanymedE/Online/Json.h"

#include <cstring>

extern "C" {
#include <monocypher-ed25519.h>
}

namespace GanymedE {

	namespace {

		constexpr int64_t kLeewaySeconds = 5;

		// `urlOnly` is the token alphabet: raw base64url, `-` and `_`. The public-key flag also
		// accepts `+` and `/`, because a shell paste is not what the backend mints.
		int Alphabet(char c, bool urlOnly)
		{
			if (c >= 'A' && c <= 'Z') return c - 'A';
			if (c >= 'a' && c <= 'z') return c - 'a' + 26;
			if (c >= '0' && c <= '9') return c - '0' + 52;
			if (c == '-') return 62;
			if (c == '_') return 63;
			if (!urlOnly && c == '+') return 62;
			if (!urlOnly && c == '/') return 63;
			return -1;
		}

		// Unpadded. `out` is appended to. `=` is rejected; the public-key parser strips it first.
		bool DecodeBase64(std::string_view in, std::string& out, bool urlOnly)
		{
			uint32_t buffer = 0;
			int bits = 0;
			for (char c : in)
			{
				if (c == '=' || c == '\n' || c == '\r' || c == ' ')
					return false;

				const int value = Alphabet(c, urlOnly);
				if (value < 0)
					return false;

				buffer = (buffer << 6) | static_cast<uint32_t>(value);
				bits += 6;
				if (bits >= 8)
				{
					bits -= 8;
					out.push_back(static_cast<char>((buffer >> bits) & 0xff));
				}
			}

			// A leftover that is not a whole byte is padding bits, which must be zero. One
			// character cannot carry a byte.
			return bits == 0 || bits == 4 || bits == 2 ? (bits == 0 || ((buffer & ((1u << bits) - 1)) == 0)) : false;
		}

		bool TakeInt(const JsonValue& object, const char* key, int64_t& out)
		{
			const JsonValue* value = object.Find(key);
			if (!value || value->Kind != JsonValue::Type::Integer)
				return false;
			out = value->Integer;
			return true;
		}

		bool TakeString(const JsonValue& object, const char* key, std::string& out)
		{
			const JsonValue* value = object.Find(key);
			if (!value || value->Kind != JsonValue::Type::String)
				return false;
			out = value->String;
			return true;
		}

	}

	bool ParseConnectPublicKey(std::string_view text, uint8_t out[32])
	{
		// Padding and a trailing newline are what a shell paste adds. The token path stays
		// strict: a padded token is not what the backend mints.
		while (!text.empty() && (text.back() == '=' || text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
			text.remove_suffix(1);

		std::string decoded;
		if (!DecodeBase64(text, decoded, /*urlOnly=*/false) || decoded.size() != 32)
			return false;
		std::memcpy(out, decoded.data(), 32);
		return true;
	}

	ConnectResult VerifyConnectToken(const uint8_t publicKey[32], std::string_view token,
		std::string_view serverAddr, std::string_view matchId, int64_t nowUnix)
	{
		ConnectResult result;

		const size_t dot = token.find('.');
		if (dot == std::string_view::npos || dot == 0 || dot + 1 >= token.size() || token.find('.', dot + 1) != std::string_view::npos)
		{
			result.Reason = "malformed connect token";
			return result;
		}

		const std::string_view payload = token.substr(0, dot);
		const std::string_view signatureText = token.substr(dot + 1);

		std::string signature;
		if (!DecodeBase64(signatureText, signature, /*urlOnly=*/true) || signature.size() != 64)
		{
			result.Reason = "malformed connect token";
			return result;
		}

		// The signature covers the encoded payload, the bytes as received, not a re-encoding.
		uint8_t signatureBytes[64];
		std::memcpy(signatureBytes, signature.data(), 64);
		if (crypto_ed25519_check(signatureBytes, publicKey,
			reinterpret_cast<const uint8_t*>(payload.data()), payload.size()) != 0)
		{
			result.Reason = "bad connect token signature";
			return result;
		}

		std::string jsonText;
		if (!DecodeBase64(payload, jsonText, /*urlOnly=*/true))
		{
			result.Reason = "malformed connect token";
			return result;
		}

		std::string parseError;
		const std::optional<JsonValue> json = ParseJson(jsonText, &parseError);
		if (!json || json->Kind != JsonValue::Type::Object)
		{
			result.Reason = "malformed connect token";
			return result;
		}

		int64_t version = 0;
		if (!TakeInt(*json, "v", version) || !TakeInt(*json, "iat", result.Claims.IssuedAt)
			|| !TakeInt(*json, "exp", result.Claims.ExpiresAt)
			|| !TakeString(*json, "match_id", result.Claims.MatchId)
			|| !TakeString(*json, "account_id", result.Claims.AccountId)
			|| !TakeString(*json, "server_addr", result.Claims.ServerAddr)
			|| !TakeString(*json, "nonce", result.Claims.Nonce))
		{
			result.Reason = "malformed connect token";
			return result;
		}

		if (version != 1)
		{
			result.Reason = "unsupported connect token version";
			return result;
		}
		if (nowUnix > result.Claims.ExpiresAt + kLeewaySeconds)
		{
			result.Reason = "connect token expired";
			return result;
		}
		if (nowUnix < result.Claims.IssuedAt - kLeewaySeconds)
		{
			result.Reason = "connect token issued in the future";
			return result;
		}
		if (result.Claims.ServerAddr != serverAddr)
		{
			result.Reason = "connect token is for another server";
			return result;
		}
		if (result.Claims.MatchId != matchId)
		{
			result.Reason = "connect token is for another match";
			return result;
		}
		if (result.Claims.AccountId.empty() || result.Claims.Nonce.empty())
		{
			result.Reason = "malformed connect token";
			return result;
		}

		result.Ok = true;
		return result;
	}

}
