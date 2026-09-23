#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <wayland-client.h>

namespace reshade
{
	// Clipboard integration through the core 'wl_data_device' protocol. All proxies live on the
	// owning backend's private event queue.
	class wayland_clipboard
	{
	public:
		struct offer_info
		{
			bool has_text = false;
			std::string mime_type;

			void add_mime_type(const char *type);
		};

		wayland_clipboard() = default;
		~wayland_clipboard() { reset(); }
		wayland_clipboard(const wayland_clipboard &) = delete;
		wayland_clipboard &operator=(const wayland_clipboard &) = delete;

		void bind_manager(wl_registry *registry, uint32_t name, uint32_t version, wl_event_queue *queue);
		void attach(wl_display *display, wl_seat *seat, wl_event_queue *queue);
		// Destroys every proxy. Must run before the event queue is destroyed.
		void reset();

		// 'serial' is the latest input serial, which Wayland requires to change the selection.
		void set_text(const char *text, uint32_t serial);
		// ImGui asks for the text synchronously on the render thread, so the wait for the source
		// client is bounded and a slow one is treated as having no text.
		std::string text();

		void on_data_offer(wl_data_offer *offer);
		void on_drag_enter(wl_data_offer *offer);
		void on_selection(wl_data_offer *offer);
		void on_offer_mime_type(wl_data_offer *offer, const char *mime_type);
		void on_send(int32_t fd);
		void on_cancelled(wl_data_source *source);

	private:
		void forget_offer(wl_data_offer *offer);

		wl_display *_display = nullptr;
		wl_event_queue *_queue = nullptr;
		wl_data_device_manager *_manager = nullptr;
		wl_data_device *_device = nullptr;

		// Every offer the compositor announced, until it is identified as the selection or discarded.
		std::unordered_map<wl_data_offer *, offer_info> _offers;
		wl_data_offer *_selection = nullptr;

		wl_data_source *_source = nullptr;
		std::string _source_text;
		// Shared with detached send threads, which may outlive this object.
		std::shared_ptr<std::atomic<int>> _sends_in_flight = std::make_shared<std::atomic<int>>(0);
	};
}
