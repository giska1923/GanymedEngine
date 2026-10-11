#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace GanymedE {

	// connect-token.md, rules 1–6. Rules 7 and 8 need the allocation and a nonce map, so they
	// stay with the server. The reason strings are the backend's, so a DENIED line matches
	// what stubserver would have said. Nothing here logs the token.
	struct ConnectClaims
	{
		std::string MatchId;
		std::string AccountId;
		std::string ServerAddr;
		int64_t IssuedAt = 0;
		int64_t ExpiresAt = 0;
		std::string Nonce;
	};

	struct ConnectResult
	{
		bool Ok = false;
		ConnectClaims Claims;
		const char* Reason = nullptr;   // static, or null when Ok
	};

	// `nowUnix` is the server's clock in Unix seconds. Leeway is 5 s, the contract's.
	ConnectResult VerifyConnectToken(const uint8_t publicKey[32], std::string_view token,
		std::string_view serverAddr, std::string_view matchId, int64_t nowUnix);

	// --public-key, 32 bytes. The agent's encoder is raw base64url. A shell paste may use the
	// standard alphabet or padding; those are stripped here. A connect token is not: `+`, `/`
	// and `=` are malformed, matching Go's RawURLEncoding.
	bool ParseConnectPublicKey(std::string_view text, uint8_t out[32]);

}
