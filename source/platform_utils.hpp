/*
 * Copyright (C) 2022 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdio>
#include <ctime>
#include <string>
#include <filesystem>

namespace reshade::utils
{
	enum class file_share_mode
	{
		deny_none,
		deny_write
	};

	/// <summary>
	/// Opens the specified file, optionally preventing other processes from writing to it while it is open.
	/// </summary>
	FILE *open_file(const std::filesystem::path &path, const char *mode, file_share_mode share_mode = file_share_mode::deny_none);

	/// <summary>
	/// Converts the specified time to calendar time in the local time zone.
	/// </summary>
	std::tm local_time(std::time_t time);

	/// <summary>
	/// Gets the value of the specified environment variable, or an empty string if it does not exist.
	/// </summary>
	std::string get_environment_variable(const std::string &name);

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
}
