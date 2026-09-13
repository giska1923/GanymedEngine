#include "gepch.h"
#include "GanymedE/Assets/AssetPaths.h"

namespace GanymedE {

	namespace {

		// Not a function-local static: those are the usual answer to static initialisation
		// order, but the ordering hazard here runs the other way. A namespace-scope object in
		// *another* translation unit that copies GetAssetRoot() into itself would capture the
		// default before main() ever runs, and the editor had exactly that bug in
		// ContentBrowserPanel's g_AssetPath. Keeping the storage here and handing back a
		// reference makes such a copy visible as a copy rather than as a path that silently
		// stopped tracking.
		std::filesystem::path s_AssetRoot = "assets";
		bool s_Explicit = false;

	}

	void SetAssetRoot(const std::filesystem::path& root)
	{
		// An empty root would resolve every asset against the working directory, which is not a
		// sensible project and is what an unset config key looks like. Keep the default.
		if (root.empty())
		{
			GE_CORE_WARN("SetAssetRoot ignored an empty root; keeping '{0}'",
				s_AssetRoot.generic_string());
			return;
		}

		// Setting the same root twice is what an app that re-inits looks like, and is harmless.
		// Setting a *different* one after the first is the race described in the header: paths
		// already handed out as project-relative would now resolve somewhere else.
		if (s_Explicit && root != s_AssetRoot)
		{
			GE_CORE_ERROR("SetAssetRoot called twice with different roots ('{0}' then '{1}'). "
				"The root is read from worker threads during Parse and cannot change after "
				"startup; keeping the first.", s_AssetRoot.generic_string(),
				root.generic_string());
			GE_CORE_ASSERT(false, "Asset root changed after it was set");
			return;
		}

		s_AssetRoot = root;
		s_Explicit = true;
	}

	const std::filesystem::path& GetAssetRoot()
	{
		return s_AssetRoot;
	}

}
