#include "gepch.h"
#include "GanymedE/Scripting/ScriptEngine.h"
#include "GanymedE/Scripting/ScriptBindings.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

namespace GanymedE {

	namespace {

		struct ScriptInstance
		{
			sol::table Self;
			AssetHandle Script = InvalidAssetHandle;
			std::string Path;            // for diagnostics; the handle alone means nothing to a human

			// Set on the first failed protected call. A script that throws in OnUpdate would
			// otherwise log the identical error every frame forever - 60 lines a second of noise
			// that buries whatever went wrong first. One log, then the instance goes quiet.
			bool Dead = false;
		};

		struct ScriptEngineData
		{
			sol::state Lua;
			Scene* SceneContext = nullptr;

			// Chunk result per script asset, executed once per load. Cleared on runtime stop, which
			// is the cheapest hot reload that exists: every Play press re-reads scripts from disk.
			struct ScriptClass
			{
				sol::table Table;
				std::filesystem::path Path;
				std::filesystem::file_time_type Stamp{};
			};
			std::unordered_map<AssetHandle, ScriptClass> ScriptClasses;

			// Hot reload polls the filesystem, so it is throttled rather than run every
			// frame - a stat() per script per frame is real cost for no benefit.
			float HotReloadTimer = 0.0f;

			// Live instances, keyed by entity UUID.
			std::unordered_map<UUID, ScriptInstance> Instances;

			// Reverse map, and the reason instances are not simply keyed by entt::entity.
			//
			// Teardown is driven by FiniView, which fires for entity *destruction* as well as
			// component removal. By then the entity is gone from the registry, so its IDComponent -
			// and therefore its UUID - is unreachable. This map is captured at Instantiate time and
			// is the only way to find the instance again afterwards.
			std::unordered_map<entt::entity, UUID> EntityToUUID;
		};

		ScriptEngineData* s_Data = nullptr;

		// Resolves a script asset to an absolute path on disk. Scripts are loaded by path rather
		// than through AssetManager::GetAsset<> on purpose: there is no runtime object to cache
		// (the "asset" is a chunk executed into the shared VM), and hot reload wants the path.
		bool ResolveScriptPath(AssetHandle handle, std::filesystem::path& out)
		{
			const AssetMetadata* metadata = AssetManager::GetMetadata(handle);
			if (!metadata || metadata->Type != AssetType::Script)
				return false;

			out = GetAssetRoot() / metadata->FilePath;
			return true;
		}

		// Executes a script chunk once and caches the resulting table.
		//
		// TypeScriptToLua emits `exports.default = ...`, so the real class table is one level in.
		// Unwrapping here means a hand-written Lua script and a compiled TS one look identical to
		// everything downstream.
		// Runs the chunk and returns the (unwrapped) table, or nullopt on any failure.
		// Split out of LoadScriptClass so hot reload can re-run a chunk without
		// disturbing the cached entry until it knows the new one is good.
		std::optional<sol::table> RunScriptChunk(const std::filesystem::path& path)
		{
			if (!std::filesystem::exists(path))
			{
				GE_CORE_ERROR("ScriptEngine: script file not found: {0}", path.string());
				return std::nullopt;
			}

			sol::protected_function_result result =
				s_Data->Lua.safe_script_file(path.string(), sol::script_pass_on_error);

			if (!result.valid())
			{
				sol::error error = result;
				GE_ERROR("Script load failed [{0}]: {1}", path.string(), error.what());
				return std::nullopt;
			}

			sol::object returned = result;
			if (!returned.is<sol::table>())
			{
				GE_ERROR("Script [{0}] must return a table of lifecycle methods", path.string());
				return std::nullopt;
			}

			sol::table scriptClass = returned.as<sol::table>();

			sol::object defaultExport = scriptClass["default"];
			if (defaultExport.is<sol::table>())
				scriptClass = defaultExport.as<sol::table>();

			return scriptClass;
		}

		// The table method lookup should delegate to. TSTL `class` output puts methods
		// on .prototype; object literals carry them directly.
		sol::table MethodTableOf(const sol::table& scriptClass)
		{
			sol::object prototype = scriptClass["prototype"];
			return prototype.is<sol::table>() ? prototype.as<sol::table>() : scriptClass;
		}

		std::filesystem::file_time_type StampOf(const std::filesystem::path& path)
		{
			std::error_code ec;
			auto stamp = std::filesystem::last_write_time(path, ec);
			return ec ? std::filesystem::file_time_type{} : stamp;
		}

