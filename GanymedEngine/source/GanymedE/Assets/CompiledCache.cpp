#include "gepch.h"
#include "GanymedE/Assets/CompiledCache.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Core/JobSystem.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace GanymedE {

	namespace {

		constexpr uint32_t DEP_MAGIC = 0x50454447;   // 'GDEP'
		constexpr uint32_t DEP_VERSION = 1;

		// FNV-1a, 64-bit. Hand-rolled rather than pulled from bx: it is nine lines, it has to
		// stay byte-identical across builds forever (every `.dep` on disk depends on it), and a
		// vendored hash that changed implementation under a version bump would silently
		// invalidate every compiled artifact in every project.
		//
		// Not cryptographic and does not need to be: the adversary here is an edited file, not
		// an attacker choosing collisions.
		uint64_t HashBytes(const void* data, std::size_t size, uint64_t seed = 0xcbf29ce484222325ull)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			uint64_t hash = seed;
			for (std::size_t i = 0; i < size; i++)
			{
				hash ^= bytes[i];
				hash *= 0x100000001b3ull;
			}
			return hash;
		}

		uint64_t HashString(const std::string& str, uint64_t seed = 0xcbf29ce484222325ull)
		{
			return HashBytes(str.data(), str.size(), seed);
		}

		uint64_t HashConfig(const AssetConfig& config)
		{
			// std::map iterates in key order, so the hash does not depend on insertion order -
			// which matters because the sidecar reader and a hand-edited file can produce the
			// same settings in a different order.
			uint64_t hash = 0xcbf29ce484222325ull;
			for (const auto& [key, value] : config)
			{
				hash = HashString(key, hash);
				hash = HashString(value, hash);
			}
			return hash;
		}

		uint64_t FileTimestamp(const std::filesystem::path& path)
		{
			std::error_code ec;
			auto time = std::filesystem::last_write_time(path, ec);
			if (ec)
				return 0;
			return (uint64_t)time.time_since_epoch().count();
		}

		uint64_t FileSize(const std::filesystem::path& path)
		{
			std::error_code ec;
			auto size = std::filesystem::file_size(path, ec);
			return ec ? 0 : (uint64_t)size;
		}

		bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out)
		{
			std::ifstream in(path, std::ios::binary | std::ios::ate);
			if (!in)
				return false;

			const std::streamsize size = in.tellg();
			if (size < 0)
				return false;

			in.seekg(0, std::ios::beg);
			out.resize((std::size_t)size);
			if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size))
				return false;

			return true;
		}

		// The record decision 10 asks for: everything that can make an output stale, so the
		// answer to "recompile?" is a comparison rather than a guess.
		struct Epoch
		{
			uint32_t CompilerVersion = 0;
			uint64_t SourceSize = 0;
			uint64_t SourceMtime = 0;
			uint64_t SourceHash = 0;
			uint64_t ConfigHash = 0;

			struct Dependency
			{
				std::string Path;   // asset-root-relative
				uint64_t Hash = 0;
			};
			std::vector<Dependency> Dependencies;

			// Reported, never compared. Kept because Phase 4's `.dep` is already the place a
			// statistics panel would read compile cost from, per the roadmap's "not a phase".
			double CompileMs = 0.0;
			uint64_t OutputSize = 0;
		};

		template<typename T>
		void Write(std::ostream& out, const T& value)
		{
			static_assert(std::is_trivially_copyable<T>::value, "raw write");
			out.write(reinterpret_cast<const char*>(&value), sizeof(T));
		}

		template<typename T>
		void Read(std::istream& in, T& value)
		{
			static_assert(std::is_trivially_copyable<T>::value, "raw read");
			in.read(reinterpret_cast<char*>(&value), sizeof(T));
		}

		void WriteStr(std::ostream& out, const std::string& str)
		{
			Write(out, (uint32_t)str.size());
			out.write(str.data(), (std::streamsize)str.size());
		}

		std::string ReadStr(std::istream& in)
		{
			uint32_t size = 0;
			Read(in, size);

			// A truncated or garbage `.dep` must not turn into a multi-gigabyte allocation.
			// Anything past a sane path length means the file is not what it claims to be.
			if (size > 4096)
				return {};

			std::string str(size, '\0');
			if (size > 0)
				in.read(str.data(), size);
			return str;
		}

		bool ReadEpoch(const std::filesystem::path& path, Epoch& out)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return false;

			uint32_t magic = 0, version = 0;
			Read(in, magic);
			Read(in, version);
			if (magic != DEP_MAGIC || version != DEP_VERSION)
				return false;

			Read(in, out.CompilerVersion);
			Read(in, out.SourceSize);
			Read(in, out.SourceMtime);
			Read(in, out.SourceHash);
			Read(in, out.ConfigHash);
			Read(in, out.CompileMs);
			Read(in, out.OutputSize);

			uint32_t depCount = 0;
			Read(in, depCount);
			if (depCount > 4096)
				return false;

			out.Dependencies.resize(depCount);
			for (Epoch::Dependency& dep : out.Dependencies)
			{
				dep.Path = ReadStr(in);
				Read(in, dep.Hash);
			}

			return (bool)in;
		}

		bool WriteEpoch(const std::filesystem::path& path, const Epoch& epoch)
		{
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);

			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
				return false;

			Write(out, DEP_MAGIC);
			Write(out, DEP_VERSION);
			Write(out, epoch.CompilerVersion);
			Write(out, epoch.SourceSize);
			Write(out, epoch.SourceMtime);
			Write(out, epoch.SourceHash);
			Write(out, epoch.ConfigHash);
			Write(out, epoch.CompileMs);
			Write(out, epoch.OutputSize);
			Write(out, (uint32_t)epoch.Dependencies.size());

			for (const Epoch::Dependency& dep : epoch.Dependencies)
			{
				WriteStr(out, dep.Path);
				Write(out, dep.Hash);
			}

			return (bool)out;
		}

		uint64_t HashDependency(const std::string& relativePath)
		{
			// Size and mtime rather than content: a dependency hash is checked on every load of
			// every dependent, and hashing a 4K normal map's bytes to answer "did it change"
			// costs more than the recompile it is trying to avoid. The source itself is content
			// hashed; its dependencies are not, and that asymmetry is deliberate.
			const std::filesystem::path full = GetAssetRoot() / relativePath;

			uint64_t hash = HashString(relativePath);
			for (uint64_t part : { FileTimestamp(full), FileSize(full) })
				hash ^= part + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);

			return hash;
		}

		struct CompiledCacheData
		{
			// Written only at Init, read from workers afterwards. Not atomic and does not need
			// to be: registration happens before any manager can queue a parse.
			std::array<Scope<IAssetCompiler>, 16> Compilers;   // indexed by AssetType ordinal

			// Atomic because Open runs on workers and several can finish at once.
			std::atomic<uint32_t> Compiles{ 0 };
			std::atomic<uint32_t> CacheHits{ 0 };
			std::atomic<uint64_t> TotalCompileMicros{ 0 };
			std::atomic<uint32_t> Compiling{ 0 };
			std::atomic_flag WarnedAboutReadOnlyCompile = ATOMIC_FLAG_INIT;
		};

		CompiledCacheData s_Data;

	}

	std::string EpochDiffToString(EpochDiff diff)
	{
		if (diff == EpochDiff::None)
			return "none";

		std::string out;
		auto append = [&out](const char* name)
		{
			if (!out.empty())
				out += '|';
			out += name;
		};

		if (HasDiff(diff, EpochDiff::CompilerVersion)) append("compiler-version");
		if (HasDiff(diff, EpochDiff::SourceSize))      append("source-size");
		if (HasDiff(diff, EpochDiff::SourceMtime))     append("source-mtime");
		if (HasDiff(diff, EpochDiff::SourceHash))      append("source-hash");
		if (HasDiff(diff, EpochDiff::Dependencies))    append("dependencies");
		if (HasDiff(diff, EpochDiff::Config))          append("config");
		return out;
	}

	void CompiledCache::RegisterCompiler(AssetType type, Scope<IAssetCompiler> compiler)
	{
		const std::size_t index = (std::size_t)type;
		GE_CORE_ASSERT(index < s_Data.Compilers.size(), "AssetType ordinal past the compiler table");

		s_Data.Compilers[index] = std::move(compiler);
	}

	void CompiledCache::Shutdown()
	{
		for (Scope<IAssetCompiler>& compiler : s_Data.Compilers)
			compiler.reset();

		ResetStats();
	}

	const IAssetCompiler* CompiledCache::CompilerFor(AssetType type)
	{
		const std::size_t index = (std::size_t)type;
		if (index >= s_Data.Compilers.size())
			return nullptr;
		return s_Data.Compilers[index].get();
	}

	std::filesystem::path CompiledCache::OutputPath(const std::string& relativeSourcePath)
	{
		const uint64_t hash = HashString(relativeSourcePath);

		char name[17] = {};
		std::snprintf(name, sizeof(name), "%016llx", (unsigned long long)hash);

		// Two-character bucket: 256 directories, so a project with tens of thousands of assets
		// never puts more than a few hundred files in one directory. Windows and ext4 both get
		// slow enough at ~100k entries per directory to matter, and this costs one substring.
		return GetAssetRoot() / ".compiled" / std::string(name, name + 2) / (std::string(name) + ".gres");
	}

	CompiledCache::Stats CompiledCache::GetStats()
	{
		Stats stats;
		stats.Compiles = s_Data.Compiles.load(std::memory_order_relaxed);
		stats.CacheHits = s_Data.CacheHits.load(std::memory_order_relaxed);
		stats.TotalCompileMs = s_Data.TotalCompileMicros.load(std::memory_order_relaxed) / 1000.0;
		return stats;
	}

	void CompiledCache::ResetStats()
	{
		s_Data.Compiles.store(0, std::memory_order_relaxed);
		s_Data.CacheHits.store(0, std::memory_order_relaxed);
		s_Data.TotalCompileMicros.store(0, std::memory_order_relaxed);
	}

	uint32_t CompiledCache::CompilesInFlight() { return s_Data.Compiling.load(std::memory_order_relaxed); }

	bool CompiledCache::Invalidate(const AssetMetadata& metadata)
	{
		const std::filesystem::path output = OutputPath(metadata.FilePath);
		std::filesystem::path dep = output;
		dep.replace_extension(".dep");

		std::error_code ec;
		const bool removedOutput = std::filesystem::remove(output, ec);
		std::filesystem::remove(dep, ec);

		if (removedOutput)
			GE_CORE_INFO("Invalidated compiled output for '{0}'", metadata.FilePath);

		return removedOutput;
	}

	bool CompiledCache::Open(const AssetMetadata& metadata, std::vector<uint8_t>& out)
	{
		GE_PROFILE_FUNCTION();

		const std::filesystem::path sourceFull = GetAssetRoot() / metadata.FilePath;
		const IAssetCompiler* compiler = CompilerFor(metadata.Type);

		// No compiler for this type: the source *is* the compiled form. Environments take this
		// path - their expensive step is a GPU bake that cannot be precomputed into bytes.
		if (!compiler)
			return ReadFileBytes(sourceFull, out);

		if (!std::filesystem::exists(sourceFull))
		{
			GE_CORE_WARN("Cannot compile '{0}': the source file is not there", metadata.FilePath);
			return false;
		}

		const std::filesystem::path output = OutputPath(metadata.FilePath);
		std::filesystem::path depPath = output;
		depPath.replace_extension(".dep");

		Epoch current;
		current.CompilerVersion = compiler->Version();
		current.SourceSize = FileSize(sourceFull);
		current.SourceMtime = FileTimestamp(sourceFull);
		current.ConfigHash = HashConfig(metadata.Config);

		Epoch previous;
		const bool havePrevious = std::filesystem::exists(output) && ReadEpoch(depPath, previous);

		EpochDiff diff = EpochDiff::None;
		std::vector<uint8_t> sourceBytes;
		bool sourceRead = false;

		if (havePrevious)
		{
			if (current.CompilerVersion != previous.CompilerVersion) diff |= EpochDiff::CompilerVersion;
			if (current.SourceSize != previous.SourceSize)           diff |= EpochDiff::SourceSize;
			if (current.SourceMtime != previous.SourceMtime)         diff |= EpochDiff::SourceMtime;
			if (current.ConfigHash != previous.ConfigHash)           diff |= EpochDiff::Config;

			for (const Epoch::Dependency& dep : previous.Dependencies)
			{
				if (HashDependency(dep.Path) != dep.Hash)
				{
					diff |= EpochDiff::Dependencies;
					break;
				}
			}

			// mtime and size are the cheap pre-filter, nothing more. A file that was touched,
			// or rewritten with identical bytes by an editor's save-to-temp-and-rename, moves
			// mtime without changing content - and hashing is what tells those apart. This is
			// the whole reason the epoch record exists instead of MeshCache's mtime compare.
			if (HasDiff(diff, EpochDiff::SourceMtime) || HasDiff(diff, EpochDiff::SourceSize))
			{
				sourceRead = ReadFileBytes(sourceFull, sourceBytes);
				current.SourceHash = sourceRead ? HashBytes(sourceBytes.data(), sourceBytes.size()) : 0;
				if (current.SourceHash != previous.SourceHash)
					diff |= EpochDiff::SourceHash;
			}
			else
			{
				current.SourceHash = previous.SourceHash;
			}
		}

		// A moved mtime on identical content is *not* a reason to recompile - it is a reason to
		// refresh the record so the next boot short-circuits on the cheap compare again.
		const EpochDiff recompileFlags = EpochDiff::CompilerVersion | EpochDiff::SourceSize
			| EpochDiff::SourceHash | EpochDiff::Dependencies | EpochDiff::Config;
		const bool stale = !havePrevious || ((uint32_t)diff & (uint32_t)recompileFlags) != 0;

		if (!stale)
		{
			if (ReadFileBytes(output, out))
			{
				if (HasDiff(diff, EpochDiff::SourceMtime))
				{
					current.Dependencies = previous.Dependencies;
					current.CompileMs = previous.CompileMs;
					current.OutputSize = previous.OutputSize;
					WriteEpoch(depPath, current);
					GE_CORE_TRACE("'{0}': mtime moved but content is unchanged - kept the compiled output",
						metadata.FilePath);
				}

				s_Data.CacheHits.fetch_add(1, std::memory_order_relaxed);
				return true;
			}

			GE_CORE_WARN("Compiled output for '{0}' could not be read - recompiling", metadata.FilePath);
		}

		if (!sourceRead && !ReadFileBytes(sourceFull, sourceBytes))
		{
			GE_CORE_ERROR("Cannot read source '{0}'", metadata.FilePath);
			return false;
		}

		if (current.SourceHash == 0)
			current.SourceHash = HashBytes(sourceBytes.data(), sourceBytes.size());

		// The cheapest cancellation point there is, and the one that matters most: a cold open
		// queues every asset at once, so most cancelled parses have not started compiling yet
		// and stop here for free. A compile already running polls between mips instead.
		if (JobSystem::IsCurrentJobCancelled())
			return false;

		CompileInput input;
		input.Metadata = &metadata;
		input.SourceFullPath = sourceFull;
		input.SourceBytes = &sourceBytes;
		input.Config = &metadata.Config;

		CompileOutput result;

		const auto start = std::chrono::steady_clock::now();
		s_Data.Compiling.fetch_add(1, std::memory_order_relaxed);
		const bool ok = compiler->Compile(input, result);
		s_Data.Compiling.fetch_sub(1, std::memory_order_relaxed);
		result.CompileMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();

		if (!ok || result.Bytes.empty())
		{
			GE_CORE_ERROR("{0} failed on '{1}'", compiler->Name(), metadata.FilePath);
			return false;
		}

		// Persisting is best-effort from here on: the bytes are already correct, and an install
		// that cannot write its own directory should still *run*. Every failure below warns and
		// falls through to returning the in-memory result.
		std::error_code ec;
		std::filesystem::create_directories(output.parent_path(), ec);

		// Same write-then-rename discipline as the `.meta` sidecar: an interrupted write must
		// not leave a truncated `.gres` that parses as a valid but wrong asset. The `.dep` is
		// written *after* the output, so a crash between them costs a recompile rather than a
		// record claiming an output that is not there.
		bool persisted = false;
		const std::filesystem::path temp = output.string() + ".tmp";
		{
			std::ofstream file(temp, std::ios::binary | std::ios::trunc);
			persisted = file && (bool)file.write(reinterpret_cast<const char*>(result.Bytes.data()),
				(std::streamsize)result.Bytes.size());
		}

		if (persisted)
		{
			std::filesystem::rename(temp, output, ec);
			persisted = !ec;
			if (!persisted)
				std::filesystem::remove(temp, ec);
		}

		if (persisted)
		{
			current.CompileMs = result.CompileMs;
			current.OutputSize = (uint64_t)result.Bytes.size();
			current.Dependencies.reserve(result.Dependencies.size());
			for (const std::string& dep : result.Dependencies)
				current.Dependencies.push_back({ dep, HashDependency(dep) });

			WriteEpoch(depPath, current);
		}
		else
		{
			GE_CORE_WARN("Compiled '{0}', but the result could not be written to "
				"assets/.compiled - it will be compiled again on the next run.", metadata.FilePath);
		}

		s_Data.Compiles.fetch_add(1, std::memory_order_relaxed);
		s_Data.TotalCompileMicros.fetch_add((uint64_t)(result.CompileMs * 1000.0),
			std::memory_order_relaxed);

		GE_CORE_INFO("Compiled '{0}' with {1} in {2:.0f} ms ({3} KB -> {4} KB){5}",
			metadata.FilePath, compiler->Name(), result.CompileMs,
			current.SourceSize / 1024, (uint64_t)result.Bytes.size() / 1024,
			havePrevious ? " [stale: " + EpochDiffToString(diff) + "]" : " [no cached output]");

		// A shipped game compiling anything means its compiled tree did not travel with it. It
		// still works - that is what the fallback above is for - but every boot pays for it, and
		// on a first run of a large project that is minutes. Said once, because a whole project
		// missing its outputs would otherwise say it per asset.
		if (!AssetManager::IsRegistryWritable() && !s_Data.WarnedAboutReadOnlyCompile.test_and_set())
		{
			GE_CORE_WARN("This install treats assets/ as read-only but had to compile "
				"'{0}' - the assets/.compiled tree was not shipped with it. Compiled output is "
				"derived, but it is derived *at build time*: shipping without it means every boot "
				"recompiles everything.", metadata.FilePath);
		}

		out = std::move(result.Bytes);
		return true;
	}

}
