#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <wayland-client.h>

namespace reshade
{
	class wayland_clipboard
	{
	public:
		struct offer_info
		{
			bool has_text = false;
			std::string mime_type;

			void add_mime_type(const char *type);
		};

		static wayland_clipboard *get(wl_display *display);

		void dispatch();
		void set_text(const char *text, uint32_t serial);
		std::string text();

		void on_global(wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
		void on_data_offer(wl_data_offer *offer);
		void on_drag_enter(wl_data_offer *offer);
		void on_selection(wl_data_offer *offer);
		void on_offer_mime_type(wl_data_offer *offer, const char *mime_type);
		void on_send(int32_t fd);
		void on_cancelled(wl_data_source *source);

	private:
		explicit wayland_clipboard(wl_display *display) : _display(display) {}
		~wayland_clipboard();
		bool initialize();
		void forget_offer(wl_data_offer *offer);

		std::mutex _mutex;
		wl_display *const _display;
		wl_event_queue *_queue = nullptr;
		wl_registry *_registry = nullptr;
		wl_seat *_seat = nullptr;
		wl_data_device_manager *_manager = nullptr;
		wl_data_device *_device = nullptr;

		std::unordered_map<wl_data_offer *, offer_info> _offers;
		wl_data_offer *_selection = nullptr;

		wl_data_source *_source = nullptr;
		std::string _source_text;
		std::shared_ptr<std::atomic<int>> _sends_in_flight = std::make_shared<std::atomic<int>>(0);
	};
}
