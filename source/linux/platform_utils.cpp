#include "platform_utils.hpp"
#include "dll_log.hpp"
#include <cerrno>
#include <fontconfig/fontconfig.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <vector>

extern char **environ;

namespace
{
	// Starts 'argv' (nullptr-terminated, resolved through $PATH) without waiting for it. 'hide_window'
	// redirects its stdio to /dev/null. A detached thread reaps the child so it does not stay a zombie.
	bool spawn_detached(const std::vector<const char *> &argv, const std::filesystem::path &working_directory, bool hide_window)
	{
		posix_spawn_file_actions_t file_actions;
		if (posix_spawn_file_actions_init(&file_actions) != 0)
			return false;

		bool ok = true;
		if (!working_directory.empty())
			ok = posix_spawn_file_actions_addchdir_np(&file_actions, working_directory.c_str()) == 0;
		if (ok && hide_window)
		{
			ok =
				posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
				posix_spawn_file_actions_addopen(&file_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
				posix_spawn_file_actions_addopen(&file_actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
		}

		pid_t child = -1;
		if (ok)
			ok = posix_spawnp(&child, argv[0], &file_actions, nullptr, const_cast<char *const *>(argv.data()), environ) == 0;

		posix_spawn_file_actions_destroy(&file_actions);
		if (!ok)
			return false;

		try
		{
			std::thread([child]() {
				int status = 0;
				while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
			}).detach();
		}
		catch (...)
		{
			// The child was started, it just stays a zombie until this process exits.
		}
		return true;
	}
}

FILE *reshade::utils::open_file(const std::filesystem::path &path, const char *mode, file_share_mode)
{
	return std::fopen(path.c_str(), mode);
}

void reshade::utils::local_time(const std::time_t &time, std::tm &result)
{
	localtime_r(&time, &result);
}

std::filesystem::path reshade::utils::find_system_font(const char *family)
{
	std::filesystem::path result;
	FcPattern *const pattern = FcNameParse(reinterpret_cast<const FcChar8 *>(family));
	if (pattern == nullptr)
		return result;

	FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	FcResult match_result = FcResultNoMatch;
	if (FcPattern *const match = FcFontMatch(nullptr, pattern, &match_result))
	{
		FcChar8 *file = nullptr;
		if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr)
			result = std::filesystem::u8path(reinterpret_cast<const char *>(file));
		FcPatternDestroy(match);
	}

	FcPatternDestroy(pattern);
	return result;
}

// Opens the directory containing 'path'. Selecting the file itself would need the FileManager1 D-Bus interface.
bool reshade::utils::open_explorer(const std::filesystem::path &path)
{
	std::error_code ec;
	const std::filesystem::path directory = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
	if (spawn_detached({ "xdg-open", directory.c_str(), nullptr }, {}, true))
		return true;
	log::message(log::level::warning, "Failed to launch 'xdg-open' for '%s'.", directory.c_str());
	return false;
}

// 'command_line' is the user's own post-save command setting, so it runs through the shell like on Windows.
bool reshade::utils::execute_command(const std::string &command_line, const std::filesystem::path &working_directory, bool hide_window)
{
	if (command_line.empty())
		return false;
	if (spawn_detached({ "/bin/sh", "-c", command_line.c_str(), nullptr }, working_directory, hide_window))
		return true;
	log::message(log::level::warning, "Failed to execute command '%s'.", command_line.c_str());
	return false;
}

// Not implemented, Linux has no dependency-free equivalent of 'PlaySound'.
void reshade::utils::play_sound_async(const std::filesystem::path &)
{
}
