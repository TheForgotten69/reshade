#pragma once

#include <cctype>
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

	// Windows file systems are case-insensitive, so rename differently cased variants (e.g. "reshade.ini") to the expected name.
	// Returns whether the file exists afterwards.
	inline bool normalize_file_name_case(const std::filesystem::path &path)
	{
		const auto lower = [](std::string value) {
			for (char &c : value)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return value;
		};

		std::error_code ec;
		if (std::filesystem::is_regular_file(path, ec))
			return true;

		const std::string expected = lower(path.filename().u8string());
		for (std::filesystem::directory_iterator it(path.parent_path(), ec), end; !ec && it != end; it.increment(ec))
		{
			if (lower(it->path().filename().u8string()) == expected && it->is_regular_file(ec))
			{
				std::filesystem::rename(it->path(), path, ec);
				break;
			}
		}

		return std::filesystem::is_regular_file(path, ec);
	}

	// Moves the staged file into place unless the destination already exists, so concurrent writers never replace each other's file.
	// Returns whether the destination exists afterwards.
	inline bool publish_file(const std::filesystem::path &staged, const std::filesystem::path &destination, std::error_code &ec)
	{
		std::filesystem::create_hard_link(staged, destination, ec);
		if (ec == std::errc::file_exists)
			ec.clear();
		else if (ec) // File system may not support hard links
			std::filesystem::rename(staged, destination, ec);

		std::error_code remove_ec;
		std::filesystem::remove(staged, remove_ec);
		return !ec;
	}
}
