#pragma once

#include "GanymedE/Core/UUID.h"

#include <cstdint>

namespace GanymedE {

	// Editor-only joint selection. Not a scene concept: Delete / Ctrl+D stay entity verbs.
	// Subordinate to the outliner — changing the primary entity clears the joint; clicking a
	// bone must not change the entity selection.
	struct EditorJointTool
	{
		UUID SkeletonEntity{ 0 };
		int32_t Joint = -1;

		// Primary entity UUID last seen. When it changes, the joint clears.
		UUID AnchorEntity{ 0 };

		bool VisualizerOn = false;
		bool ScrollToJoint = false;

		bool PickForSocket = false;
		UUID SocketEntity{ 0 };

		bool HasJoint() const { return SkeletonEntity != UUID{ 0 } && Joint >= 0; }

		void ClearJoint()
		{
			SkeletonEntity = UUID{ 0 };
			Joint = -1;
			ScrollToJoint = false;
		}

		void Select(UUID skeletonEntity, int32_t joint)
		{
			SkeletonEntity = skeletonEntity;
			Joint = joint;
			ScrollToJoint = true;
		}

		void CancelPick()
		{
			PickForSocket = false;
			SocketEntity = UUID{ 0 };
		}
	};

}
