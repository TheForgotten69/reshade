#include "platform_utils.hpp"
#include <fontconfig/fontconfig.h>

FILE *reshade::utils::open_file(const std::filesystem::path &path, const char *mode, file_share_mode)
{
	return std::fopen(path.c_str(), mode);
}

void reshade::utils::local_time(const std::time_t &time, std::tm &result)
{
	localtime_r(&time, &result);
}

std::filesystem::path reshade::utils::find_system_font(const char *family)
{
	std::filesystem::path result;
	FcPattern *const pattern = FcNameParse(reinterpret_cast<const FcChar8 *>(family));
	if (pattern == nullptr)
		return result;

	FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	FcResult match_result = FcResultNoMatch;
	if (FcPattern *const match = FcFontMatch(nullptr, pattern, &match_result))
	{
		FcChar8 *file = nullptr;
		if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr)
			result = std::filesystem::u8path(reinterpret_cast<const char *>(file));
		FcPatternDestroy(match);
	}

	FcPatternDestroy(pattern);
	return result;
}

bool reshade::utils::open_explorer(const std::filesystem::path &)
{
	return false;
}

bool reshade::utils::execute_command(const std::string &, const std::filesystem::path &, bool)
{
	return false;
}

void reshade::utils::play_sound_async(const std::filesystem::path &)
{
}
