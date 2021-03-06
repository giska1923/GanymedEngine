#pragma once

#if defined(GE_PLATFORM_WINDOWS) && defined(GE_DYNAMIC_LINK)
	#ifdef GE_BUILD_DLL
		#define GE_API __declspec(dllexport)
	#else
		#define GE_API __declspec(dllimport)
	#endif
#else
	#define GE_API
#endif

#ifdef GE_DEBUG
#define GE_ENABLE_ASSERTS
#endif

#ifdef GE_ENABLE_ASSERTS
#define GE_ASSERT(x, ...) { if(!(x)) { GE_ERROR("Assertion failed: {0}", __VA_ARGS__); __debugbreak(); } }
#define GE_CORE_ASSERT(x, ...) { if(!(x)) { GE_CORE_ERROR("Assertion failed: {0}", __VA_ARGS__); __debugbreak(); } }
#else
#define GE_ASSERT(x, ...)
#define GE_CORE_ASSERT(x, ...)
#endif

//basics...

#define BIT(x) (1 << x)

//to bind some function targeted with x and caller y (mainly this) as an std::function
#define BIND_CALLBACK_FN(x,y) std::bind(&x, y, std::placeholders::_1)

//defaults

#ifdef GE_PLATFORM_WINDOWS
	#define DEFAULT_WINDOW_HEIGHT 720
	#define DEFAULT_WINDOW_WIDTH 1280
#else
	#define DEFAULT_WINDOW_HEIGHT 480
	#define DEFAULT_WINDOW_WIDTH 640
#endif