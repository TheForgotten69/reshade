#include "wayland_overlay_surface.hpp"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

void reshade::wayland_overlay_surface::bind(wl_registry *registry, uint32_t name, const char *interface)
{
	const auto matches = [interface](const wl_interface &candidate) { return std::strcmp(interface, candidate.name) == 0; };

	if (_compositor == nullptr && matches(wl_compositor_interface))
		_compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
	else if (_subcompositor == nullptr && matches(wl_subcompositor_interface))
		_subcompositor = static_cast<wl_subcompositor *>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
	else if (_shm == nullptr && matches(wl_shm_interface))
		_shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
	else if (_viewporter == nullptr && matches(wp_viewporter_interface))
		_viewporter = static_cast<wp_viewporter *>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
	else if (_fractional_manager == nullptr && matches(wp_fractional_scale_manager_v1_interface))
		_fractional_manager = static_cast<wp_fractional_scale_manager_v1 *>(wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
}

bool reshade::wayland_overlay_surface::attach(wl_surface *parent)
{
	if (_surface != nullptr || parent == nullptr || _compositor == nullptr || _subcompositor == nullptr)
		return false;

	// Never commit the host surface. The child stays unmapped until it gets a buffer.
	_surface = wl_compositor_create_surface(_compositor);
	if (_surface == nullptr)
		return false;
	_subsurface = wl_subcompositor_get_subsurface(_subcompositor, _surface, parent);
	if (_subsurface == nullptr)
		return false;
	// Apply own commits immediately, rather than with the next frame the host presents.
	wl_subsurface_set_desync(_subsurface);

	if (_fractional_manager != nullptr)
	{
		_fractional = wp_fractional_scale_manager_v1_get_fractional_scale(_fractional_manager, _surface);
		static const wp_fractional_scale_v1_listener listener = { on_preferred_scale };
		wp_fractional_scale_v1_add_listener(_fractional, &listener, this);
	}
	return true;
}

void reshade::wayland_overlay_surface::reset()
{
	if (_viewport != nullptr)
		wp_viewport_destroy(_viewport);
	if (_buffer != nullptr)
		wl_buffer_destroy(_buffer);
	if (_fractional != nullptr)
		wp_fractional_scale_v1_destroy(_fractional);
	if (_subsurface != nullptr)
		wl_subsurface_destroy(_subsurface);
	if (_surface != nullptr)
		wl_surface_destroy(_surface);
	if (_fractional_manager != nullptr)
		wp_fractional_scale_manager_v1_destroy(_fractional_manager);
	if (_viewporter != nullptr)
		wp_viewporter_destroy(_viewporter);
	if (_shm != nullptr)
		wl_shm_destroy(_shm);
	if (_subcompositor != nullptr)
		wl_subcompositor_destroy(_subcompositor);
	if (_compositor != nullptr)
		wl_compositor_destroy(_compositor);

	_viewport = nullptr;
	_buffer = nullptr;
	_fractional = nullptr;
	_subsurface = nullptr;
	_surface = nullptr;
	_fractional_manager = nullptr;
	_viewporter = nullptr;
	_shm = nullptr;
	_subcompositor = nullptr;
	_compositor = nullptr;
	_preferred = 0;
	_mapped = false;
	_mapped_regions.clear();
}

bool reshade::wayland_overlay_surface::set_capture(const std::vector<input_backend::pixel_rect> &regions, unsigned int logical_width, unsigned int logical_height)
{
	if (_surface == nullptr)
		return false;

	if (regions.empty())
	{
		if (!_mapped)
			return false;
		wl_surface_attach(_surface, nullptr, 0, 0);
		wl_surface_commit(_surface);
		_mapped = false;
		_mapped_regions.clear();
		return true;
	}

	if (_mapped && _mapped_size[0] == logical_width && _mapped_size[1] == logical_height && regions == _mapped_regions)
		return false;
	if (_buffer == nullptr && !create_transparent_buffer())
		return false;

	// A single transparent pixel, stretched over the whole host surface.
	wp_viewport_set_destination(_viewport, static_cast<int32_t>(logical_width), static_cast<int32_t>(logical_height));

	wl_region *const input_region = wl_compositor_create_region(_compositor);
	for (const input_backend::pixel_rect &region : regions)
		wl_region_add(input_region, region.x, region.y, region.width, region.height);
	wl_surface_set_input_region(_surface, input_region);
	wl_region_destroy(input_region);

	if (!_mapped)
	{
		wl_surface_attach(_surface, _buffer, 0, 0);
		wl_surface_damage(_surface, 0, 0, 1, 1);
	}
	wl_surface_commit(_surface);

	_mapped = true;
	_mapped_size[0] = logical_width;
	_mapped_size[1] = logical_height;
	_mapped_regions = regions;
	return true;
}

void reshade::wayland_overlay_surface::on_preferred_scale(void *data, wp_fractional_scale_v1 *, uint32_t scale)
{
	static_cast<wayland_overlay_surface *>(data)->_preferred = scale;
}

bool reshade::wayland_overlay_surface::create_transparent_buffer()
{
	if (_shm == nullptr || _viewporter == nullptr)
		return false;

	const int fd = memfd_create("reshade-overlay", MFD_CLOEXEC);
	if (fd < 0)
		return false;
	// Zero-filled, so the pixel is fully transparent.
	if (ftruncate(fd, 4) != 0)
	{
		close(fd);
		return false;
	}
	wl_shm_pool *const pool = wl_shm_create_pool(_shm, fd, 4);
	close(fd);
	_buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);

	_viewport = wp_viewporter_get_viewport(_viewporter, _surface);
	return _buffer != nullptr && _viewport != nullptr;
}
