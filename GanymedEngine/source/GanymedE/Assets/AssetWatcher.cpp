#include "gepch.h"
#include "GanymedE/Assets/AssetWatcher.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"

#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace GanymedE {

	namespace {

		using Clock = std::chrono::steady_clock;

		// Fast enough that a save feels immediate, slow enough that the stat walk stays
		// invisible. The measured cost is in docs/engine/assets.md; the editor shows it live so
		// the day it stops being free is the day you find out.
		constexpr double kPollSeconds = 0.25;

		// A change has to still be there, unchanged, on a later poll before it counts.
		//
		// This is the debounce, and it is not only about coalescing bursts. Editors save by
		// writing a temp file and renaming, sometimes touching the result again; reading one
		// mid-write parses into a *failure*, and a failed load is remembered until something
		// evicts it. Waiting for the writer to stop is what keeps a hot reload from turning a
		// good asset into a broken one.
		constexpr double kSettleSeconds = 0.2;

		// A `git checkout` across a texture-heavy branch changes hundreds of files at once, and
		// every accepted change queues a parse that holds its compiled bytes until Apply lands.
		// Unbounded, the whole set would be in flight simultaneously - which for 2K BC7 textures
		// is gigabytes. Spreading them over consecutive polls costs a quarter second per batch
		// and bounds the peak. The same unbounded-in-flight property is true of a cold open and
		// is not new here; this only declines to make it easy to trigger.
		constexpr uint32_t kMaxReloadsPerPoll = 16;

		// A file's identity for staleness purposes, and deliberately *not* a content hash:
		// deciding whether the bytes really changed is `CompiledCache`'s job, one layer down,
		// where the epoch record already does it. Here the only question is "did anything move",
		// and (mtime, size) answers it in a couple of microseconds per file. A save that lands
		// identical bytes gets as far as an eviction and then costs a hash rather than a
		// recompile - which is the property that makes polling on mtime acceptable at all.
		struct Stamp
		{
			uint64_t Mtime = 0;   // zero also means "not on disk"
			uint64_t Size = 0;

			bool operator==(const Stamp& other) const
			{
				return Mtime == other.Mtime && Size == other.Size;
			}

			bool operator!=(const Stamp& other) const { return !(*this == other); }
			bool Exists() const { return Mtime != 0; }
		};

		struct Entry
		{
			std::string Path;        // asset-root-relative, kept for the log line
			Stamp Known;             // what the resident asset was built from
			Stamp Candidate;         // a change seen but not yet settled
			bool Settling = false;
			Clock::time_point FirstSeen;
		};

		struct WatcherData
		{
			bool Enabled = false;
			Clock::time_point LastPoll;

			// Keyed by handle rather than by path, so a rename is two events (one file gone, one
			// arrived) instead of one confused one. Grows with the index and never shrinks,
			// which matches the index itself: a handle a scene references is never dropped.
			std::unordered_map<AssetHandle, Entry> Entries;

			uint32_t Reloads = 0;
			uint32_t Settling = 0;
			double LastPollMs = 0.0;
			std::string LastReloaded;
		};

		WatcherData s_Data;

		Stamp StampOf(const std::filesystem::path& fullPath)
		{
			std::error_code ec;
			const auto time = std::filesystem::last_write_time(fullPath, ec);
			if (ec)
				return {};   // gone, or unreadable, which for this purpose is the same thing

			Stamp stamp;
			stamp.Mtime = (uint64_t)time.time_since_epoch().count();

			// A file whose clock genuinely reads zero would be indistinguishable from a missing
			// one. Vanishingly unlikely, and one bit is cheaper than a second flag.
			if (stamp.Mtime == 0)
				stamp.Mtime = 1;

			const auto size = std::filesystem::file_size(fullPath, ec);
			stamp.Size = ec ? 0 : (uint64_t)size;
			return stamp;
		}

	}

	void AssetWatcher::Init(bool enabled)
	{
		s_Data = WatcherData{};
		s_Data.Enabled = enabled;
		s_Data.LastPoll = Clock::now();

		if (!enabled)
			return;

		// Entries are adopted lazily, on the first poll that sees each index row. That is what
		// makes a file created *after* the scan - an extracted `.glb` texture, a generated
		// `.gmat` - start being watched with no registration call anywhere.
		GE_CORE_TRACE("Asset hot reload is on (polling assets/ every {0:.2f} s)", kPollSeconds);
	}

	void AssetWatcher::Shutdown()
	{
		s_Data = WatcherData{};
	}

	bool AssetWatcher::IsEnabled() { return s_Data.Enabled; }

	void AssetWatcher::SetEnabled(bool enabled)
	{
		if (s_Data.Enabled == enabled)
			return;

		s_Data.Enabled = enabled;

		if (enabled)
		{
			// Everything that moved while the watcher was off is adopted, not reloaded. The
			// switch exists to survive a branch switch; turning it back on and immediately
			// reloading the whole branch would defeat it.
			Resync();
			GE_CORE_INFO("Asset hot reload enabled - adopted the current state of assets/ without reloading");
		}
		else
		{
			GE_CORE_INFO("Asset hot reload disabled");
		}
	}

	void AssetWatcher::Resync()
	{
		AssetManager::ForEachAsset([](const AssetMetadata& metadata)
		{
			Entry& entry = s_Data.Entries[metadata.Handle];
			entry.Path = metadata.FilePath;
			entry.Known = StampOf(GetAssetRoot() / metadata.FilePath);
			entry.Settling = false;
		});

		s_Data.Settling = 0;
	}

	void AssetWatcher::Poll()
	{
		if (!s_Data.Enabled)
			return;

		const Clock::time_point now = Clock::now();
		if (std::chrono::duration<double>(now - s_Data.LastPoll).count() < kPollSeconds)
			return;

		s_Data.LastPoll = now;

		GE_PROFILE_FUNCTION();

		// Settled changes are collected during the walk and acted on after it. Acting inline
		// would evict from inside the iteration - and eviction reaches into *other* managers'
		// caches, which is one refactor away from being this one.
		std::vector<AssetHandle> changed;
		uint32_t settling = 0;

		AssetManager::ForEachAsset([&](const AssetMetadata& metadata)
		{
			auto it = s_Data.Entries.find(metadata.Handle);
			if (it == s_Data.Entries.end())
			{
				// First sight: adopt what is on disk rather than calling it a change. The asset
				// has either not been loaded yet, or was loaded from exactly these bytes.
				Entry fresh;
				fresh.Path = metadata.FilePath;
				fresh.Known = StampOf(GetAssetRoot() / metadata.FilePath);
				s_Data.Entries.emplace(metadata.Handle, std::move(fresh));
				return;
			}

			Entry& entry = it->second;
			const Stamp current = StampOf(GetAssetRoot() / metadata.FilePath);

			if (current == entry.Known)
			{
				// Includes a file that changed and changed back inside the settle window, which
				// is what a branch switch landing on the same content looks like.
				entry.Settling = false;
				return;
			}

			if (!entry.Settling || current != entry.Candidate)
			{
				// New, or it moved again while settling - restart the timer either way. A file
				// being written in bursts keeps landing here until the writer stops.
				entry.Settling = true;
				entry.Candidate = current;
				entry.FirstSeen = now;
				++settling;
				return;
			}

			if (std::chrono::duration<double>(now - entry.FirstSeen).count() < kSettleSeconds)
			{
				++settling;
				return;
			}

			changed.push_back(metadata.Handle);
		});

		s_Data.Settling = settling;

		// The walk only. What the reloads below cost belongs to the asset layer's own counters,
		// and mixing them would make the number that answers "is polling free?" unreadable.
		s_Data.LastPollMs = std::chrono::duration<double, std::milli>(Clock::now() - now).count();

		uint32_t acted = 0;
		for (AssetHandle handle : changed)
		{
			// Over budget: the entry keeps its settled state and is picked up on the next poll.
			if (acted >= kMaxReloadsPerPoll)
			{
				s_Data.Settling += (uint32_t)changed.size() - acted;
				break;
			}

			Entry& entry = s_Data.Entries[handle];
			entry.Known = entry.Candidate;
			entry.Settling = false;
			++acted;

			// The new stamp is accepted before the reload is attempted, not after. A source that
			// is now broken must not be retried every poll forever; the next real edit moves the
			// stamp again, which is what makes a mid-write read recoverable.
			if (!AssetManager::OnAssetModified(handle))
				continue;

			++s_Data.Reloads;
			s_Data.LastReloaded = entry.Path;

			GE_CORE_INFO("Hot reload: '{0}' {1}", entry.Path,
				entry.Known.Exists() ? "changed on disk" : "was deleted");
		}
	}

	AssetWatcher::Stats AssetWatcher::GetStats()
	{
		Stats stats;
		stats.Watched = s_Data.Entries.size();
		stats.Reloads = s_Data.Reloads;
		stats.Settling = s_Data.Settling;
		stats.LastPollMs = s_Data.LastPollMs;
		stats.LastReloaded = s_Data.LastReloaded;
		return stats;
	}

}
