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
	// Launches 'argv' (execvp-style, nullptr-terminated) as a detached child process without
	// blocking the calling thread. 'hide_window' redirects the child's stdio to /dev/null, the
	// closest Linux equivalent of Windows' hidden-window process creation; 'working_directory', if
	// non-empty, becomes the child's current directory and never affects this process. The child is
	// reaped by a short-lived detached thread rather than left a zombie; that thread only captures
	// the child pid by value, so it cannot outlive or dangle off any ReShade runtime state.
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

		// 'posix_spawnp' (not 'posix_spawn') so a bare command name like "xdg-open" resolves
		// against $PATH the same way the shell would, instead of requiring a full path.
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
			// The reaper thread failed to start; the child becomes a zombie until this process
			// exits. That is a resource leak, not a crash, and no worse than the outcome of a
			// failed process launch on any platform, so it is not treated as failure here.
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

// Opens the freedesktop-default file manager on the directory containing 'path' via 'xdg-open'.
// Unlike Windows' Explorer integration, this cannot select 'path' itself within that folder view -
// doing so portably across file managers requires the org.freedesktop.FileManager1 D-Bus interface,
// which is a materially larger dependency (a D-Bus client) not justified purely for this.
bool reshade::utils::open_explorer(const std::filesystem::path &path)
{
	std::error_code ec;
	const std::filesystem::path directory = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
	if (spawn_detached({ "xdg-open", directory.c_str(), nullptr }, {}, true))
		return true;
	log::message(log::level::warning, "Failed to launch 'xdg-open' for '%s'.", directory.c_str());
	return false;
}

// Interprets 'command_line' as a shell command, matching how the equivalent Windows
// 'CreateProcess' call treats it: both accept a single string the user configured themselves
// (ReShade's screenshot post-save command setting) rather than externally supplied input, so
// shell interpretation is the intended behavior, not an injection risk.
bool reshade::utils::execute_command(const std::string &command_line, const std::filesystem::path &working_directory, bool hide_window)
{
	if (command_line.empty())
		return false;
	if (spawn_detached({ "/bin/sh", "-c", command_line.c_str(), nullptr }, working_directory, hide_window))
		return true;
	log::message(log::level::warning, "Failed to execute command '%s'.", command_line.c_str());
	return false;
}

// No lightweight, dependency-free way to play audio exists on Linux the way 'PlaySound' does on
// Windows; pulling in a full audio library only for the screenshot sound is not justified. Left
// unimplemented rather than silently pretending to succeed - callers only ever fire-and-forget
// this, so there is nothing else to report a failure to.
void reshade::utils::play_sound_async(const std::filesystem::path &)
{
}
