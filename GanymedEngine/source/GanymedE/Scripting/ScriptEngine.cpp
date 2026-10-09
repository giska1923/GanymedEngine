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

		// A backend request a script instance is waiting on. Script-side ids, not Online's: a
		// request Online could not even queue still completes (with a failure), and the script must
		// hear about it the same way.
		using ScriptRequestId = uint64_t;

		struct ScriptRequest
		{
			UUID Owner;
			RequestId Transport = 0;          // Online's id, for Cancel; 0 if it was never queued
			sol::protected_function Callback;
			ScriptResponseReader Read;
		};

		struct ArrivedResponse
		{
			ScriptRequestId Id = 0;
			OnlineResponse Response;
		};

		struct Subscription
		{
			UUID Owner;
			std::string Type;
			sol::protected_function Callback;
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

				// Extracted from the class's `Properties` table at load. Cached because the
				// inspector asks for it every frame it draws.
				std::vector<ScriptEngine::ScriptField> Fields;
			};
			std::unordered_map<AssetHandle, ScriptClass> ScriptClasses;

			// Hot reload polls the filesystem, so it is throttled rather than run every
			// frame - a stat() per script per frame is real cost for no benefit.
			float HotReloadTimer = 0.0f;

			// Live instances, per scene, keyed by entity UUID.
			//
			// The outer Scene* key is load-bearing, not defensive. The editor holds several Scenes
			// at once - the edit scene, the play-mode copy, and any temporary one the serializer
			// deserializes into - and Scene::~Scene calls OnRuntimeStop. Flat maps meant that
			// destroying a throwaway Scene tore down the *playing* scene's instances, and that
			// entt::entity ids (which are per-registry and freely collide between scenes) resolved
			// against whichever scene happened to have registered that id.
			struct SceneInstances
			{
				std::unordered_map<UUID, ScriptInstance> ByUUID;

				// Reverse map, and the reason instances are not simply keyed by entt::entity.
				//
				// Teardown is driven by FiniView, which fires for entity *destruction* as well as
				// component removal. By then the entity is gone from the registry, so its
				// IDComponent - and therefore its UUID - is unreachable. This is captured at
				// Instantiate time and is the only way to find the instance again afterwards.
				std::unordered_map<entt::entity, UUID> EntityToUUID;

				// Backend requests this scene's instances are waiting on, and the responses that
				// have reached the main thread but not yet the scene's script update. The mailbox
				// is the second of the three hops in docs/engine/online.md: Online delivers from
				// JobSystem::OnUpdate, where no scene context is set, so the callback cannot run
				// there; it runs when this scene's LuaScriptSystem next updates.
				std::unordered_map<ScriptRequestId, ScriptRequest> Requests;
				std::vector<ArrivedResponse> Mailbox;

				// Push subscriptions, and pushes queued for this scene's next update. The same mailbox
				// rule as responses: pushes arrive from JobSystem::OnUpdate, with no scene context.
				std::vector<Subscription> Subscriptions;
				std::vector<OnlinePush> Pushes;
			};
			std::unordered_map<Scene*, SceneInstances> Instances;

			ScriptRequestId NextRequestId = 1;
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

		// Maps one Lua value to the closed set of tunable types, or nullopt for anything else
		// (tables, functions, nil). Lua 5.4 distinguishes integers from floats, and that
		// distinction is kept: it decides whether the inspector draws an int or a float widget.
		std::optional<ScriptFieldValue> ToFieldValue(const sol::object& value)
		{
			switch (value.get_type())
			{
				case sol::type::boolean:
					return ScriptFieldValue{ value.as<bool>() };

				case sol::type::number:
					// Lua 5.4's integer/float subtypes are deliberately collapsed here; see the
					// note on ScriptFieldValue in Components.h.
					return ScriptFieldValue{ value.as<double>() };

				case sol::type::string:
					return ScriptFieldValue{ value.as<std::string>() };

				case sol::type::userdata:
					if (value.is<glm::vec3>())
						return ScriptFieldValue{ value.as<glm::vec3>() };
					return std::nullopt;

				default:
					return std::nullopt;
			}
		}

		// Reads the class's `Properties` table into a sorted field list.
		//
		// Sorted because Lua table iteration order is unspecified - without this the inspector
		// would reshuffle its rows between runs, and between two machines.
		std::vector<ScriptEngine::ScriptField> ExtractFields(const sol::table& scriptClass,
			const std::string& scriptName)
		{
			std::vector<ScriptEngine::ScriptField> fields;

			sol::object properties = scriptClass["Properties"];
			if (!properties.is<sol::table>())
				return fields;

			for (const auto& [key, value] : properties.as<sol::table>())
			{
				if (key.get_type() != sol::type::string)
					continue;

				const std::string name = key.as<std::string>();
				std::optional<ScriptFieldValue> converted = ToFieldValue(value);
				if (!converted)
				{
					GE_CORE_WARN("Script [{0}]: property '{1}' has an unsupported type and is "
						"ignored (use a boolean, number, string or Vec3)", scriptName, name);
					continue;
				}

				fields.push_back({ name, *converted });
			}

			std::sort(fields.begin(), fields.end(),
				[](const ScriptEngine::ScriptField& a, const ScriptEngine::ScriptField& b)
				{
					return a.Name < b.Name;
				});

			return fields;
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
			entry.Fields = ExtractFields(entry.Table, path.filename().string());

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

		// Instances for the scene the engine is currently pointed at, or null. Every per-entity
		// call resolves through this, so an entity id is never looked up against another scene's
		// registry.
		ScriptEngineData::SceneInstances* CurrentScene()
		{
			if (!s_Data || !s_Data->SceneContext)
				return nullptr;

			auto it = s_Data->Instances.find(s_Data->SceneContext);
			return it != s_Data->Instances.end() ? &it->second : nullptr;
		}

		ScriptInstance* FindInstance(entt::entity entity)
		{
			ScriptEngineData::SceneInstances* scene = CurrentScene();
			if (!scene)
				return nullptr;

			auto uuid = scene->EntityToUUID.find(entity);
			if (uuid == scene->EntityToUUID.end())
				return nullptr;

			auto instance = scene->ByUUID.find(uuid->second);
			return instance != scene->ByUUID.end() ? &instance->second : nullptr;
		}

		// Cancels the requests `owner` is waiting on, or every request in the scene when `owner` is
		// null. Cancel, not just forget: the network thread stops waiting on it and moves on to the
		// next request, rather than sitting out a slow response nobody will read (Online::Cancel
		// says what it does not do).
		void CancelRequests(ScriptEngineData::SceneInstances& scene, const UUID* owner)
		{
			for (auto it = scene.Requests.begin(); it != scene.Requests.end();)
			{
				if (owner && it->second.Owner != *owner)
				{
					++it;
					continue;
				}
				Online::Cancel(it->second.Transport);
				it = scene.Requests.erase(it);
			}
		}

		// Main thread, from Online's completion (JobSystem::OnUpdate). Only queues: there is no
		// scene context here, so nothing can run yet.
		void OnScriptResponse(Scene* scene, ScriptRequestId id, const OnlineResponse& response)
		{
			if (!s_Data)
				return;

			auto instances = s_Data->Instances.find(scene);
			if (instances == s_Data->Instances.end())
				return;   // the scene's instances are gone; its requests were cancelled with them

			if (instances->second.Requests.count(id))
				instances->second.Mailbox.push_back({ id, response });
		}

		// The second argument a failed request's callback receives. A problem-details body's
		// `type` URN when there is one (what O3's bindings promise scripts), else the status, else
		// why no response arrived.
		std::string DescribeFailure(const OnlineResponse& response)
		{
			if (!response.Arrived())
				return response.TransportError;

			if (response.ContentType.find("problem+json") != std::string::npos)
			{
				if (std::optional<JsonValue> problem = ParseJson(response.Body))
				{
					const JsonValue* type = problem->Find("type");
					if (type && type->Kind == JsonValue::Type::String)
						return type->String;
				}
			}
			return "HTTP " + std::to_string(response.Status);
		}
	}

	namespace {

		// The owner is explicit (self.entity) and checked, rather than inferred from whichever
		// instance happens to be running: a request or a subscription with no owner would have
		// nobody to end it.
		UUID ResolveOwner(const char* binding, const sol::object& owner, ScriptEngineData::SceneInstances*& scene)
		{
			scene = CurrentScene();
			const Entity* entity = owner.is<Entity>() ? &owner.as<Entity&>() : nullptr;
			if (!scene || !entity || !*entity || !FindInstance(static_cast<entt::entity>(*entity)))
				throw sol::error(std::string(binding) + ": the first argument must be the calling "
					"script's own entity (self.entity), which owns this and ends it when it goes");
			return scene->EntityToUUID.at(static_cast<entt::entity>(*entity));
		}
	}

	void SendScriptRequest(const char* binding, const sol::object& owner, OnlineRequest request,
		const sol::object& callback, ScriptResponseReader read)
	{
		ScriptEngineData::SceneInstances* scene = nullptr;
		const UUID ownerId = ResolveOwner(binding, owner, scene);

		if (!callback.is<sol::protected_function>())
			throw sol::error(std::string(binding) + ": the last argument must be a function(ok, result)");

		const ScriptRequestId id = s_Data->NextRequestId++;
		Scene* sceneKey = s_Data->SceneContext;

		ScriptRequest pending;
		pending.Owner = ownerId;
		pending.Callback = callback.as<sol::protected_function>();
		pending.Read = std::move(read);

		// Recorded before Send, so the completion always finds it - Send never calls back inline,
		// but a request it cannot queue still completes, and must still reach the script.
		ScriptRequest& stored = scene->Requests.emplace(id, std::move(pending)).first->second;
		stored.Transport = Online::Send(std::move(request),
			[sceneKey, id](const OnlineResponse& response) { OnScriptResponse(sceneKey, id, response); });
	}

	void SubscribeScript(const char* binding, const sol::object& owner, const std::string& type,
		const sol::object& callback)
	{
		ScriptEngineData::SceneInstances* scene = nullptr;
		const UUID ownerId = ResolveOwner(binding, owner, scene);
		if (type.empty())
			throw sol::error(std::string(binding) + ": the message type must not be empty");
		if (!callback.is<sol::protected_function>())
			throw sol::error(std::string(binding) + ": the last argument must be a function(message)");

		for (Subscription& existing : scene->Subscriptions)
		{
			if (existing.Owner == ownerId && existing.Type == type)
			{
				existing.Callback = callback.as<sol::protected_function>();
				return;
			}
		}
		scene->Subscriptions.push_back({ ownerId, type, callback.as<sol::protected_function>() });
	}

	void UnsubscribeScript(const char* binding, const sol::object& owner, const std::string& type)
	{
		ScriptEngineData::SceneInstances* scene = nullptr;
		const UUID ownerId = ResolveOwner(binding, owner, scene);
		auto& subs = scene->Subscriptions;
		subs.erase(std::remove_if(subs.begin(), subs.end(),
			[&](const Subscription& s) { return s.Owner == ownerId && s.Type == type; }), subs.end());
	}

	size_t ScriptEngine::QueuePush(const OnlinePush& push)
	{
		if (!s_Data)
			return 0;

		size_t queued = 0;
		for (auto& [scene, instances] : s_Data->Instances)
		{
			const bool listening = std::any_of(instances.Subscriptions.begin(), instances.Subscriptions.end(),
				[&](const Subscription& s) { return s.Type == push.Type; });
			if (listening)
			{
				instances.Pushes.push_back(push);
				queued++;
			}
		}
		return queued;
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

	void ScriptEngine::Instantiate(Entity entity, const ScriptComponent& component)
	{
		const AssetHandle script = component.Script;
		if (!s_Data || !s_Data->SceneContext || !IsAssetHandleValid(script))
			return;

		ScriptEngineData::SceneInstances& sceneInstances = s_Data->Instances[s_Data->SceneContext];
		const UUID uuid = entity.GetUUID();

		// Idempotent by contract: OnRuntimeStart sweeps every live script, then the InitView's
		// first-ever read reports those same entities again.
		if (sceneInstances.ByUUID.find(uuid) != sceneInstances.ByUUID.end())
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

		const std::string instancePath = scriptClass->Path.filename().string();
		const auto& overrides = component.Fields;

		// Properties land on `self` directly, not via the metatable: they are per-entity state,
		// and a script assigning to self.speed must not write through to the shared class table.
		//
		// Defaults first, then this entity's overrides on top. Both happen BEFORE OnCreate, so a
		// script can read its tuned values there - which is the whole point of exposing them.
		for (const ScriptField& field : scriptClass->Fields)
		{
			const ScriptFieldValue* value = &field.Default;

			auto override_ = overrides.find(field.Name);
			if (override_ != overrides.end())
			{
				// Only accept an override whose type still matches the declaration. A script
				// that changes `speed` from a number to a string leaves stale scene data behind,
				// and pushing that into Lua would fail somewhere far less obvious than here.
				if (override_->second.index() == field.Default.index())
					value = &override_->second;
				else
					GE_WARN("Script [{0}]: saved value for '{1}' no longer matches the declared "
						"type; using the script's default", instancePath, field.Name);
			}

			std::visit([&](const auto& v) { self[field.Name] = v; }, *value);
		}

		ScriptInstance instance;
		instance.Self = self;
		instance.Script = script;
		instance.Path = instancePath;

		auto& stored = sceneInstances.ByUUID.emplace(uuid, std::move(instance)).first->second;
		sceneInstances.EntityToUUID[static_cast<entt::entity>(entity)] = uuid;

		CallMethod(stored, "OnCreate");
	}

	void ScriptEngine::DestroyInstance(entt::entity entity)
	{
		ScriptEngineData::SceneInstances* scene = CurrentScene();
		if (!scene)
			return;

		auto uuid = scene->EntityToUUID.find(entity);
		if (uuid == scene->EntityToUUID.end())
			return;

		auto instance = scene->ByUUID.find(uuid->second);
		if (instance != scene->ByUUID.end())
		{
			CallMethod(instance->second, "OnDestroy");
			scene->ByUUID.erase(instance);
		}

		// After OnDestroy, so a request sent from OnDestroy is cancelled too rather than orphaned.
		CancelRequests(*scene, &uuid->second);
		auto& subs = scene->Subscriptions;
		const UUID owner = uuid->second;
		subs.erase(std::remove_if(subs.begin(), subs.end(),
			[&](const Subscription& s) { return s.Owner == owner; }), subs.end());

		scene->EntityToUUID.erase(uuid);
	}

	void ScriptEngine::DestroySceneInstances(Scene* scene)
	{
		if (!s_Data || !scene)
			return;

		auto it = s_Data->Instances.find(scene);
		if (it != s_Data->Instances.end())
		{
			for (auto& [uuid, instance] : it->second.ByUUID)
				CallMethod(instance, "OnDestroy");

			// Stopping play mid-request lands here: the transfers are aborted, and their late
			// responses are counted by Online as dropped, never delivered.
			CancelRequests(it->second, nullptr);
			s_Data->Instances.erase(it);
		}

		// Dropping the cached chunks is what makes every Play press pick up edits from disk.
		// Global on purpose and harmless when another scene is live: the chunks simply reload
		// on next use, and existing instances keep the method table they already resolved.
		s_Data->ScriptClasses.clear();

		// Only relinquish the context if it was this scene's; a temporary Scene being destroyed
		// must not blind the one that is actually playing.
		if (s_Data->SceneContext == scene)
			s_Data->SceneContext = nullptr;
	}

	void ScriptEngine::DestroyAllInstances()
	{
		if (!s_Data)
			return;

		for (auto& [scene, instances] : s_Data->Instances)
		{
			for (auto& [uuid, instance] : instances.ByUUID)
				CallMethod(instance, "OnDestroy");
			CancelRequests(instances, nullptr);
		}

		s_Data->Instances.clear();
		s_Data->ScriptClasses.clear();
	}

	const std::vector<ScriptEngine::ScriptField>& ScriptEngine::GetScriptFields(AssetHandle script)
	{
		static const std::vector<ScriptField> s_Empty;

		if (!s_Data || !IsAssetHandleValid(script))
			return s_Empty;

		ScriptEngineData::ScriptClass* scriptClass = LoadScriptClass(script);
		return scriptClass ? scriptClass->Fields : s_Empty;
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
			scriptClass.Fields = ExtractFields(scriptClass.Table, scriptClass.Path.filename().string());
			const sol::table methods = MethodTableOf(scriptClass.Table);

			// Re-point each live instance's metatable at the new methods. Only the
			// lookup target changes: fields already on `self` survive, so an entity
			// keeps its position, timers and whatever else state it accumulated.
			int repointed = 0;
			for (auto& [scene, instances] : s_Data->Instances)
			{
				for (auto& [uuid, instance] : instances.ByUUID)
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
			}

			GE_INFO("Hot reloaded {0} ({1} live instance{2})",
				scriptClass.Path.filename().string(), repointed, repointed == 1 ? "" : "s");
		}
	}

	void ScriptEngine::DeliverResponses()
	{
		GE_PROFILE_FUNCTION();

		ScriptEngineData::SceneInstances* scene = CurrentScene();
		if (!scene || (scene->Mailbox.empty() && scene->Pushes.empty()))
			return;

		// Swapped out first: a callback may send another request, and while its response cannot
		// arrive before the next frame, nothing here should depend on that.
		std::vector<ArrivedResponse> arrived;
		arrived.swap(scene->Mailbox);
		std::vector<OnlinePush> pushes;
		pushes.swap(scene->Pushes);

		for (ArrivedResponse& item : arrived)
		{
			auto pending = scene->Requests.find(item.Id);
			if (pending == scene->Requests.end())
				continue;   // cancelled after it arrived

			ScriptRequest request = std::move(pending->second);
			scene->Requests.erase(pending);

			auto instance = scene->ByUUID.find(request.Owner);
			if (instance == scene->ByUUID.end() || instance->second.Dead)
				continue;   // a disabled script gets nothing, like every other callback

			bool ok = item.Response.IsSuccess();
			sol::object value = sol::lua_nil;
			std::string failure;
			if (ok)
			{
				try
				{
					value = request.Read ? request.Read(s_Data->Lua, item.Response) : sol::lua_nil;
				}
				catch (const std::exception& e)
				{
					ok = false;
					failure = std::string("malformed response: ") + e.what();
				}
			}
			else
				failure = DescribeFailure(item.Response);

			sol::protected_function_result result = ok
				? request.Callback(true, value)
				: request.Callback(false, failure);

			if (!result.valid())
			{
				sol::error error = result;
				GE_ERROR("Script error in {0}: a backend callback - {1} (this script is now disabled)",
					instance->second.Path, error.what());
				instance->second.Dead = true;
			}
		}

		for (const OnlinePush& push : pushes)
		{
			// The subscribers as they are now, copied: a callback may subscribe or unsubscribe, which
			// must not disturb this loop.
			std::vector<Subscription> targets;
			for (const Subscription& s : scene->Subscriptions)
			{
				if (s.Type == push.Type)
					targets.push_back(s);
			}

			// The wire envelope, as the contract writes it: { type, id, payload }. Payload keys keep
			// their wire names (party_id, display_name). A push is a nudge; the typed bindings that
			// re-fetch the state are where names are mapped.
			sol::table message = s_Data->Lua.create_table();
			message["type"] = push.Type;
			message["id"] = push.Id;
			message["payload"] = JsonToLua(s_Data->Lua, push.Payload);

			for (Subscription& target : targets)
			{
				auto instance = scene->ByUUID.find(target.Owner);
				if (instance == scene->ByUUID.end() || instance->second.Dead)
					continue;

				sol::protected_function_result result = target.Callback(message);
				if (!result.valid())
				{
					sol::error error = result;
					GE_ERROR("Script error in {0}: a '{1}' push handler - {2} (this script is now disabled)",
						instance->second.Path, push.Type, error.what());
					instance->second.Dead = true;
				}
			}
		}
	}

	void ScriptEngine::Update(entt::entity entity, Timestep ts)
	{
		if (!s_Data)
			return;

		if (ScriptInstance* instance = FindInstance(entity))
			CallMethod(*instance, "OnUpdate", ts.GetSeconds());
	}

	// A table rather than two more arguments, for the reason Physics.Raycast returns one: named
	// fields read at the call site, and a handler written for one argument still works, because
	// Lua drops arguments a function does not declare.
	void ScriptEngine::OnCollisionEnter(Entity entity, Entity other, const glm::vec3& point,
		const glm::vec3& normal)
	{
		if (!s_Data)
			return;

		if (ScriptInstance* instance = FindInstance(static_cast<entt::entity>(entity)))
		{
			sol::table contact = s_Data->Lua.create_table();
			contact["point"] = point;
			contact["normal"] = normal;
			CallMethod(*instance, "OnCollisionEnter", other, contact);
		}
	}

	void ScriptEngine::OnCollisionExit(Entity entity, Entity other)
	{
		if (!s_Data)
			return;

		if (ScriptInstance* instance = FindInstance(static_cast<entt::entity>(entity)))
			CallMethod(*instance, "OnCollisionExit", other);
	}
}
