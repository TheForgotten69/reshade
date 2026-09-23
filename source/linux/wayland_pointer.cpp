#include "wayland_pointer.hpp"
#include <algorithm>
#include <cmath>

void reshade::wayland_pointer::set_extent(unsigned int width, unsigned int height)
{
	_extent[0] = std::max(1u, width);
	_extent[1] = std::max(1u, height);
}

void reshade::wayland_pointer::set_scale(double scale)
{
	if (!std::isfinite(scale) || scale <= 0.0 || scale == _scale)
		return;

	const double ratio = scale / _scale;
	_scale = scale;
	for (unsigned int axis = 0; axis < 2; ++axis)
	{
		_virtual[axis] = clamp(_virtual[axis] * ratio, axis);
		_absolute[axis] = clamp(_absolute[axis] * ratio, axis);
	}
	publish(_virtual);
}

void reshade::wayland_pointer::set_software_cursor(bool active)
{
	if (_software_cursor == active)
		return;

	_software_cursor = active;
	_relative_session = false;
	_relative_delta[0] = _relative_delta[1] = 0.0;
	_virtual[0] = _position[0];
	_virtual[1] = _position[1];
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
	_relative_session = false;
	_absolute_in_batch = false;
	_relative_delta[0] = _relative_delta[1] = 0.0;
	_scroll_steps = 0;
	_has_scroll_steps = false;
	_scroll_distance = 0.0;
}

void reshade::wayland_pointer::absolute_motion(double x, double y)
{
	if (_relative_session)
		return;

	_absolute[0] = clamp(x * _scale, 0);
	_absolute[1] = clamp(y * _scale, 1);
	_absolute_in_batch = true;
}

void reshade::wayland_pointer::relative_motion(double dx, double dy)
{
	if (!_software_cursor)
		return;

	_relative_session = true;
	_relative_delta[0] += dx;
	_relative_delta[1] += dy;
}

void reshade::wayland_pointer::end_batch()
{
	if (_relative_session)
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
	_relative_delta[0] = _relative_delta[1] = 0.0;
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

double reshade::wayland_pointer::clamp(double value, unsigned int axis) const
{
	return std::clamp(value, 0.0, static_cast<double>(_extent[axis]));
}

void reshade::wayland_pointer::publish(const double position[2])
{
	for (unsigned int axis = 0; axis < 2; ++axis)
		_position[axis] = static_cast<unsigned int>(std::lround(position[axis]));
}
