#include "addon_compat.hpp"
#include "reshade.hpp"

#if defined(__linux__)

BOOL ReShadeLinuxAddonDllMain(HMODULE module, DWORD reason, LPVOID reserved);

extern "C" RESHADE_ADDON_EXPORT bool AddonInit(HMODULE addon_module, HMODULE)
{
	return ReShadeLinuxAddonDllMain(addon_module, DLL_PROCESS_ATTACH, nullptr) != FALSE;
}

extern "C" RESHADE_ADDON_EXPORT void AddonUninit(HMODULE addon_module, HMODULE)
{
	ReShadeLinuxAddonDllMain(addon_module, DLL_PROCESS_DETACH, nullptr);
}

#endif
