#pragma once

#include <string>

namespace GanymedE {

	class Scene;
	class PhysicsScene;

	namespace Net {

		// Client only, and before the scene update: applying a spawn is CreateEntity, which
		// asserts while a system is running. Interpolation is written here too, so the physics
		// step that follows sees the pose.
		void PresentWorld(Scene& scene, float frameDt);

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
