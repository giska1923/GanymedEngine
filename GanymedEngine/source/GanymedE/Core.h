#pragma once

#ifdef GE_PLATFORM_WINDOWS
	#ifdef GE_BUILD_DLL
		#define GE_API __declspec(dllexport)
	#else
		#define GE_API __declspec(dllimport)
	#endif
#else
	#error GanymedEngine only supports Windows!
#endif

#ifdef GE_ENABLE_ASSERTS
#define GE_ASSERT(x, ...) { if(!(x)) { GE_ERROR("Assertion failed: {0}", __VA_ARGS__); __debugbreak(); } }
#define GE_CORE_ASSERT(x, ...) { if(!(x)) { GE_CORE_ERROR("Assertion failed: {0}", __VA_ARGS__); __debugbreak(); } }
#else
#define GE_ASSERT(x, ...)
#define GE_CORE_ASSERT(x, ...)
#endif

//basic maths...

#define BIT(x) (1 << x)

//defaults

#ifdef GE_PLATFORM_WINDOWS
	#define DEFAULT_WINDOW_HEIGHT 720
	#define DEFAULT_WINDOW_WIDTH 1280
#else
	#define DEFAULT_WINDOW_HEIGHT 480
	#define DEFAULT_WINDOW_WIDTH 640
#endif