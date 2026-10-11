#pragma once

#include "GanymedE/Core/UUID.h"

#include <string>

namespace GanymedE {

	class Scene;
	class PhysicsScene;

	namespace Net {

		// Client only, and before the scene update: applying a spawn is CreateEntity, which
		// asserts while a system is running. Interpolation is written here too, so the physics
		// step that follows sees the pose. The locally controlled entity is left for prediction.
		void PresentWorld(Scene& scene, float frameDt);

		// Client only, after bodies exist and before the accumulator. Reconciles the owned
		// character, then simulates it ahead by half the round trip.
		void PredictLocal(Scene& scene, PhysicsScene* physics);

		// Server and standalone, once per physics step, before that step. OnTick.
		void BeginStep(Scene& scene, PhysicsScene* physics);

		// Client only, after the final SyncTransforms. Adds the decaying visual error.
		void ApplyVisualError(Scene& scene, float frameDt);

		// The entity this client predicts. False on a server, and for every other entity.
		bool Controls(UUID id);

		// Server only. Once per physics step, after that step.
		void NoteSimulatedStep(Scene& scene);

		// Server only. The scripted mover, before the step, so a kinematic body copies this
		// pose rather than the one from the previous step.
		void DriveWorldTest(Scene& scene);

		// Server only. After the steps: the delta against what that client has acknowledged.
		void CaptureWorld(Scene& scene, PhysicsScene* physics);

		// Structural half of --net-test (the 500, and their destroy). Not safe during a system.
		void AdvanceWorldTest(Scene& scene);
		void StartWorldTest(Scene& scene, const std::string& test);

	}

}
