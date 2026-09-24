#include "wayland_clipboard.hpp"
#include "clipboard.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <poll.h>
#include <wayland-client.h>
#include <unistd.h>

namespace
{
	constexpr size_t max_text_size = 16 * 1024 * 1024;
	// A peer that never reads its pipe would otherwise accumulate send threads indefinitely.
	constexpr int max_sends_in_flight = 4;
	constexpr auto receive_timeout = std::chrono::milliseconds(100);
	constexpr const char *text_mime_types[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING" };

	template <typename T>
	void set_queue(T *proxy, wl_event_queue *queue)
	{
		wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(proxy), queue);
	}

	auto self(void *data) { return static_cast<reshade::wayland_clipboard *>(data); }

	const wl_registry_listener registry_listener = {
		[](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) { self(data)->on_global(registry, name, interface, version); },
		[](void *, wl_registry *, uint32_t) {},
	};
	const wl_data_offer_listener offer_listener = {
		[](void *data, wl_data_offer *offer, const char *mime_type) { self(data)->on_offer_mime_type(offer, mime_type); },
		[](void *, wl_data_offer *, uint32_t) {},
		[](void *, wl_data_offer *, uint32_t) {},
	};
	const wl_data_device_listener device_listener = {
		[](void *data, wl_data_device *, wl_data_offer *offer) { self(data)->on_data_offer(offer); },
		[](void *data, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t, wl_data_offer *offer) { self(data)->on_drag_enter(offer); },
		[](void *, wl_data_device *) {},
		[](void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {},
		[](void *, wl_data_device *) {},
		[](void *data, wl_data_device *, wl_data_offer *offer) { self(data)->on_selection(offer); },
	};
	const wl_data_source_listener source_listener = {
		[](void *, wl_data_source *, const char *) {},
		[](void *data, wl_data_source *, const char *, int32_t fd) { self(data)->on_send(fd); },
		[](void *data, wl_data_source *source) { self(data)->on_cancelled(source); },
		[](void *, wl_data_source *) {},
		[](void *, wl_data_source *) {},
		[](void *, wl_data_source *, uint32_t) {},
	};
}

void reshade::wayland_clipboard::offer_info::add_mime_type(const char *type)
{
	if (std::none_of(std::begin(text_mime_types), std::end(text_mime_types), [type](const char *text_type) { return std::strcmp(type, text_type) == 0; }))
		return;

	has_text = true;
	// Prefer explicit UTF-8 over the legacy text types.
	if (mime_type.empty() || std::strcmp(type, text_mime_types[0]) == 0)
		mime_type = type;
}

reshade::wayland_clipboard *reshade::wayland_clipboard::get(wl_display *display)
{
	// Never destroyed, see the class description. Freeing it at exit could also touch a connection
	// the host already closed.
	static std::mutex s_mutex;
	static std::unordered_map<wl_display *, wayland_clipboard *> s_clipboards;

	const std::lock_guard<std::mutex> lock(s_mutex);
	const auto [it, inserted] = s_clipboards.try_emplace(display, nullptr);
	if (inserted)
	{
		auto *const clipboard = new wayland_clipboard(display);
		if (clipboard->initialize())
			it->second = clipboard;
		else
			delete clipboard;
	}
	return it->second;
}

reshade::wayland_clipboard::~wayland_clipboard()
{
	// Only reached when initialization failed, which is before the data device exists.
	if (_seat != nullptr)
		wl_seat_destroy(_seat);
	if (_manager != nullptr)
		wl_data_device_manager_destroy(_manager);
	if (_registry != nullptr)
		wl_registry_destroy(_registry);
	if (_queue != nullptr)
		wl_event_queue_destroy(_queue);
}

bool reshade::wayland_clipboard::initialize()
{
	_queue = wl_display_create_queue(_display);
	auto *const display_wrapper = _queue != nullptr ? static_cast<wl_display *>(wl_proxy_create_wrapper(_display)) : nullptr;
	if (display_wrapper == nullptr)
		return false;
	set_queue(display_wrapper, _queue);
	_registry = wl_display_get_registry(display_wrapper);
	wl_proxy_wrapper_destroy(display_wrapper);
	if (_registry == nullptr)
		return false;
	wl_registry_add_listener(_registry, &registry_listener, this);

	if (wl_display_roundtrip_queue(_display, _queue) < 0 || _manager == nullptr || _seat == nullptr)
		return false;
	_device = wl_data_device_manager_get_data_device(_manager, _seat);
	set_queue(_device, _queue);
	wl_data_device_add_listener(_device, &device_listener, this);
	return true;
}

void reshade::wayland_clipboard::dispatch()
{
	const std::lock_guard<std::mutex> lock(_mutex);
	wl_display_dispatch_queue_pending(_display, _queue);
}

void reshade::wayland_clipboard::on_global(wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	if (_manager == nullptr && std::strcmp(interface, wl_data_device_manager_interface.name) == 0)
	{
		_manager = static_cast<wl_data_device_manager *>(wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3u)));
		set_queue(_manager, _queue);
	}
	else if (_seat == nullptr && std::strcmp(interface, wl_seat_interface.name) == 0)
	{
		_seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
		set_queue(_seat, _queue);
	}
}

