#pragma once

#include <memory>

#include "PlatformDetection.h"

// DLL support
#ifdef GE_PLATFORM_WINDOWS
	#if GE_DYNAMIC_LINK
		#ifdef GE_BUILD_DLL
			#define GE_API __declspec(dllexport)
		#else
			#define GE_API __declspec(dllimport)
		#endif
	#else
		#define GE_API
	#endif
#else
	#error GanymedE only supports Windows!
#endif // End of DLL support

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

#define GE_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

//defaults

#ifdef GE_PLATFORM_WINDOWS
	#define DEFAULT_WINDOW_HEIGHT 900
	#define DEFAULT_WINDOW_WIDTH 1600
#else
	#define DEFAULT_WINDOW_HEIGHT 480
	#define DEFAULT_WINDOW_WIDTH 640
#endif

namespace GanymedE {
	
	template<typename T>
	using Scope = std::unique_ptr<T>;
	template<typename T, typename ... Args>
	constexpr Scope<T> CreateScope(Args&& ... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	template<typename T>
	using Ref = std::shared_ptr<T>;
	template<typename T, typename ... Args>
	constexpr Ref<T> CreateRef(Args&& ... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}
}
