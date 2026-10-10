#include "process_environment.hpp"
#include "game_identity.hpp"
#include "paths.hpp"
#include "dll_log.hpp"
#include "ini_file.hpp"
#include "version.h"
#include <array>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unistd.h>

std::filesystem::path g_reshade_dll_path;
std::filesystem::path g_reshade_base_path;
std::filesystem::path g_target_executable_path;

namespace
{
	std::once_flag s_initialize_once;
	bool s_process_active = false;
	std::string s_application_id;
	std::filesystem::path s_config_path;
	std::filesystem::path s_log_path;

	bool write_default_config(const std::filesystem::path &config_path, const std::filesystem::path &data_root)
	{
		const std::filesystem::path cache_root = reshade::utils::xdg_path("XDG_CACHE_HOME", ".cache");
		if (cache_root.empty())
			return false;

		std::error_code ec;
		std::filesystem::create_directories(cache_root / "reshade", ec);

		const std::filesystem::path shader_root = data_root / "reshade" / "reshade-shaders";
		reshade::ini_file config(config_path);
		config.set("GENERAL", "EffectSearchPaths", shader_root / "Shaders" / "**");
		config.set("GENERAL", "TextureSearchPaths", shader_root / "Textures" / "**");
		config.set("GENERAL", "IntermediateCachePath", cache_root / "reshade");
		return config.save();
	}

	bool create_config(const std::filesystem::path &config_path, const std::filesystem::path &data_root)
	{
		std::error_code ec;
		std::filesystem::create_directories(config_path.parent_path(), ec);
		if (ec)
			return false;

		std::filesystem::path staged_path = config_path;
		staged_path += '.' + std::to_string(getpid()) + ".tmp";
		std::filesystem::remove(staged_path, ec);
		return write_default_config(staged_path, data_root) && reshade::utils::publish_file(staged_path, config_path, ec);
	}

	std::filesystem::path canonical_path(const std::filesystem::path &path, std::error_code &ec)
	{
		std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
		if (ec)
			result.clear();
		return result;
	}

	std::filesystem::path executable_path(std::error_code &ec)
	{
		if (const char *const appimage = std::getenv("APPIMAGE"); appimage != nullptr && appimage[0] != '\0')
		{
			std::filesystem::path result = canonical_path(std::filesystem::u8path(appimage), ec);
			if (!result.empty())
				return result;
		}

		ec.clear();
		std::array<char, 4096> path = {};
		const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
		if (length <= 0 || static_cast<size_t>(length) >= path.size() - 1)
		{
			ec.assign(errno, std::generic_category());
			return {};
		}

		return canonical_path(std::filesystem::u8path(path.data(), path.data() + length), ec);
	}

	std::vector<std::string> command_line()
	{
		std::vector<std::string> result;
		std::ifstream stream("/proc/self/cmdline", std::ios::binary);
		for (std::string argument; std::getline(stream, argument, '\0');)
			result.push_back(std::move(argument));
		return result;
	}

	std::string environment(const char *name)
	{
		const char *const value = std::getenv(name);
		return value != nullptr ? value : "";
	}
}

bool reshade::process::initialize(const char *vulkan_application_name)
{
	std::call_once(s_initialize_once, [vulkan_application_name]() {
		std::error_code ec;

		Dl_info module_info = {};
		if (dladdr(reinterpret_cast<const void *>(&initialize), &module_info) == 0 || module_info.dli_fname == nullptr)
			return;
		g_reshade_dll_path = canonical_path(std::filesystem::u8path(module_info.dli_fname), ec);
		if (g_reshade_dll_path.empty())
			return;

		g_target_executable_path = executable_path(ec);
		if (g_target_executable_path.empty())
			return;

		const std::filesystem::path data_root = reshade::utils::xdg_path("XDG_DATA_HOME", ".local/share");
		if (data_root.empty())
			return;

		const game_identity identity = resolve_game_identity({
			g_target_executable_path,
			vulkan_application_name != nullptr ? vulkan_application_name : "",
			command_line(),
			environment("SteamAppId"),
			std::filesystem::u8path(environment("STEAM_COMPAT_INSTALL_PATH")),
			std::filesystem::u8path(environment("WINEPREFIX")),
			environment("RESHADE_PROFILE") });
		s_application_id = identity.directory_name;

		const std::filesystem::path adjacent_config = g_target_executable_path.parent_path() / "ReShade.ini";
		if (!identity.wine_host && reshade::utils::normalize_file_name_case(adjacent_config))
		{
			s_config_path = adjacent_config;
		}
		else
		{
			s_config_path = identity.configuration_path(data_root);
			if (!reshade::utils::normalize_file_name_case(s_config_path) &&
				!create_config(s_config_path, data_root))
			{
				s_config_path.clear();
				return;
			}
		}

		g_reshade_base_path = s_config_path.parent_path();
		s_process_active = true;

		const char *const disable_logging = std::getenv("RESHADE_DISABLE_LOGGING");
		if (disable_logging != nullptr && disable_logging[0] != '\0')
			return;

		const reshade::ini_file config(s_config_path);
		if (config.has("INSTALL", "Logging") && !config.get("INSTALL", "Logging"))
			return;

		s_log_path = identity.log_path(data_root);
		if (reshade::log::open_log_file(s_log_path, ec))
		{
			reshade::log::message(reshade::log::level::info,
				"Initializing ReShade version '" VERSION_STRING_FILE "' loaded from '%s' into '%s'.",
				g_reshade_dll_path.u8string().c_str(), g_target_executable_path.u8string().c_str());
			reshade::log::message(reshade::log::level::info, "Resolved %s identity '%s'.", identity.description.c_str(), identity.directory_name.c_str());
			reshade::log::message(reshade::log::level::info, "Using configuration file '%s'.", s_config_path.u8string().c_str());
		}
	});

	return s_process_active;
}

std::filesystem::path reshade::process::get_log_path()
{
	return s_log_path;
}
