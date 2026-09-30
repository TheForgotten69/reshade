#include "game_identity.hpp"
#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace
{
	constexpr size_t max_name_length = 96;

	// Wine guest paths use backslashes, which are not separators on Linux
	std::string guest_filename(std::string path)
	{
		const size_t separator = path.find_last_of("/\\");
		if (separator != std::string::npos)
			path.erase(0, separator + 1);
		return path;
	}

	std::string without_extension(std::string name)
	{
		const size_t extension = name.find_last_of('.');
		if (extension != std::string::npos && extension != 0)
			name.resize(extension);
		return name;
	}

	std::string to_lower(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value;
	}

	void truncate_utf8(std::string &value, size_t length)
	{
		if (value.size() <= length)
			return;
		while (length != 0 && (static_cast<unsigned char>(value[length]) & 0xc0) == 0x80)
			--length;
		value.resize(length);
	}

	std::string path_label(std::filesystem::path path)
	{
		path = path.lexically_normal();
		while (!path.empty() && path.filename().empty())
			path = path.parent_path();
		if (path.filename() == "pfx" || path.filename() == "prefix")
			path = path.parent_path();
		return path.filename().u8string();
	}

	// Keep UTF-8 characters, but replace ASCII punctuation
	std::string sanitize(std::string name)
	{
		for (char &value : name)
		{
			const unsigned char byte = static_cast<unsigned char>(value);
			if (byte < 0x80 && !std::isalnum(byte) && value != '-' && value != '_')
				value = '_';
		}
		name.erase(std::unique(name.begin(), name.end(), [](char lhs, char rhs) { return lhs == '_' && rhs == '_'; }), name.end());
		while (!name.empty() && name.front() == '_') name.erase(name.begin());
		while (!name.empty() && name.back() == '_') name.pop_back();
		if (name.empty())
			name = "application";
		truncate_utf8(name, max_name_length);
		return name;
	}

	std::string append_suffix(std::string base, const std::string &suffix)
	{
		const std::string safe_suffix = sanitize(suffix);
		if (safe_suffix.size() >= max_name_length - 1)
			return safe_suffix;
		base = sanitize(std::move(base));
		truncate_utf8(base, max_name_length - safe_suffix.size() - 1);
		return base + '-' + safe_suffix;
	}

	// Steam only passes the install directory to games run through a compatibility tool, so extract it from the executable path otherwise
	std::filesystem::path steam_install_directory(const std::filesystem::path &executable_path)
	{
		const auto end = executable_path.end();
		std::filesystem::path result;
		for (auto it = executable_path.begin(); it != end; ++it)
		{
			result /= *it;
			if (to_lower(it->u8string()) != "steamapps")
				continue;
			const auto common = std::next(it);
			if (common == end || *common != "common")
				continue;
			// The game directory must contain the executable rather than be it
			if (const auto game = std::next(common); game != end && std::next(game) != end)
				return result / *common / *game;
		}
		return {};
	}

	bool is_wine_loader(const std::filesystem::path &path)
	{
		const std::string name = path.filename().u8string();
		return name == "wine" || name == "wine64" || name == "wine-preloader" || name == "wine64-preloader";
	}

	// Only match at the start of a word, so that e.g. "sweden" is not detected as Eden
	bool contains_word_prefix(const std::string &value, std::string_view marker)
	{
		const std::string lower = to_lower(value);
		for (size_t position = lower.find(marker); position != std::string::npos; position = lower.find(marker, position + 1))
			if (position == 0 || !std::isalnum(static_cast<unsigned char>(lower[position - 1])))
				return true;
		return false;
	}

	std::string emulator_family(const std::string &name)
	{
		for (const auto &[marker, display_name] : {
			std::pair<std::string_view, std::string_view>{ "rpcs3", "RPCS3" },
			{ "dolphin", "Dolphin" }, { "pcsx2", "PCSX2" }, { "eden", "Eden" },
			{ "ryujinx", "Ryujinx" }, { "yuzu", "Yuzu" }, { "duckstation", "DuckStation" },
			{ "ppsspp", "PPSSPP" }, { "cemu", "Cemu" }, { "xenia", "Xenia" } })
			if (contains_word_prefix(name, marker))
				return std::string(display_name);
		return {};
	}

	bool is_game_content_path(const std::string &argument)
	{
		if (argument.empty() || argument[0] == '-')
			return false;
		const std::string lower = to_lower(argument);
		for (const std::string_view extension : { ".iso", ".rvz", ".gcz", ".wbfs", ".wad", ".elf", ".bin", ".pkg", ".rap", ".chd", ".cso", ".nsp", ".xci", ".nca", ".wua", ".wux", ".wud", ".rom" })
			if (lower.size() >= extension.size() && lower.compare(lower.size() - extension.size(), extension.size(), extension) == 0)
				return true;
		return false;
	}

	std::string emulator_content_name(const std::vector<std::string> &arguments)
	{
		for (auto it = arguments.rbegin(); it != arguments.rend(); ++it)
		{
			if (!is_game_content_path(*it))
				continue;
			std::string name = without_extension(guest_filename(*it));
			// PS3 games boot "PS3_GAME/USRDIR/EBOOT.BIN", so name them after the folder that contains the game
			if (to_lower(name) == "eboot")
			{
				std::filesystem::path parent = std::filesystem::u8path(*it).parent_path();
				while (!parent.empty() && (to_lower(parent.filename().u8string()) == "usrdir" || to_lower(parent.filename().u8string()) == "ps3_game"))
					parent = parent.parent_path();
				if (!parent.filename().empty())
					name = parent.filename().u8string();
			}
			return sanitize(name);
		}
		return {};
	}
}