void reshade::wayland_clipboard::set_text(const char *text, uint32_t serial)
{
	const std::lock_guard<std::mutex> lock(_mutex);

	_source_text.assign(text, std::min(std::strlen(text), max_text_size));
	if (_source != nullptr)
		wl_data_source_destroy(_source);
	_source = wl_data_device_manager_create_data_source(_manager);
	set_queue(_source, _queue);
	wl_data_source_add_listener(_source, &source_listener, this);
	for (const char *mime_type : text_mime_types)
		wl_data_source_offer(_source, mime_type);
	wl_data_device_set_selection(_device, _source, serial);
}

std::string reshade::wayland_clipboard::text()
{
	const std::lock_guard<std::mutex> lock(_mutex);

	// Our own source is served on the same queue, which cannot dispatch while waiting here.
	if (_source != nullptr)
		return _source_text;

	const auto it = _offers.find(_selection);
	if (it == _offers.end() || !it->second.has_text)
		return {};

	int fds[2];
	if (pipe(fds) != 0)
		return {};
	wl_data_offer_receive(_selection, it->second.mime_type.c_str(), fds[1]);
	close(fds[1]);
	wl_display_flush(_display);

	std::string result;
	char buffer[4096];
	const auto deadline = std::chrono::steady_clock::now() + receive_timeout;
	while (result.size() < max_text_size)
	{
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		pollfd pfd = { fds[0], POLLIN, 0 };
		if (remaining <= 0 || poll(&pfd, 1, static_cast<int>(remaining)) <= 0)
			break;
		const ssize_t count = read(fds[0], buffer, sizeof(buffer));
		if (count <= 0)
			break;
		result.append(buffer, std::min(static_cast<size_t>(count), max_text_size - result.size()));
	}
	close(fds[0]);
	return result;
}

void reshade::wayland_clipboard::on_data_offer(wl_data_offer *offer)
{
	set_queue(offer, _queue);
	wl_data_offer_add_listener(offer, &offer_listener, this);
	_offers.try_emplace(offer);
}

void reshade::wayland_clipboard::on_drag_enter(wl_data_offer *offer)
{
	// Drag and drop is not supported.
	if (offer != nullptr && offer != _selection)
		forget_offer(offer);
}

void reshade::wayland_clipboard::on_selection(wl_data_offer *offer)
{
	if (_selection != nullptr && _selection != offer)
		forget_offer(_selection);
	_selection = offer;
}

void reshade::wayland_clipboard::on_offer_mime_type(wl_data_offer *offer, const char *mime_type)
{
	if (const auto it = _offers.find(offer); it != _offers.end())
		it->second.add_mime_type(mime_type);
}

void reshade::wayland_clipboard::on_send(int32_t fd)
{
	// The receiving client may read slowly or never, so write on a detached thread that only owns
	// copies of what it needs and therefore may outlive this object.
	const std::shared_ptr<std::atomic<int>> in_flight = _sends_in_flight;
	if (in_flight->fetch_add(1, std::memory_order_relaxed) >= max_sends_in_flight)
	{
		in_flight->fetch_sub(1, std::memory_order_relaxed);
		close(fd);
		return;
	}

	try
	{
		std::thread([in_flight, fd, text = _source_text]() {
			utils::write_clipboard_text(fd, text);
			in_flight->fetch_sub(1, std::memory_order_relaxed);
		}).detach();
	}
	catch (...)
	{
		in_flight->fetch_sub(1, std::memory_order_relaxed);
		close(fd);
	}
}

void reshade::wayland_clipboard::on_cancelled(wl_data_source *source)
{
	if (source != _source)
		return;

	wl_data_source_destroy(_source);
	_source = nullptr;
}

void reshade::wayland_clipboard::forget_offer(wl_data_offer *offer)
{
	_offers.erase(offer);
	wl_data_offer_destroy(offer);
}
