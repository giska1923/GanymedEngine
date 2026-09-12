// **No gepch.h.** This TU is excluded from the precompiled header in premake5.lua - see the
// comment there for why the PCH breaks dr_FLAC's SSE4.1 path on GCC. It needs nothing from the
// PCH anyway: the only include below is miniaudio's own.

// The one and only implementation TU for miniaudio's single header. It sits alone in
// its own file so the ~84k-line implementation is preprocessed once per build rather
// than once per consumer - AudioEngine.cpp includes the same header for declarations
// only, and stays cheap to iterate on.
//
// Nothing else in the engine may define MINIAUDIO_IMPLEMENTATION.
#define MINIAUDIO_IMPLEMENTATION

// Third-party single header at the engine's warning level; the Log.h/spdlog precedent.
#if defined(_MSC_VER)
	#pragma warning(push, 0)
#endif

#include <miniaudio.h>

#if defined(_MSC_VER)
	#pragma warning(pop)
#endif
