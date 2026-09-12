#pragma once

#include <algorithm>
#include <filesystem>
#include <vector>

namespace reshade::utils
{
	inline std::vector<std::filesystem::path> find_addon_files(const std::filesystem::path &user_directory, const std::filesystem::path &installed_directory)
	{
		std::vector<std::filesystem::path> files;
		for (const auto &directory : { user_directory, installed_directory })
		{
			if (directory.empty())
				continue;
			std::error_code ec;
			for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec), end;
				!ec && it != end; it.increment(ec))
			{
				const auto &path = it->path();
				if (path.extension() != ".addon" && path.extension() != ".addon64")
					continue;
				// A user copy takes precedence over an installed copy of the same add-on.
				if (std::none_of(files.begin(), files.end(), [&path](const auto &file) { return file.filename() == path.filename(); }))
					files.push_back(path);
			}
		}
		return files;
	}
}
