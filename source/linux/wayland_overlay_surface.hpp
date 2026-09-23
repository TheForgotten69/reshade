#pragma once

#include "input.hpp"
#include <cstdint>
#include <vector>
#include <wayland-client.h>

struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;
struct wp_viewporter;
struct wp_viewport;

namespace reshade
{
	// An owned child subsurface of the host's Vulkan surface. While unmapped it only learns the
	// compositor's preferred fractional scale for the host surface (KWin and Mutter propagate it to
	// children). That is the scale the compositor recommends, not necessarily the one the host
	// applies to its buffer. While mapped with a transparent buffer it covers the host surface and
	// takes pointer input away from it in the capture regions.
	class wayland_overlay_surface
	{
	public:
		wayland_overlay_surface() = default;
		~wayland_overlay_surface() { reset(); }
		wayland_overlay_surface(const wayland_overlay_surface &) = delete;
		wayland_overlay_surface &operator=(const wayland_overlay_surface &) = delete;

		// Offer every registry global, proxies inherit the registry's event queue.
		void bind(wl_registry *registry, uint32_t name, const char *interface);
		bool attach(wl_surface *parent);
		// Destroys every proxy. Must run before the event queue is destroyed.
		void reset();

		wl_surface *surface() const { return _surface; }
		// Zero while unknown.
		double preferred() const { return _preferred / 120.0; }

		// Maps the surface with the size of its parent in logical coordinates, taking pointer input
		// in 'regions' (relative to that size), or unmaps it when 'regions' is empty. Returns whether
		// anything changed and needs to be flushed.
		bool set_capture(const std::vector<input::capture_rect> &regions, unsigned int logical_width, unsigned int logical_height);

	private:
		static void on_preferred_scale(void *data, wp_fractional_scale_v1 *, uint32_t scale);
		bool create_transparent_buffer();

		wl_compositor *_compositor = nullptr;
		wl_subcompositor *_subcompositor = nullptr;
		wl_shm *_shm = nullptr;
		wp_viewporter *_viewporter = nullptr;
		wp_fractional_scale_manager_v1 *_fractional_manager = nullptr;
		wl_surface *_surface = nullptr;
		wl_subsurface *_subsurface = nullptr;
		wp_fractional_scale_v1 *_fractional = nullptr;
		wp_viewport *_viewport = nullptr;
		wl_buffer *_buffer = nullptr;
		uint32_t _preferred = 0;

		bool _mapped = false;
		unsigned int _mapped_size[2] = {};
		std::vector<input::capture_rect> _mapped_regions;
	};
}
