#pragma once

namespace GanymedE {

	// Who a scene is simulating for. Registration is the only thing it changes: a server does not
	// construct the systems that exist to draw, hear or emit, and that decision lives in
	// SystemManager rather than in #ifdefs. Client registers the same set as Standalone until
	// prediction has a reason to differ (NETCODE.md, N4).
	enum class SceneRole
	{
		Standalone,
		Server,
		Client
	};

}
