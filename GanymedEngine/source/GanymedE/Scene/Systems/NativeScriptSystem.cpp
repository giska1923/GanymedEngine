#include "gepch.h"
#include "NativeScriptSystem.h"

#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

namespace GanymedE {

	void NativeScriptSystem::InstantiateScript(entt::entity entity, NativeScriptComponent& script)
	{
		if (script.Instance || !script.InstantiateScript)
			return;

		script.Instance = script.InstantiateScript();
		script.Instance->m_Entity = Entity{ entity, &m_Scene };
		script.Instance->OnCreate();
	}

	void NativeScriptSystem::DestroyScript(NativeScriptComponent& script)
	{
		if (!script.Instance)
			return;

		script.Instance->OnDestroy();
		script.DestroyScript(&script);
	}

	void NativeScriptSystem::DrainReactiveViews(bool instantiate)
	{
		// Scripts that appeared since the last update. On the very first read this reports every
		// existing script, which is why InstantiateScript is idempotent — OnRuntimeStart has
		// usually created them already.
		for (auto [entity, script] : View<ScriptInitView>())
		{
			if (instantiate)
				InstantiateScript(entity, script);
		}

		// Scripts that went away since the last update. The slot reads the buried copy, so this
		// works whether the component was removed or the whole entity destroyed.
		for (auto [entity, script] : View<ScriptFiniView>())
		{
			(void)entity;
			DestroyScript(script);
		}
	}

	void NativeScriptSystem::OnRuntimeStart()
	{
		for (auto [entity, script] : View<ScriptView>())
			InstantiateScript(entity, script);
	}

	void NativeScriptSystem::OnUpdate(Timestep ts)
	{
		DrainReactiveViews(/*instantiate=*/true);

		for (auto [entity, script] : View<ScriptView>())
		{
			(void)entity;
			if (script.Instance)
				script.Instance->OnUpdate(ts);
		}
	}

	void NativeScriptSystem::OnUpdateEditor(Timestep ts)
	{
		(void)ts;

		// Edit mode does not run scripts, but the reactive views still have to be read every
		// update or the next runtime read would assert on the gap. Nothing is instantiated;
		// teardown still runs, so removing a script component in edit mode cleans up any instance
		// left over from a previous play session.
		DrainReactiveViews(/*instantiate=*/false);
	}

	void NativeScriptSystem::OnRuntimeStop()
	{
		// Play stopping is not a component removal, so FiniView never sees it — the sweep over
		// every live script stays.
		for (auto [entity, script] : View<ScriptView>())
		{
			(void)entity;
			DestroyScript(script);
		}
	}
}
