#include "dll_resources.hpp"
#include "resources_linux.hpp"
#include "runtime.hpp"
#include "version.h"

#if RESHADE_LOCALIZATION
#include "localization_linux.hpp"
#include <algorithm>
#include <cstdlib>
#endif

extern "C"
{
	const char *ReShadeVersion = VERSION_STRING_PRODUCT;
}

thread_local bool g_in_dxgi_runtime = false;

unsigned int reshade::runtime::s_latest_version[3] = {};

void reshade::runtime::check_for_update()
{
}

bool reshade::runtime::init_gui_vr()
{
	return false;
}

void reshade::runtime::deinit_gui_vr()
{
}

void reshade::runtime::draw_gui_vr()
{
}

reshade::resources::data_resource reshade::resources::load_data_resource(unsigned short id)
{
	switch (id)
	{
	case IDR_IMGUI_VS_SPIRV:
		return { sizeof(reshade_imgui_vs_spirv), reshade_imgui_vs_spirv };
	case IDR_IMGUI_PS_SPIRV:
		return { sizeof(reshade_imgui_ps_spirv), reshade_imgui_ps_spirv };
	default:
		return { 0, nullptr };
	}
}

#if RESHADE_LOCALIZATION

namespace
{
	// Empty means "use the system default", matching how set_current_language("") is used on
	// Windows to mean "MUI thread default"; resolved lazily so a later locale environment
	// variable change (there is no equivalent of a live system language change notification
	// on Linux) does not require re-selecting it explicitly.
	thread_local std::string s_current_language;

	// There is no per-thread MUI preferred-language concept on Linux; approximate the same
	// "what language is the user running their desktop in" question from the same environment
	// variables every other localized application on the system already reads, in POSIX locale
	// precedence order. Normalizes e.g. "de_DE.UTF-8" to "de-DE" to match the res/lang_*.rc2
	// naming convention (see source/linux/generate_localization.py).
	std::string detect_system_language()
	{
		for (const char *const name : { "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG" })
		{
			const char *const value = std::getenv(name);
			if (value == nullptr || *value == '\0')
				continue;

			std::string language = value;
			if (const size_t colon = language.find(':'); colon != std::string::npos)
				language.resize(colon); // "LANGUAGE" may list multiple ':'-separated fallbacks; only the first is used
			if (const size_t suffix = language.find_first_of(".@"); suffix != std::string::npos)
				language.resize(suffix); // Strip encoding/modifier, e.g. ".UTF-8" or "@euro"
			std::replace(language.begin(), language.end(), '_', '-');
			return language;
		}

		return "en-US";
	}
}

std::string reshade::resources::load_string(unsigned short id)
{
	const std::string language = get_current_language();
	if (const char *const text = find_localized_string_linux(language, id))
		return text;
	// Fall back to English when the current language exists but is missing this particular
	// string (translations lag behind as new strings are added), mirroring the secondary
	// MUI language Windows' set_current_language() implicitly appends.
	if (language != "en-US")
		if (const char *const text = find_localized_string_linux("en-US", id))
			return text;
	return std::string();
}

std::string reshade::resources::get_current_language()
{
	return s_current_language.empty() ? detect_system_language() : s_current_language;
}
std::string reshade::resources::set_current_language(const std::string &language)
{
	const std::string prev = get_current_language();
	if (language != prev)
		s_current_language = language;
	return prev;
}

std::vector<std::string> reshade::resources::get_languages()
{
	return get_languages_linux();
}

#endif
