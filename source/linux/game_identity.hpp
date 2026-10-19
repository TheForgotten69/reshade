#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace reshade::process
{
	struct game_identity_inputs
	{
		std::filesystem::path executable_path;
		std::string vulkan_application_name;
		std::vector<std::string> command_line;
		std::string steam_app_id;
		std::filesystem::path steam_install_path;
		std::filesystem::path wine_prefix;
		std::string profile_name;
	};

	struct game_identity
	{
		std::string directory_name;
		std::string description;
		std::string log_discriminator;
		bool wine_host = false;

		std::filesystem::path configuration_path(const std::filesystem::path &data_root) const;
		std::filesystem::path log_path(const std::filesystem::path &data_root) const;
	};

	game_identity resolve_game_identity(const game_identity_inputs &inputs);
}