std::filesystem::path reshade::process::game_identity::configuration_path(const std::filesystem::path &data_root) const
{
	return data_root / "reshade" / "configurations" / directory_name / "ReShade.ini";
}

std::filesystem::path reshade::process::game_identity::log_path(const std::filesystem::path &data_root) const
{
	const std::string filename = log_discriminator.empty() ? "ReShade.log" : "ReShade-" + log_discriminator + ".log";
	return data_root / "reshade" / "logs" / directory_name / filename;
}

reshade::process::game_identity reshade::process::resolve_game_identity(const game_identity_inputs &inputs)
{
	game_identity result;
	result.wine_host = is_wine_loader(inputs.executable_path);

	const std::string executable_label = sanitize(without_extension(inputs.executable_path.filename().u8string()));
	std::string guest_name;
	if (result.wine_host && !inputs.command_line.empty())
		guest_name = guest_filename(inputs.command_line.front());
	if (!guest_name.empty())
		result.log_discriminator = sanitize(without_extension(guest_name));

	if (!inputs.profile_name.empty())
	{
		result.directory_name = sanitize(inputs.profile_name);
		result.description = "RESHADE_PROFILE";
		return result;
	}

	// The Wine loader lives in the Proton installation, which does not name the game
	const std::filesystem::path steam_install_path = inputs.steam_install_path.empty() && !result.wine_host ?
		steam_install_directory(inputs.executable_path) : inputs.steam_install_path;
	if (!inputs.steam_app_id.empty() && !steam_install_path.empty())
	{
		result.directory_name = append_suffix(path_label(steam_install_path), inputs.steam_app_id);
		if (!result.wine_host)
			result.log_discriminator = executable_label;
		result.description = result.wine_host ? "Wine/Proton game" : "Steam game process";
		return result;
	}

	if (result.wine_host)
	{
		std::string label;
		if (!steam_install_path.empty())
			label = path_label(steam_install_path);
		if (label.empty())
			label = without_extension(guest_name);
		if (label.empty())
			label = executable_label;
		result.directory_name = sanitize(label);
		if (!inputs.steam_app_id.empty())
			result.directory_name = append_suffix(result.directory_name, inputs.steam_app_id);
		else if (!inputs.wine_prefix.empty() && path_label(inputs.wine_prefix) != ".wine")
			result.directory_name = append_suffix(result.directory_name, path_label(inputs.wine_prefix));
		result.description = "Wine/Proton game";
		return result;
	}

	std::string label = inputs.vulkan_application_name.empty() ? executable_label : sanitize(inputs.vulkan_application_name);
	const std::string executable_emulator = emulator_family(executable_label);
	const std::string application_emulator = emulator_family(label);
	if (!executable_emulator.empty() && executable_emulator != application_emulator)
		label = executable_emulator;
	const bool emulator = !application_emulator.empty() || !executable_emulator.empty();
	if (emulator)
		if (const std::string content = emulator_content_name(inputs.command_line); !content.empty())
			label = append_suffix(label, content);
	if (!inputs.steam_app_id.empty())
		label = append_suffix(label, inputs.steam_app_id);
	result.directory_name = sanitize(label);
	result.description = emulator ? "emulator" : "native application";
	return result;
}
