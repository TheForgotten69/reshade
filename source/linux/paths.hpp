#pragma once

#include <cstdlib>
#include <filesystem>

namespace reshade::utils
{
	// XDG directory overrides and HOME must be absolute paths.
	inline std::filesystem::path xdg_path(const char *variable, const char *fallback)
	{
		if (const char *value = std::getenv(variable); value != nullptr && value[0] != '\0')
			if (std::filesystem::path path = std::filesystem::u8path(value); path.is_absolute())
				return path;

		if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
			if (std::filesystem::path path = std::filesystem::u8path(home); path.is_absolute())
				return path / fallback;

		return {};
	}
}
