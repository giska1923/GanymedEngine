#include "gepch.h"
#include "CameraSystem.h"

#include "GanymedE/ECS/Singleton.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/SceneSingletons.h"

namespace GanymedE {

	void CameraSystem::ResolveMainCamera()
	{
		ECS::SingletonAccessView<ECS::RW<RenderContext>> render{ m_Scene };
		RenderContext& context = render.Get().Modify();

		context.MainCamera = nullptr;
		context.CameraTransform = glm::mat4{ 1.0f };

		for (auto [entity, transform, camera] : View<CameraView>())
		{
			(void)transform;
			if (!camera.Primary)
				continue;

			context.MainCamera = &camera.Camera;
			context.CameraTransform = m_Scene.GetWorldSpaceTransform(Entity{ entity, &m_Scene });
			break;   // first primary wins, as before
		}
	}

	void CameraSystem::ClearMainCamera()
	{
		// MainCamera points into a live CameraComponent, so it must never outlive the update that
		// resolved it. Edit mode renders from the editor camera and ignores it entirely.
		ECS::SingletonAccessView<ECS::RW<RenderContext>> render{ m_Scene };
		RenderContext& context = render.Get().Modify();
		context.MainCamera = nullptr;
		context.CameraTransform = glm::mat4{ 1.0f };
	}

	void CameraSystem::OnUpdate(Timestep ts)
	{
		(void)ts;
		ResolveMainCamera();
	}

	void CameraSystem::OnUpdateEditor(Timestep ts)
	{
		(void)ts;
		ClearMainCamera();
	}
}
