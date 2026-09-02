/*
 * Copyright (C) 2022 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdio>
#include <ctime>
#include <filesystem>

namespace reshade::utils
{
	enum class file_share_mode
	{
		read_write,
		read_only,
	};

	FILE *open_file(const std::filesystem::path &path, const char *mode, file_share_mode sharing = file_share_mode::read_write);
	void local_time(const std::time_t &time, std::tm &result);

	/// <summary>
	/// Opens a file explorer window with the specified file selected.
	/// </summary>
	bool open_explorer(const std::filesystem::path &path);

	/// <summary>
	/// Executes the specified command as a new process, with basic (not elevated) user privileges.
	/// </summary>
	bool execute_command(const std::string &command_line, const std::filesystem::path &working_directory = std::filesystem::path(), bool hide_window = false);

	/// <summary>
	/// Plays the specified audio file asynchronously.
	/// </summary>
	void play_sound_async(const std::filesystem::path &audio_file);

#if defined(__linux__)
	std::filesystem::path find_system_font(const char *family);
#endif
}
