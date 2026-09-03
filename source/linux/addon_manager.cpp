#include "addon_manager.hpp"

#if RESHADE_ADDON && defined(__linux__)

#include "dll_log.hpp"
#include "ini_file.hpp"
#include "reshade.hpp"
#include "runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <dlfcn.h>

extern std::filesystem::path g_reshade_base_path;

extern void register_addon_depth();
extern void unregister_addon_depth();

bool reshade::addon_enabled = true;
bool reshade::addon_all_loaded = true;
std::vector<void *> reshade::addon_event_list[static_cast<uint32_t>(reshade::addon_event::max)];
std::vector<reshade::addon_info> reshade::addon_loaded_info;
thread_local const reshade::addon_info *reshade::addon_current = nullptr;

namespace
{
	std::atomic_ulong s_reference_count = 0;

	void *get_reshade_module_handle()
	{
		Dl_info info = {};
		return dladdr(reinterpret_cast<const void *>(&get_reshade_module_handle), &info) != 0 ? info.dli_fbase : nullptr;
	}
}

void reshade::load_addons()
{
	if (s_reference_count.fetch_add(1, std::memory_order_acq_rel) != 0)
		return;

	addon_all_loaded = true;

	addon_info &info = addon_loaded_info.emplace_back();
	info.name = "Generic Depth";
	info.description = "Automatic depth buffer detection that works in the majority of games.";
	info.author = "crosire";
	info.api_version = RESHADE_API_VERSION;
	info.external = false;

	std::vector<std::string> disabled_addons;
	global_config().get("ADDON", "DisabledAddons", disabled_addons);
	if (std::find(disabled_addons.cbegin(), disabled_addons.cend(), info.name) == disabled_addons.cend())
	{
		info.handle = get_reshade_module_handle();
		register_addon_depth();
		log::message(log::level::info, "Loaded built-in add-on \"Generic Depth\".");
	}
}

void reshade::unload_addons()
{
	const unsigned long previous = s_reference_count.fetch_sub(1, std::memory_order_acq_rel);
	assert(previous != 0);
	if (previous != 1)
		return;

	unregister_addon_depth();
	addon_loaded_info.clear();
}

bool reshade::has_loaded_addons()
{
	return false; // Linux currently supports the built-in Generic Depth add-on only.
}

reshade::addon_info *reshade::find_addon(const void *address)
{
	if (address == nullptr)
		return nullptr;

	Dl_info module_info = {};
	if (dladdr(address, &module_info) == 0 || module_info.dli_fbase == nullptr)
		return nullptr;

	for (auto it = addon_loaded_info.rbegin(); it != addon_loaded_info.rend(); ++it)
		if (it->handle == module_info.dli_fbase)
			return &*it;
	return nullptr;
}

bool ReShadeRegisterAddon(void *, uint32_t)
{
	reshade::log::message(reshade::log::level::warning, "External add-ons are not supported on Linux.");
	return false;
}

void ReShadeUnregisterAddon(void *)
{
}

void ReShadeRegisterEvent(reshade::addon_event ev, void *callback)
{
	if (ev >= reshade::addon_event::max || callback == nullptr)
		return;

	reshade::addon_info *const info = reshade::find_addon(callback);
	if (info == nullptr)
	{
		reshade::log::message(reshade::log::level::error, "Could not find associated add-on and therefore failed to register an event.");
		return;
	}

	reshade::addon_event_list[static_cast<uint32_t>(ev)].push_back(callback);
	info->event_callbacks.emplace_back(static_cast<uint32_t>(ev), callback);
}

void ReShadeRegisterEventForAddon(void *, reshade::addon_event ev, void *callback)
{
	ReShadeRegisterEvent(ev, callback);
}

void ReShadeUnregisterEvent(reshade::addon_event ev, void *callback)
{
	if (ev >= reshade::addon_event::max || callback == nullptr)
		return;

	std::vector<void *> &event_list = reshade::addon_event_list[static_cast<uint32_t>(ev)];
	event_list.erase(std::remove(event_list.begin(), event_list.end(), callback), event_list.end());

	if (reshade::addon_info *const info = reshade::find_addon(callback))
		info->event_callbacks.erase(std::remove(info->event_callbacks.begin(), info->event_callbacks.end(), std::make_pair(static_cast<uint32_t>(ev), callback)), info->event_callbacks.end());
}

