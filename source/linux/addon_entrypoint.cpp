#include "addon_compat.hpp"

#if defined(__linux__)

BOOL ReShadeLinuxAddonDllMain(HMODULE module, DWORD reason, LPVOID reserved);

namespace
{
	void *get_module_handle()
	{
		Dl_info info = {};
		struct link_map *map = nullptr;
		return dladdr1(reinterpret_cast<const void *>(&get_module_handle), &info, reinterpret_cast<void **>(&map), RTLD_DL_LINKMAP) != 0 ? map : nullptr;
	}

	__attribute__((constructor)) void load_addon()
	{
		ReShadeLinuxAddonDllMain(get_module_handle(), DLL_PROCESS_ATTACH, nullptr);
	}

	__attribute__((destructor)) void unload_addon()
	{
		ReShadeLinuxAddonDllMain(get_module_handle(), DLL_PROCESS_DETACH, nullptr);
	}
}

#endif
