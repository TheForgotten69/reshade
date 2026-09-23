#pragma once

#include <cstdint>
#include <wayland-client.h>

struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;

namespace reshade
{
	// Learns the compositor's preferred fractional scale for a foreign surface through an owned,
	// never mapped child subsurface (KWin and Mutter propagate the preference to children). This is
	// the scale the compositor recommends, not necessarily the one the host applies to its buffer.
	class wayland_scale_probe
	{
	public:
		wayland_scale_probe() = default;
		~wayland_scale_probe() { reset(); }
		wayland_scale_probe(const wayland_scale_probe &) = delete;
		wayland_scale_probe &operator=(const wayland_scale_probe &) = delete;

		// Offer every registry global, proxies inherit the registry's event queue.
		void bind(wl_registry *registry, uint32_t name, const char *interface);
		bool attach(wl_surface *parent);
		// Destroys every proxy. Must run before the event queue is destroyed.
		void reset();

		// Zero while unknown.
		double preferred() const { return _preferred / 120.0; }

	private:
		static void on_preferred_scale(void *data, wp_fractional_scale_v1 *, uint32_t scale);

		wl_compositor *_compositor = nullptr;
		wl_subcompositor *_subcompositor = nullptr;
		wp_fractional_scale_manager_v1 *_manager = nullptr;
		wl_surface *_surface = nullptr;
		wl_subsurface *_subsurface = nullptr;
		wp_fractional_scale_v1 *_fractional = nullptr;
		uint32_t _preferred = 0;
	};
}
