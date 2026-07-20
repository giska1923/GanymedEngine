#include "gepch.h"
#include "LuaScriptSystem.h"

#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scripting/ScriptEngine.h"

namespace GanymedE {

	void LuaScriptSystem::BindSceneContext()
	{
		ScriptEngine::SetSceneContext(&m_Scene);
	}

	void LuaScriptSystem::DrainReactiveViews(bool instantiate)
	{
		// Scripts that appeared since the last update. On the very first read this reports every
		// existing script, which is why ScriptEngine::Instantiate is idempotent — OnRuntimeStart
		// has usually created them already.
		for (auto [entity, script] : View<ScriptInitView>())
		{
			if (instantiate)
				ScriptEngine::Instantiate(Entity{ entity, &m_Scene }, script.Script);
		}

		// Scripts that went away since the last update. The slot reads the buried copy, so this
		// works whether the component was removed or the whole entity destroyed — and in the
		// destroyed case the entity id is all ScriptEngine can use, since the UUID is gone with it.
		for (auto [entity, script] : View<ScriptFiniView>())
		{
			(void)script;
			ScriptEngine::DestroyInstance(entity);
		}
	}

	void LuaScriptSystem::OnRuntimeStart()
	{
		BindSceneContext();

		for (auto [entity, script] : View<ScriptView>())
			ScriptEngine::Instantiate(Entity{ entity, &m_Scene }, script.Script);
	}

	void LuaScriptSystem::OnUpdate(Timestep ts)
	{
		BindSceneContext();

		DrainReactiveViews(/*instantiate=*/true);

		// Before the update calls, so an edit saved this second takes effect on this
		// frame rather than the next one.
		ScriptEngine::PollHotReload(ts);

		for (auto [entity, script] : View<ScriptView>())
		{
			(void)script;
			ScriptEngine::Update(entity, ts);
		}
	}

	void LuaScriptSystem::OnUpdateEditor(Timestep ts)
	{
		(void)ts;

		BindSceneContext();

		// Edit mode does not run scripts, but the reactive views still have to be read every update
		// or the next runtime read would assert on the gap. Nothing is instantiated; teardown still
		// runs, so removing a script component in edit mode cleans up any instance left over from a
		// previous play session. Exactly NativeScriptSystem's pattern.
		DrainReactiveViews(/*instantiate=*/false);
	}

	void LuaScriptSystem::OnRuntimeStop()
	{
		BindSceneContext();

		// Play stopping is not a component removal, so FiniView never sees it — the sweep over
		// every live instance stays. This also drops the cached chunks, so the next Play press
		// re-reads every script from disk.
		ScriptEngine::DestroyAllInstances();

		ScriptEngine::SetSceneContext(nullptr);
	}
}
