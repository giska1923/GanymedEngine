#pragma once

#include <filesystem>

namespace GanymedE {

	inline const std::filesystem::path& GetAssetRoot()
	{
		static const std::filesystem::path s_AssetRoot = "assets";
		return s_AssetRoot;
	}

	inline std::filesystem::path MakeAssetRelative(const std::filesystem::path& path)
	{
		std::error_code ec;
		auto relative = std::filesystem::relative(path, GetAssetRoot(), ec);
		if (!ec)
			return relative;
		return path;
	}

}
