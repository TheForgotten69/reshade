#include "wayland_scale_probe.hpp"
#include "fractional-scale-v1-client-protocol.h"
#include <cstring>

void reshade::wayland_scale_probe::bind(wl_registry *registry, uint32_t name, const char *interface)
{
	if (_compositor == nullptr && std::strcmp(interface, wl_compositor_interface.name) == 0)
		_compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
	else if (_subcompositor == nullptr && std::strcmp(interface, wl_subcompositor_interface.name) == 0)
		_subcompositor = static_cast<wl_subcompositor *>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
	else if (_manager == nullptr && std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0)
		_manager = static_cast<wp_fractional_scale_manager_v1 *>(wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
}

bool reshade::wayland_scale_probe::attach(wl_surface *parent)
{
	if (_surface != nullptr || parent == nullptr || _compositor == nullptr || _subcompositor == nullptr || _manager == nullptr)
		return false;

	// Never attach a buffer to the child or commit the host surface.
	_surface = wl_compositor_create_surface(_compositor);
	if (_surface == nullptr)
		return false;
	_subsurface = wl_subcompositor_get_subsurface(_subcompositor, _surface, parent);
	if (_subsurface == nullptr)
		return false;
	_fractional = wp_fractional_scale_manager_v1_get_fractional_scale(_manager, _surface);
	if (_fractional == nullptr)
		return false;

	static const wp_fractional_scale_v1_listener listener = { on_preferred_scale };
	return wp_fractional_scale_v1_add_listener(_fractional, &listener, this) == 0;
}

void reshade::wayland_scale_probe::reset()
{
	if (_fractional != nullptr)
		wp_fractional_scale_v1_destroy(_fractional);
	if (_subsurface != nullptr)
		wl_subsurface_destroy(_subsurface);
	if (_surface != nullptr)
		wl_surface_destroy(_surface);
	if (_manager != nullptr)
		wp_fractional_scale_manager_v1_destroy(_manager);
	if (_subcompositor != nullptr)
		wl_subcompositor_destroy(_subcompositor);
	if (_compositor != nullptr)
		wl_compositor_destroy(_compositor);

	_fractional = nullptr;
	_subsurface = nullptr;
	_surface = nullptr;
	_manager = nullptr;
	_subcompositor = nullptr;
	_compositor = nullptr;
	_preferred = 0;
}

void reshade::wayland_scale_probe::on_preferred_scale(void *data, wp_fractional_scale_v1 *, uint32_t scale)
{
	static_cast<wayland_scale_probe *>(data)->_preferred = scale;
}
