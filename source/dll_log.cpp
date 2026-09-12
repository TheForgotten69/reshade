/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "dll_log.hpp"
#include <cstdarg>
#ifdef _WIN32
#include <Windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#endif

#ifdef _WIN32
struct scoped_file_handle
{
	scoped_file_handle(HANDLE handle = INVALID_HANDLE_VALUE) : handle(handle) {}
	~scoped_file_handle()
	{
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
	}

	operator HANDLE() const { return handle; }

	void operator=(HANDLE new_handle)
	{
		handle = new_handle;
	}

private:
	HANDLE handle;
};

static scoped_file_handle s_log_file_handle;
#elif defined(__linux__)
static FILE *s_log_file = nullptr;
static std::mutex s_log_mutex;
#endif

bool reshade::log::open_log_file(const std::filesystem::path &path, std::error_code &ec)
{
#ifdef _WIN32
	// Close the previous file first
	// Do this here, instead of in 'scoped_file_handle::operator=', so that the old handle is closed before the new handle is created
	if (s_log_file_handle != INVALID_HANDLE_VALUE)
		CloseHandle(s_log_file_handle);

	// Open the log file for writing (and flush on each write) and clear previous contents
	s_log_file_handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);

	if (s_log_file_handle != INVALID_HANDLE_VALUE)
	{
		// Last error may be ERROR_ALREADY_EXISTS if an existing file was overwritten, which can be ignored
		ec.clear();
		return true;
	}
	else
	{
		ec.assign(GetLastError(), std::system_category());
		return false;
	}
#elif defined(__linux__)
	std::lock_guard<std::mutex> lock(s_log_mutex);
	if (s_log_file != nullptr)
		std::fclose(s_log_file);

	std::filesystem::create_directories(path.parent_path(), ec);
	if (ec)
		return false;

	s_log_file = std::fopen(path.c_str(), "w");
	if (s_log_file == nullptr)
	{
		ec.assign(errno, std::generic_category());
		return false;
	}

	ec.clear();
	return true;
#endif
}

void reshade::log::message(level level, const char *format, ...)
{
	static constexpr char level_names[][6] = { "ERROR", "WARN ", "INFO ", "DEBUG" };

	if (static_cast<size_t>(level) == 0)
		level = level::error;
	if (static_cast<size_t>(level) > std::size(level_names))
		level = level::debug;

	unsigned int year, month, day, hour, minute, second, millisecond;
	size_t thread_id;
#ifdef _WIN32
	SYSTEMTIME time = {};
	GetLocalTime(&time);
	year = time.wYear;
	month = time.wMonth;
	day = time.wDay;
	hour = time.wHour;
	minute = time.wMinute;
	second = time.wSecond;
	millisecond = time.wMilliseconds;
	thread_id = GetCurrentThreadId();
#elif defined(__linux__)
	const auto now = std::chrono::system_clock::now();
	const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
	std::tm time = {};
	localtime_r(&now_time, &time);
	year = time.tm_year + 1900;
	month = time.tm_mon + 1;
	day = time.tm_mday;
	hour = time.tm_hour;
	minute = time.tm_min;
	second = time.tm_sec;
	millisecond = static_cast<unsigned int>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
	thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif

	std::string line_string(256, '\0');

	// Start a new line
	const auto meta_length = std::snprintf(line_string.data(), line_string.size(),
#if RESHADE_VERBOSE_LOG
		"%04u-%02u-%02uT"
#endif
		"%02u:%02u:%02u:%03u [%5zu] | %.5s | ",
#if RESHADE_VERBOSE_LOG
		year, month, day,
#endif
		hour, minute, second, millisecond, thread_id, level_names[static_cast<size_t>(level) - 1]);

	va_list args;
	va_start(args, format);
	const auto content_length = std::vsnprintf(line_string.data() + meta_length, line_string.size() + 1 - meta_length, format, args);
	va_end(args);

	const bool remaining_content = static_cast<size_t>(meta_length) + static_cast<size_t>(content_length) > line_string.size();
	line_string.resize(static_cast<size_t>(meta_length) + static_cast<size_t>(content_length));

	if (remaining_content)
	{
		va_start(args, format);
		std::vsnprintf(line_string.data() + meta_length, line_string.size() + 1 - meta_length, format, args);
		va_end(args);
	}

	line_string += '\n'; // Terminate line with line feed

	// Replace all LF with CRLF on Windows
#ifdef _WIN32
	for (size_t offset = 0; (offset = line_string.find('\n', offset)) != std::string::npos; offset += 2)
		line_string.replace(offset, 1, "\r\n", 2);
#endif

	// Write line to the log file
#ifdef _WIN32
	if (s_log_file_handle != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(s_log_file_handle, line_string.data(), static_cast<DWORD>(line_string.size()), &written, nullptr);
		assert(written == line_string.size());
	}
#elif defined(__linux__)
	std::lock_guard<std::mutex> lock(s_log_mutex);
	if (s_log_file != nullptr)
	{
		const size_t written = std::fwrite(line_string.data(), 1, line_string.size(), s_log_file);
		assert(written == line_string.size());
		std::fflush(s_log_file);
	}
#endif

#if defined(_WIN32) && !defined(NDEBUG)
	// Write line to the debug output
	OutputDebugStringA(line_string.c_str());
#endif
}
