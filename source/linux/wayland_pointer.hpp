#pragma once

namespace reshade
{
	// Protocol-free pointer model for one Wayland surface. Wayland reports surface-local logical
	// coordinates; 'scale' maps them to swapchain pixels. Motion is collected per dispatch batch and
	// resolved once in 'end_batch', so a host recenter within a batch cannot overwrite a relative
	// movement from the same batch.
	class wayland_pointer
	{
	public:
		void set_extent(unsigned int width, unsigned int height);
		// Switches the logical-to-framebuffer factor, keeping the cursor on the same logical point.
		// Non-positive or non-finite values are ignored.
		void set_scale(double scale);
		double scale() const { return _scale; }
		// While the overlay draws its own cursor, relative motion drives it (see 'relative_motion').
		void set_software_cursor(bool active);

		void enter(double x, double y);
		void leave();
		void absolute_motion(double x, double y);
		// Once relative motion arrives during a software cursor session, it keeps driving the cursor
		// until the session ends, and absolute motion is ignored.
		void relative_motion(double dx, double dy);
		void end_batch();

		void axis(double distance);
		void axis_discrete(int steps);
		// Ends a 'wl_pointer.frame' and returns the accumulated wheel delta in notches.
		int end_axis_frame();

		unsigned int x() const { return _position[0]; }
		unsigned int y() const { return _position[1]; }

	private:
		double clamp(double value, unsigned int axis) const;
		void publish(const double position[2]);

		unsigned int _extent[2] = { 1, 1 };
		double _scale = 1.0;
		bool _software_cursor = false;
		bool _relative_session = false;
		bool _absolute_in_batch = false;
		double _absolute[2] = {};
		double _virtual[2] = {};
		double _relative_delta[2] = {};
		unsigned int _position[2] = {};

		int _scroll_steps = 0;
		bool _has_scroll_steps = false;
		double _scroll_distance = 0.0;
	};
}
