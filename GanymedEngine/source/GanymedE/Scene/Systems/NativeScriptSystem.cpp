#include "gepch.h"
#include "NativeScriptSystem.h"

#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

namespace GanymedE {

	void NativeScriptSystem::InstantiateScripts()
	{
		for (auto [entity, script] : View<ScriptView>())
		{
			if (!script.Instance && script.InstantiateScript)
			{
				script.Instance = script.InstantiateScript();
				script.Instance->m_Entity = Entity{ entity, &m_Scene };
				script.Instance->OnCreate();
			}
		}
	}

	void NativeScriptSystem::OnRuntimeStart()
	{
		InstantiateScripts();
	}

	void NativeScriptSystem::OnUpdate(Timestep ts)
	{
		for (auto [entity, script] : View<ScriptView>())
		{
			// Entities that gained a script after play started are instantiated here, matching
			// the original inline behaviour.
			if (!script.Instance && script.InstantiateScript)
			{
				script.Instance = script.InstantiateScript();
				script.Instance->m_Entity = Entity{ entity, &m_Scene };
				script.Instance->OnCreate();
			}

			if (script.Instance)
				script.Instance->OnUpdate(ts);
		}
	}

	void NativeScriptSystem::OnRuntimeStop()
	{
		for (auto [entity, script] : View<ScriptView>())
		{
			(void)entity;
			if (script.Instance)
			{
				script.Instance->OnDestroy();
				script.DestroyScript(&script);
			}
		}
	}
}