void ReShadeUnregisterEventForAddon(void *, reshade::addon_event ev, void *callback)
{
	ReShadeUnregisterEvent(ev, callback);
}

#if RESHADE_GUI
void ReShadeRegisterOverlay(const char *title, void(*callback)(reshade::api::effect_runtime *))
{
	reshade::addon_info *const info = reshade::find_addon(reinterpret_cast<void *>(callback));
	if (info == nullptr)
		return;

	if (title == nullptr)
		info->settings_overlay_callback = callback;
	else
		info->overlay_callbacks.push_back({ title, callback });
}

void ReShadeRegisterOverlayForAddon(void *, const char *title, void(*callback)(reshade::api::effect_runtime *))
{
	ReShadeRegisterOverlay(title, callback);
}

void ReShadeUnregisterOverlay(const char *title, void(*callback)(reshade::api::effect_runtime *))
{
	reshade::addon_info *const info = reshade::find_addon(reinterpret_cast<void *>(callback));
	if (info == nullptr)
		return;

	if (title == nullptr)
		info->settings_overlay_callback = nullptr;
	else
		info->overlay_callbacks.erase(std::remove_if(info->overlay_callbacks.begin(), info->overlay_callbacks.end(),
			[title, callback](const reshade::addon_info::overlay_callback &item) { return item.title == title && item.callback == callback; }), info->overlay_callbacks.end());
}

void ReShadeUnregisterOverlayForAddon(void *, const char *title, void(*callback)(reshade::api::effect_runtime *))
{
	ReShadeUnregisterOverlay(title, callback);
}
#endif

void ReShadeLogMessage(void *module, int level, const char *message)
{
	if (const reshade::addon_info *const info = reshade::find_addon(module))
		reshade::log::message(static_cast<reshade::log::level>(level), "[%.*s] %s", static_cast<int>(info->name.size()), info->name.c_str(), message);
	else
		reshade::log::message(static_cast<reshade::log::level>(level), "%s", message);
}

void ReShadeGetBasePath(char *path, size_t *size)
{
	if (size == nullptr)
		return;
	const std::string path_string = g_reshade_base_path.u8string();
	if (path == nullptr)
		*size = path_string.size() + 1;
	else if (*size != 0)
	{
		*size = path_string.copy(path, *size - 1);
		path[*size] = '\0';
	}
}

bool ReShadeGetConfigValue(void *, reshade::api::effect_runtime *runtime, const char *section, const char *key, char *value, size_t *size)
{
	reshade::ini_file &config = runtime != nullptr ? reshade::ini_file::load_cache(static_cast<reshade::runtime *>(runtime)->get_config_path()) : reshade::global_config();
	std::vector<std::string> elements;
	config.get(section != nullptr ? section : "", key != nullptr ? key : "", elements);

	if (size != nullptr)
	{
		std::string value_string;
		for (const std::string &element : elements)
		{
			value_string += element;
			value_string += '\0';
		}
		if (elements.empty())
			*size = 0;
		else if (value == nullptr)
			*size = value_string.size() + 1;
		else if (*size != 0)
		{
			*size = value_string.copy(value, *size - 1);
			value[*size] = '\0';
		}
	}
	return !elements.empty();
}

void ReShadeSetConfigValue(void *module, reshade::api::effect_runtime *runtime, const char *section, const char *key, const char *value)
{
	ReShadeSetConfigArray(module, runtime, section, key, value, value != nullptr ? std::strlen(value) : 0);
}

void ReShadeSetConfigArray(void *, reshade::api::effect_runtime *runtime, const char *section, const char *key, const char *value, size_t size)
{
	reshade::ini_file &config = runtime != nullptr ? reshade::ini_file::load_cache(static_cast<reshade::runtime *>(runtime)->get_config_path()) : reshade::global_config();
	if (value == nullptr)
	{
		config.remove_key(section != nullptr ? section : "", key != nullptr ? key : "");
		return;
	}

	std::vector<std::string> elements;
	for (size_t i = 0, index = 0; i < size; ++i)
	{
		if (index >= elements.size())
			elements.resize(index + 1);
		if (value[i] == '\0')
			++index;
		else
			elements[index] += value[i];
	}
	config.set(section != nullptr ? section : "", key != nullptr ? key : "", elements);
	if (runtime != nullptr)
		static_cast<reshade::runtime *>(runtime)->load_config();
}

#endif
