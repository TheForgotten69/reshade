#pragma once

#include <cerrno>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace reshade::utils
{
	// Runs only on the clipboard worker thread. Keep SIGPIPE blocked until that
	// thread exits, so a recipient closing its pipe cannot terminate the host.
	inline void write_clipboard_text(int fd, const std::string &text)
	{
		sigset_t signals;
		sigemptyset(&signals);
		sigaddset(&signals, SIGPIPE);
		if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0)
		{
			size_t written = 0;
			while (written < text.size())
			{
				const ssize_t count = write(fd, text.data() + written, text.size() - written);
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0)
					break;
				written += static_cast<size_t>(count);
			}
		}
		close(fd);
	}
}
