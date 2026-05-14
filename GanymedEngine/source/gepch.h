#pragma once

#include "GanymedE/Core/PlatformDetection.h"

#ifdef GE_PLATFORM_WINDOWS
	#ifndef NOMINMAX
		// See github.com/skypjack/entt/wiki/Frequently-Asked-Questions#warning-c4003-the-min-the-max-and-the-macro
		#define NOMINMAX
	#endif
#endif

// Platform-specific includes
#ifdef GE_PLATFORM_LINUX
	#include <unistd.h>
	#include <signal.h>
#endif

#ifdef GE_PLATFORM_MACOS
	#include <unistd.h>
	#include <signal.h>
#endif

#include <iostream>
#include <memory>
#include <utility>
#include <algorithm>
#include <functional>

#include <string>
#include <sstream>
#include <array>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "GanymedE/Core/Core.h"

#include "GanymedE/Core/Log.h"

#include "GanymedE/Debug/Instrumentor.h"

#ifdef GE_PLATFORM_WINDOWS
	#include <Windows.h>
#endif
