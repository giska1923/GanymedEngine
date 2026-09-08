#include "gepch.h"
#include "GanymedE/Assets/AssetManagerRegistry.h"

namespace GanymedE {

	namespace Detail {

		AssetTypeId NextAssetTypeId()
		{
			static AssetTypeId s_Next = 0;
			return s_Next++;
		}

		// A function-local static rather than a file-scope one: the slots hold managers whose
		// destructors touch bgfx resources through the assets they retain, and `AssetManager::
		// Shutdown` clears them explicitly while `Renderer::IsGpuAlive()` is still true. Nothing
		// here may depend on static destruction order.
		std::array<Scope<IAssetManager>, MaxAssetManagers>& AssetManagerSlots()
		{
			static std::array<Scope<IAssetManager>, MaxAssetManagers> s_Slots;
			return s_Slots;
		}

	}

	IAssetManager* AssetManagerRegistry::Find(AssetType type)
	{
		// Linear over at most 16 slots, and only from `Reload` - an editor action. The typed
		// path (`Get<T>`) is the one on the render loop, and it is an array index.
		for (const Scope<IAssetManager>& manager : Detail::AssetManagerSlots())
		{
			if (manager && manager->Type() == type)
				return manager.get();
		}

		return nullptr;
	}

	void AssetManagerRegistry::Clear()
	{
		// The managers themselves are destroyed, not just emptied. Re-`Init` re-registers, and
		// `AssetTypeIdOf<T>` keeps handing out the same id, so the slots line up again.
		for (Scope<IAssetManager>& manager : Detail::AssetManagerSlots())
			manager.reset();
	}

}
