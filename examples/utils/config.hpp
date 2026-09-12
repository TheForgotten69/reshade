/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#if defined(__linux__)
#include "linux/paths.hpp"
#include <cstdio>
#endif

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

#if defined(__linux__)
namespace reshade_addon_utils
{
	inline std::filesystem::path get_storage_directory(const char *subdirectory, bool create)
	{
		const std::filesystem::path root = reshade::utils::xdg_path("XDG_DATA_HOME", ".local/share");
		if (root.empty())
			return {};

		const std::filesystem::path directory = root / "reshade" / subdirectory;
		std::error_code ec;
		if (create)
			std::filesystem::create_directories(directory, ec);
		return ec ? std::filesystem::path() : directory;
	}

	inline std::filesystem::path make_dump_path(const char *subdirectory, uint32_t hash, const wchar_t *extension) noexcept
	{
		try
		{
			std::filesystem::path path = get_storage_directory(subdirectory, true);
			if (path.empty())
				return {};
			char name[11];
			std::snprintf(name, sizeof(name), "0x%08X", hash);
			path /= name;
			path += extension;
			return path;
		}
		catch (...)
		{
			return {};
		}
	}
}
#endif
