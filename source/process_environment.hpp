#pragma once

#include <filesystem>

namespace reshade::process
{
#if defined(__linux__)
	bool initialize();
	std::filesystem::path get_log_path();
#elif defined(_WIN32)
	inline bool initialize() { return true; }
#endif
}
