#pragma once

#include <functional>
#include <string>

namespace GanymedE::EditorUI {

	struct TitleBarState
	{
		std::string DocumentName;
		bool Dirty = false;
	};

	void InitTitleBar();
	void ShutdownTitleBar();

	// 40 px ChromeBg strip: app icon, Menu popup (File/Edit/View), one document tab,
	// min/max/close. Reports caption vs. interactive rects for WM_NCHITTEST / manual drag.
	void DrawTitleBar(const TitleBarState& state, const std::function<void()>& drawMenus);

}
