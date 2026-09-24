#include "wayland_pointer.hpp"
#include <algorithm>
#include <cmath>

void reshade::wayland_pointer::set_extent(unsigned int width, unsigned int height)
{
	width = std::max(1u, width);
	height = std::max(1u, height);
	if (width == _extent[0] && height == _extent[1])
		return;

	_extent[0] = width;
	_extent[1] = height;
	_stable_batches = 0;
}

void reshade::wayland_pointer::set_preferred_scale(double scale)
{
	if (!std::isfinite(scale) || scale <= 0.0)
		return;

	if (!_scale_refuted)
		apply_scale(scale);
}

void reshade::wayland_pointer::set_overlay_active(bool active)
{
	_overlay_active = active;
	if (!active)
		set_mode(mode::passive);
	else if (_mode == mode::passive)
		set_mode(is_lock_evident() ? mode::software_relative : mode::host_absolute);
}

void reshade::wayland_pointer::enter(double x, double y)
{
	leave();
	absolute_motion(x, y);
	std::copy_n(_absolute, 2, _virtual);
	publish(_absolute);
}

void reshade::wayland_pointer::leave()
{
	if (_mode == mode::software_relative)
		set_mode(_overlay_active ? mode::host_absolute : mode::passive);

	_lock_evidence = 0;
	_absolute_in_batch = false;
	_relative_in_batch = false;
	_relative_delta[0] = _relative_delta[1] = 0.0;
	_scroll_steps = 0;
	_has_scroll_steps = false;
	_scroll_distance = 0.0;
}

void reshade::wayland_pointer::absolute_motion(double x, double y)
{
	// Some compositors repeat the lock position, which carries no movement.
	if (_mode == mode::software_relative && std::abs(x - _lock_anchor[0]) < 0.5 && std::abs(y - _lock_anchor[1]) < 0.5)
		return;

	// A host that follows the preferred scale never reports a point beyond its swapchain.
	if (!_scale_refuted && _scale > 1.0 && _stable_batches >= stable_batches_required &&
		(x * _scale > _extent[0] + 2.0 || y * _scale > _extent[1] + 2.0))
	{
		_scale_refuted = true;
		apply_scale(1.0);
	}

	_logical[0] = x;
	_logical[1] = y;
	_absolute[0] = clamp(x * _scale, 0);
	_absolute[1] = clamp(y * _scale, 1);
	_absolute_in_batch = true;
}

void reshade::wayland_pointer::relative_motion(double dx, double dy, uint64_t time)
{
	if (!_relative_in_batch)
		_batch_relative_start = time;
	_last_relative_time = time;
	_relative_delta[0] += dx;
	_relative_delta[1] += dy;
	_relative_in_batch = true;
}

void reshade::wayland_pointer::end_batch()
{
	// A pointer pushed against a screen edge also moves relatively without moving absolutely.
	if (_absolute_in_batch)
		_lock_evidence = 0;
	else if (_relative_in_batch && !is_on_edge() && _lock_evidence < lock_batches && _lock_evidence++ == 0)
		_lock_evidence_start = _batch_relative_start;

	if (_overlay_active)
	{
		if (_absolute_in_batch)
			set_mode(mode::host_absolute);
		else if (is_lock_evident())
			set_mode(mode::software_relative);
	}

	if (_mode == mode::software_relative)
	{
		for (unsigned int axis = 0; axis < 2; ++axis)
			_virtual[axis] = clamp(_virtual[axis] + _relative_delta[axis] * _scale, axis);
		publish(_virtual);
	}
	else if (_absolute_in_batch)
	{
		std::copy_n(_absolute, 2, _virtual);
		publish(_absolute);
	}

	_absolute_in_batch = false;
	_relative_in_batch = false;
	_relative_delta[0] = _relative_delta[1] = 0.0;
	if (_stable_batches < stable_batches_required)
		++_stable_batches;
}

void reshade::wayland_pointer::axis(double distance)
{
	_scroll_distance += distance;
}

void reshade::wayland_pointer::axis_discrete(int steps)
{
	_scroll_steps += steps;
	_has_scroll_steps = true;
}

int reshade::wayland_pointer::end_axis_frame()
{
	// Discrete and continuous values describe the same wheel event, prefer the discrete one.
	int delta = 0;
	if (_has_scroll_steps)
		delta = -_scroll_steps;
	else if (_scroll_distance != 0.0)
		delta = _scroll_distance > 0.0 ? -1 : 1;

	_scroll_steps = 0;
	_has_scroll_steps = false;
	_scroll_distance = 0.0;
	return delta;
}

void reshade::wayland_pointer::apply_scale(double scale)
{
	if (scale == _scale)
		return;

	const double ratio = scale / _scale;
	_scale = scale;
	_stable_batches = 0;
	for (unsigned int axis = 0; axis < 2; ++axis)
	{
		_virtual[axis] = clamp(_virtual[axis] * ratio, axis);
		_absolute[axis] = clamp(_absolute[axis] * ratio, axis);
	}
	publish(_virtual);
}

void reshade::wayland_pointer::set_mode(mode new_mode)
{
	if (new_mode == _mode)
		return;

	if (new_mode == mode::software_relative)
	{
		// Start from where the cursor was last seen, locks keep the pointer in place.
		_virtual[0] = _position[0];
		_virtual[1] = _position[1];
		std::copy_n(_logical, 2, _lock_anchor);
	}
	else if (_mode == mode::software_relative)
	{
		// Back to the host cursor, wherever the host left it.
		std::copy_n(_absolute, 2, _virtual);
		publish(_absolute);
	}
	_mode = new_mode;
}

bool reshade::wayland_pointer::is_on_edge() const
{
	for (unsigned int axis = 0; axis < 2; ++axis)
		if (_absolute[axis] <= 1.0 || _absolute[axis] >= _extent[axis] - 1.0)
			return true;
	return false;
}

double reshade::wayland_pointer::clamp(double value, unsigned int axis) const
{
	return std::clamp(value, 0.0, static_cast<double>(_extent[axis]));
}

void reshade::wayland_pointer::publish(const double position[2])
{
	for (unsigned int axis = 0; axis < 2; ++axis)
		_position[axis] = static_cast<unsigned int>(std::lround(position[axis]));
}