		ScriptEngineData::ScriptClass* LoadScriptClass(AssetHandle handle)
		{
			auto cached = s_Data->ScriptClasses.find(handle);
			if (cached != s_Data->ScriptClasses.end())
				return &cached->second;

			std::filesystem::path path;
			if (!ResolveScriptPath(handle, path))
			{
				GE_CORE_ERROR("ScriptEngine: handle {0} is not a registered .lua asset",
					static_cast<uint64_t>(handle));
				return nullptr;
			}

			std::optional<sol::table> scriptClass = RunScriptChunk(path);
			if (!scriptClass)
				return nullptr;

			ScriptEngineData::ScriptClass entry;
			entry.Table = *scriptClass;
			entry.Path = path;
			entry.Stamp = StampOf(path);

			return &s_Data->ScriptClasses.emplace(handle, std::move(entry)).first->second;
		}

		// Every call into Lua goes through here. A script error must never cross the C++ boundary
		// as an exception or take the editor down with it - it logs, disables the instance, and
		// the frame continues.
		template<typename... Args>
		void CallMethod(ScriptInstance& instance, const char* method, Args&&... args)
		{
			if (instance.Dead)
				return;

			sol::object member = instance.Self[method];
			if (!member.is<sol::protected_function>())
				return;   // lifecycle methods are all optional

			sol::protected_function function = member.as<sol::protected_function>();
			sol::protected_function_result result = function(instance.Self, std::forward<Args>(args)...);

			if (result.valid())
				return;

			sol::error error = result;
			GE_ERROR("Script error in {0}:{1} - {2} (this script is now disabled)",
				instance.Path, method, error.what());
			instance.Dead = true;
		}

		ScriptInstance* FindInstance(entt::entity entity)
		{
			auto uuid = s_Data->EntityToUUID.find(entity);
			if (uuid == s_Data->EntityToUUID.end())
				return nullptr;

			auto instance = s_Data->Instances.find(uuid->second);
			return instance != s_Data->Instances.end() ? &instance->second : nullptr;
		}
	}

	void ScriptEngine::Init()
	{
		GE_CORE_ASSERT(!s_Data, "ScriptEngine::Init called twice");
		s_Data = new ScriptEngineData();

		// No io/os/package: gameplay scripts have no business touching the filesystem or spawning
		// processes, and leaving them out keeps a stray require() from silently finding something.
		s_Data->Lua.open_libraries(
			sol::lib::base,
			sol::lib::math,
			sol::lib::string,
			sol::lib::table,
			sol::lib::coroutine);

		RegisterScriptBindings(s_Data->Lua);

		GE_CORE_INFO("ScriptEngine initialised ({0})", LUA_RELEASE);
	}

	void ScriptEngine::Shutdown()
	{
		if (!s_Data)
			return;

		DestroyAllInstances();
		delete s_Data;
		s_Data = nullptr;
	}

	bool ScriptEngine::IsInitialized()
	{
		return s_Data != nullptr;
	}

	void ScriptEngine::SetSceneContext(Scene* scene)
	{
		if (s_Data)
			s_Data->SceneContext = scene;
	}

	Scene* ScriptEngine::GetSceneContext()
	{
		return s_Data ? s_Data->SceneContext : nullptr;
	}

	void* ScriptEngine::GetLuaState()
	{
		return s_Data ? static_cast<void*>(s_Data->Lua.lua_state()) : nullptr;
	}

	void ScriptEngine::ReinstallGlobals()
	{
		if (s_Data)
			RegisterScriptGlobals(s_Data->Lua);
	}

	void ScriptEngine::Instantiate(Entity entity, AssetHandle script)
	{
		if (!s_Data || !IsAssetHandleValid(script))
			return;

		const UUID uuid = entity.GetUUID();

		// Idempotent by contract: OnRuntimeStart sweeps every live script, then the InitView's
		// first-ever read reports those same entities again.
		if (s_Data->Instances.find(uuid) != s_Data->Instances.end())
			return;

		ScriptEngineData::ScriptClass* scriptClass = LoadScriptClass(script);
		if (!scriptClass)
			return;

		// Method lookup delegates to the class table, so all instances share one copy of the
		// methods and `self` stays per-entity. The prototype hop covers TSTL `class` output, where
		// methods live on Class.prototype rather than on the table itself - object literals are
		// still the documented pattern, since a TSTL class constructor never runs through here.
		sol::table methods = MethodTableOf(scriptClass->Table);

		sol::table self = s_Data->Lua.create_table();
		self[sol::metatable_key] = s_Data->Lua.create_table_with(sol::meta_function::index, methods);
		self["entity"] = entity;

		ScriptInstance instance;
		instance.Self = self;
		instance.Script = script;

		std::filesystem::path path;
		instance.Path = ResolveScriptPath(script, path) ? path.filename().string() : "<unknown>";

		auto& stored = s_Data->Instances.emplace(uuid, std::move(instance)).first->second;
		s_Data->EntityToUUID[static_cast<entt::entity>(entity)] = uuid;

		CallMethod(stored, "OnCreate");
	}

