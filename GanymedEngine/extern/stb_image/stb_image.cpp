#include "gepch.h"

// This stb_image version has an SSE2 bug (non-immediate _mm_slli_si128 argument)
// that newer GCC/Clang reject; the scalar path is correct everywhere
#ifndef _MSC_VER
	#define STBI_NO_SIMD
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"