#pragma once

#include <wayland-client.h>
#include "fractional-scale-v1-client-protocol.h"
#include <cstring>

namespace reshade
{
	// An owned, bufferless child receives the parent's compositor preference on
	// KWin/Mutter. This is NOT a query of the host's actual buffer/viewport mapping.
	class wayland_scale_probe
	{
	public:
		wayland_scale_probe() = default;
		wayland_scale_probe(const wayland_scale_probe &) = delete;
		wayland_scale_probe &operator=(const wayland_scale_probe &) = delete;
		~wayland_scale_probe() { reset(); }
		void bind(wl_registry *registry, uint32_t name, const char *interface)
		{
			// Factories and descendants inherit the registry's private event queue.
			if (!_compositor && std::strcmp(interface, "wl_compositor") == 0)
				_compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
			else if (!_subcompositor && std::strcmp(interface, "wl_subcompositor") == 0)
				_subcompositor = static_cast<wl_subcompositor *>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
			else if (!_manager && std::strcmp(interface, "wp_fractional_scale_manager_v1") == 0)
				_manager = static_cast<wp_fractional_scale_manager_v1 *>(wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
		}
		bool attach(wl_surface *parent)
		{
			if (_surface || !parent || !_compositor || !_subcompositor || !_manager)
				return false;
			_surface = wl_compositor_create_surface(_compositor);
			if (!_surface) return false;
			_subsurface = wl_subcompositor_get_subsurface(_subcompositor, _surface, parent);
			if (!_subsurface) return false;
			_fractional = wp_fractional_scale_manager_v1_get_fractional_scale(_manager, _surface);
			if (!_fractional) return false;
			// Never map this child or commit the host surface.
			static const wp_fractional_scale_v1_listener listener = {preferred_scale};
			return wp_fractional_scale_v1_add_listener(_fractional, &listener, this) == 0;
		}
		// Zero means unknown, not 100%. Access on the queue's dispatch thread.
		double preferred() const { return _preferred / 120.0; }
		void reset()
		{
			if (_fractional) wp_fractional_scale_v1_destroy(_fractional);
			if (_subsurface) wl_subsurface_destroy(_subsurface);
			if (_surface) wl_surface_destroy(_surface);
			if (_manager) wp_fractional_scale_manager_v1_destroy(_manager);
			if (_subcompositor) wl_subcompositor_destroy(_subcompositor);
			if (_compositor) wl_compositor_destroy(_compositor);
			_fractional = nullptr; _subsurface = nullptr; _surface = nullptr;
			_manager = nullptr; _subcompositor = nullptr; _compositor = nullptr;
			_preferred = 0;
		}
	private:
		static void preferred_scale(void *data, wp_fractional_scale_v1 *, uint32_t value)
		{ static_cast<wayland_scale_probe *>(data)->_preferred = value; }
		wl_compositor *_compositor = nullptr;
		wl_subcompositor *_subcompositor = nullptr;
		wp_fractional_scale_manager_v1 *_manager = nullptr;
		wl_surface *_surface = nullptr;
		wl_subsurface *_subsurface = nullptr;
		wp_fractional_scale_v1 *_fractional = nullptr;
		uint32_t _preferred = 0;
	};
}
