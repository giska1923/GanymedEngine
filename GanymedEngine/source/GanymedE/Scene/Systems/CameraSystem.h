#pragma once

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// Resolves which camera the frame is rendered from, once, into the RenderContext singleton.
	//
	// This used to be a "first Primary wins" scan sitting inline in the middle of the render path
	// (audit item A8): a scene-wide decision pretending to be per-entity data. Runs before
	// RenderSystem, which now simply reads the answer.
	class CameraSystem : public ECS::System<CameraSystem>
	{
	public:
		using CameraView = ECS::IterView<ECS::EntityId, ECS::RO<TransformComponent>, ECS::RO<CameraComponent>>;
		using Views = TypeList<CameraView>;

		using ECS::System<CameraSystem>::System;

		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;

	private:
		void ResolveMainCamera();
		void ClearMainCamera();
	};
}
