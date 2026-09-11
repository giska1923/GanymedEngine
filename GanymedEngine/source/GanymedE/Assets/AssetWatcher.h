#pragma once

#include "GanymedE/Assets/AssetTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace GanymedE {

	// Edit an asset on disk, see it in the viewport - without restarting, and without a button.
	//
	// This is an **mtime poll over the asset index**, not a platform file watcher, and that is
	// the roadmap's own recommendation rather than a shortcut: `ReadDirectoryChangesW` is ~150
	// lines of Win32 behind an interface, and it buys latency that a quarter-second poll already
	// has. The cost to beat is measured and reported in the editor's Stats panel; the day it
	// shows up in a profile, the poll is replaced by the watcher behind this same interface and
	// nothing above it changes. Do not build both.
	//
	// **What a change actually does is evict, not mutate.** The plan called for re-running
	// parse/apply into the existing object so holders see new contents in place. That is
	// unnecessary here - `AssetRef<T>` already re-resolves after any eviction, which is what the
	// eviction epoch is for - and it is strictly more dangerous: swapping a live `Mesh`'s vertex
	// buffer while a pass has already submitted with it is the mid-frame hazard the plan names as
	// this phase's main risk. Evicting cannot cause it. The old object stays alive and unchanged
	// for as long as anything holds it, and the new one appears at the next `Get()`.
	class AssetWatcher
	{
	public:
		// Watching is off wherever `assets/` is read-only. A shipped runtime has no editor to
		// reload into, its content does not change under it, and the poll would be pure cost.
		static void Init(bool enabled);
		static void Shutdown();

		static void SetEnabled(bool enabled);
		static bool IsEnabled();

		// Called every frame from AssetManager::Update; polls at most every kPollSeconds.
		//
		// **Main thread, at the top of the frame**, before anything reads an asset. That is what
		// makes "reload at a frame boundary" true by construction rather than by discipline: a
		// `Ref` obtained during a frame cannot be evicted out from under that frame.
		static void Poll();

		// Adopt what is on disk right now without reloading any of it. Used when the watcher is
		// switched back on - otherwise re-enabling it after a branch switch would reload every
		// file that moved while it was not looking, which is exactly the storm the switch exists
		// to avoid.
		static void Resync();

		struct Stats
		{
			std::size_t Watched = 0;
			uint32_t Reloads = 0;      // this session
			uint32_t Settling = 0;     // changes seen, not yet acted on (see kSettleSeconds)
			double LastPollMs = 0.0;   // the stat walk only, not the reloads it triggered
			std::string LastReloaded;
		};

		static Stats GetStats();
	};

}
