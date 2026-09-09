#include "process_environment.hpp"
#include "paths.hpp"
#include "dll_log.hpp"
#include "ini_file.hpp"
#include "version.h"
#include <array>
#include <cstdlib>
#include <dlfcn.h>
#include <iomanip>
#include <mutex>
#include <sstream>
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

	bool initialize_default_config(const std::filesystem::path &config_path, const std::filesystem::path &config_root)
	{
		const std::filesystem::path data_root = reshade::utils::xdg_path("XDG_DATA_HOME", ".local/share");
		const std::filesystem::path cache_root = reshade::utils::xdg_path("XDG_CACHE_HOME", ".cache");
		if (data_root.empty() || cache_root.empty())
			return false;

		const std::filesystem::path shader_root = data_root / "reshade" / "reshade-shaders";
		reshade::ini_file config(config_path);
		config.set("GENERAL", "EffectSearchPaths", shader_root / "Shaders" / "**");
		config.set("GENERAL", "TextureSearchPaths", shader_root / "Textures" / "**");
		config.set("GENERAL", "IntermediateCachePath", cache_root / "reshade");
		config.set("GENERAL", "PresetPath", config_root / "reshade" / "ReShadePreset.ini");
		return config.save();
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
		// AppImage mounts are ephemeral, so use the original image path as the stable identity.
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

	std::string make_application_id(const std::filesystem::path &path)
	{
		// FNV-1a is deterministic across processes and standard-library versions.
		uint64_t hash = 14695981039346656037ull;
		for (const unsigned char value : path.u8string())
		{
			hash ^= value;
			hash *= 1099511628211ull;
		}

		std::string name = path.filename().u8string();
		for (char &value : name)
			if (!(value >= '0' && value <= '9') && !(value >= 'A' && value <= 'Z') && !(value >= 'a' && value <= 'z') && value != '-' && value != '_')
				value = '_';
		if (name.empty())
			name = "application";

		std::ostringstream result;
		result << name.substr(0, 96) << '-' << std::hex << std::setw(16) << std::setfill('0') << hash;
		return result.str();
	}
}

bool reshade::process::initialize()
{
	std::call_once(s_initialize_once, []() {
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

		s_application_id = make_application_id(g_target_executable_path);
		const std::filesystem::path adjacent_config = g_target_executable_path.parent_path() / "ReShade.ini";
		if (std::filesystem::is_regular_file(adjacent_config, ec))
		{
			s_config_path = adjacent_config;
		}
		else
		{
			ec.clear();
			const std::filesystem::path config_root = reshade::utils::xdg_path("XDG_CONFIG_HOME", ".config");
			if (config_root.empty())
				return;
			s_config_path = config_root / "reshade" / "apps" / s_application_id / "ReShade.ini";
			if (!std::filesystem::is_regular_file(s_config_path, ec))
			{
				ec.clear();
				std::filesystem::create_directories(s_config_path.parent_path(), ec);
				if (ec || !initialize_default_config(s_config_path, config_root))
				{
					s_config_path.clear();
					return;
				}
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

		const std::filesystem::path data_root = reshade::utils::xdg_path("XDG_DATA_HOME", ".local/share");
		if (data_root.empty())
			return;

		s_log_path = data_root / "reshade" / "logs" / s_application_id / "ReShade.log";
		if (reshade::log::open_log_file(s_log_path, ec))
		{
			reshade::log::message(reshade::log::level::info,
				"Initializing ReShade version '" VERSION_STRING_FILE "' loaded from '%s' into '%s'.",
				g_reshade_dll_path.u8string().c_str(), g_target_executable_path.u8string().c_str());
			reshade::log::message(reshade::log::level::info, "Using configuration file '%s'.", s_config_path.u8string().c_str());
		}
	});

	return s_process_active;
}

std::filesystem::path reshade::process::get_log_path()
{
	return s_log_path;
}
