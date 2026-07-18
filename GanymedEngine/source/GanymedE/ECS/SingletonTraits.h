#pragma once

// Deliberately dependency-free so that headers declaring singleton types can specialize this
// without pulling in Singleton.h (which needs Scene, which needs the singleton types).

namespace GanymedE {

	template<typename T>
	struct SingletonTraits
	{
		static constexpr bool TrackChanges = false;   // enables SingletonChangeView on T
	};
}
