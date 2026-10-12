#pragma once

#if defined(__linux__)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

#define APIENTRY
#define WINAPI

using HMODULE = void *;
using HINSTANCE = void *;
using HANDLE = void *;
using LPVOID = void *;
using LPCWSTR = const wchar_t *;
using LPWSTR = wchar_t *;
using LPCTSTR = const wchar_t *;
using TCHAR = wchar_t;
using DWORD = std::uint32_t;
using BOOL = int;

#define TRUE 1
#define FALSE 0
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define VK_F10 0x79
#define VK_F11 0x7A
#define MAX_PATH 4096
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define TEXT(value) L##value

template <std::size_t N, typename... Args>
inline int swprintf_s(wchar_t (&buffer)[N], const wchar_t *format, Args... args)
{
	return std::swprintf(buffer, N, format, args...);
}

template <std::size_t N, typename... Args>
inline int sprintf_s(char (&buffer)[N], const char *format, Args... args)
{
	return std::snprintf(buffer, N, format, args...);
}

inline DWORD GetModuleFileNameW(HMODULE module, wchar_t *buffer, DWORD size)
{
	if (buffer == nullptr || size == 0)
		return 0;

	std::string path;
	if (module == nullptr)
	{
		std::vector<char> path_buffer(256);
		for (;;)
		{
			const ssize_t length = readlink("/proc/self/exe", path_buffer.data(), path_buffer.size() - 1);
			if (length < 0)
				return 0;
			if (static_cast<std::size_t>(length) < path_buffer.size() - 1)
			{
				path.assign(path_buffer.data(), static_cast<std::size_t>(length));
				break;
			}
			path_buffer.resize(path_buffer.size() * 2);
		}
	}
	else
	{
		struct link_map *map = nullptr;
		if (dlinfo(module, RTLD_DI_LINKMAP, &map) != 0 || map == nullptr || map->l_name == nullptr)
			return 0;
		path = map->l_name;
	}

	const std::size_t converted = std::mbstowcs(buffer, path.c_str(), size - 1);
	if (converted == static_cast<std::size_t>(-1))
		return 0;
	buffer[converted] = L'\0';
	return static_cast<DWORD>(converted);
}

inline DWORD GetModuleFileName(HMODULE module, wchar_t *buffer, DWORD size)
{
	return GetModuleFileNameW(module, buffer, size);
}

inline void Sleep(DWORD milliseconds)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

#endif
