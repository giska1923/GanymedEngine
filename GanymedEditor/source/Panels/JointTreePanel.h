#pragma once

#include "GanymedE/Scene/Entity.h"

#include <vector>

namespace GanymedE {

	class Scene;
	struct EditorJointTool;

	// Hierarchy from Skeleton::ParentIndices. Viewport picking and this tree write the same
	// EditorJointTool; neither is allowed to change entity selection.
	class JointTreePanel
	{
	public:
		void OnImGuiRender(Scene* scene, EditorJointTool& tool,
			const std::vector<Entity>& selection);
	private:
		void DrawJoint(int32_t index);
		bool MarkFilterVisible(int32_t index);

		Scene* m_Scene = nullptr;
		EditorJointTool* m_Tool = nullptr;
		Entity m_Skeleton;
		std::vector<std::vector<int32_t>> m_Children;
		std::vector<int32_t> m_Roots;
		std::vector<uint8_t> m_FilterVisible;
		char m_Search[128] = {};
	};

}