	void ScriptEngine::DestroyInstance(entt::entity entity)
	{
		if (!s_Data)
			return;

		auto uuid = s_Data->EntityToUUID.find(entity);
		if (uuid == s_Data->EntityToUUID.end())
			return;

		auto instance = s_Data->Instances.find(uuid->second);
		if (instance != s_Data->Instances.end())
		{
			CallMethod(instance->second, "OnDestroy");
			s_Data->Instances.erase(instance);
		}

		s_Data->EntityToUUID.erase(uuid);
	}

	void ScriptEngine::DestroyAllInstances()
	{
		if (!s_Data)
			return;

		for (auto& [uuid, instance] : s_Data->Instances)
			CallMethod(instance, "OnDestroy");

		s_Data->Instances.clear();
		s_Data->EntityToUUID.clear();

		// Dropping the cached chunks is what makes every Play press pick up edits from disk.
		s_Data->ScriptClasses.clear();
	}

	void ScriptEngine::PollHotReload(Timestep ts)
	{
		if (!s_Data || s_Data->ScriptClasses.empty())
			return;

		// Once a second, not every frame: this stats every loaded script.
		s_Data->HotReloadTimer += ts.GetSeconds();
		if (s_Data->HotReloadTimer < 1.0f)
			return;
		s_Data->HotReloadTimer = 0.0f;

		for (auto& [handle, scriptClass] : s_Data->ScriptClasses)
		{
			const std::filesystem::file_time_type stamp = StampOf(scriptClass.Path);
			if (stamp == scriptClass.Stamp || stamp == std::filesystem::file_time_type{})
				continue;

			// Take the new stamp regardless of whether the chunk is valid, or a file
			// with a syntax error re-reports every second until it is fixed.
			scriptClass.Stamp = stamp;

			std::optional<sol::table> reloaded = RunScriptChunk(scriptClass.Path);
			if (!reloaded)
			{
				// The old table stays live, so a bad save leaves the running game on
				// the last good version instead of killing every instance.
				GE_WARN("Hot reload failed for {0}; keeping the previous version",
					scriptClass.Path.filename().string());
				continue;
			}

			scriptClass.Table = *reloaded;
			const sol::table methods = MethodTableOf(scriptClass.Table);

			// Re-point each live instance's metatable at the new methods. Only the
			// lookup target changes: fields already on `self` survive, so an entity
			// keeps its position, timers and whatever else state it accumulated.
			int repointed = 0;
			for (auto& [uuid, instance] : s_Data->Instances)
			{
				if (instance.Script != handle)
					continue;

				instance.Self[sol::metatable_key] =
					s_Data->Lua.create_table_with(sol::meta_function::index, methods);

				// A reload is the author's fix for whatever killed it, so give a
				// disabled instance another chance.
				instance.Dead = false;
				repointed++;
			}

			GE_INFO("Hot reloaded {0} ({1} live instance{2})",
				scriptClass.Path.filename().string(), repointed, repointed == 1 ? "" : "s");
		}
	}

	void ScriptEngine::Update(entt::entity entity, Timestep ts)
	{
		if (!s_Data)
			return;

		if (ScriptInstance* instance = FindInstance(entity))
			CallMethod(*instance, "OnUpdate", ts.GetSeconds());
	}

	void ScriptEngine::OnCollisionEnter(Entity entity, Entity other)
	{
		if (!s_Data)
			return;

		if (ScriptInstance* instance = FindInstance(static_cast<entt::entity>(entity)))
			CallMethod(*instance, "OnCollisionEnter", other);
	}

	void ScriptEngine::OnCollisionExit(Entity entity, Entity other)
	{
		if (!s_Data)
			return;

		if (ScriptInstance* instance = FindInstance(static_cast<entt::entity>(entity)))
			CallMethod(*instance, "OnCollisionExit", other);
	}
}
