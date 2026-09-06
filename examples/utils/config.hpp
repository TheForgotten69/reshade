/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include <cstdlib>
#include <filesystem>

// The subdirectory to save shader binaries to
#if defined(__linux__)
#define RESHADE_ADDON_SHADER_SAVE_DIR "./shaderdump"
#else
#define RESHADE_ADDON_SHADER_SAVE_DIR ".\\shaderdump"
#endif

// The subdirectory to load shader binaries from
#if defined(__linux__)
#define RESHADE_ADDON_SHADER_LOAD_DIR "./shaderreplace"
#else
#define RESHADE_ADDON_SHADER_LOAD_DIR ".\\shaderreplace"
#endif

// The subdirectory to save textures to
#if defined(__linux__)
#define RESHADE_ADDON_TEXTURE_SAVE_DIR "./texdump"
#else
#define RESHADE_ADDON_TEXTURE_SAVE_DIR ".\\texdump"
#endif
#define RESHADE_ADDON_TEXTURE_SAVE_FORMAT ".png"
#define RESHADE_ADDON_TEXTURE_SAVE_HASH_TEXMOD 1
// Skip any textures that were already dumped this session, to reduce lag at the cost of increased memory usage
#define RESHADE_ADDON_TEXTURE_SAVE_ENABLE_HASH_SET 1

// The subdirectory to load textures from
#if defined(__linux__)
#define RESHADE_ADDON_TEXTURE_LOAD_DIR "./texreplace"
#else
#define RESHADE_ADDON_TEXTURE_LOAD_DIR ".\\texreplace"
#endif
#define RESHADE_ADDON_TEXTURE_LOAD_FORMAT ".png"
#define RESHADE_ADDON_TEXTURE_LOAD_HASH_TEXMOD 1

namespace reshade_addon_utils
{
#if defined(__linux__)
	// Resolve add-on example data to a user-writable XDG data directory. The
	// executable directory is commonly read-only for packaged applications.
	inline std::filesystem::path get_storage_directory(const char *subdirectory, bool create) noexcept
	{
		try
		{
			std::filesystem::path candidates[2];
			size_t candidate_count = 0;

			if (const char *const data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr && data_home[0] != '\0')
			{
				const std::filesystem::path path = data_home;
				if (path.is_absolute())
					candidates[candidate_count++] = path / "reshade";
			}

			if (const char *const home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
			{
				const std::filesystem::path path = home;
				if (path.is_absolute() && (candidate_count == 0 || candidates[0] != path / ".local/share" / "reshade"))
					candidates[candidate_count++] = path / ".local/share/reshade";
			}

			std::filesystem::path first_missing;
			for (size_t i = 0; i < candidate_count; ++i)
			{
				const std::filesystem::path directory = candidates[i] / subdirectory;
				std::error_code ec;

				if (create)
				{
					std::filesystem::create_directories(directory, ec);
					if (!ec && std::filesystem::is_directory(directory, ec) && !ec)
						return directory;
				}
				else if (std::filesystem::is_directory(directory, ec) && !ec)
				{
					return directory;
				}

				if (!ec && first_missing.empty())
				{
					std::error_code exists_ec;
					if (!std::filesystem::exists(directory, exists_ec) && !exists_ec)
						first_missing = directory;
				}
			}

			// For reads, return the preferred not-yet-created path so a later
			// replacement file is still found there. For writes, only return a
			// directory after creation succeeded.
			return create ? std::filesystem::path() : first_missing;
		}
		catch (...)
		{
			return {};
		}
	}
#else
	inline std::filesystem::path get_storage_directory(const char *, bool) noexcept
	{
		return {};
	}
#endif
}
