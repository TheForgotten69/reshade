#pragma once

namespace reshade::process
{
#if defined(__linux__)
	bool initialize();
#elif defined(_WIN32)
	inline bool initialize() { return true; }
#endif
}
