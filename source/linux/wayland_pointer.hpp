#pragma once

#include <cstdint>

namespace reshade
{
	class wayland_pointer
	{
	public:
		enum class mode { passive, host_absolute, software_relative };

		void set_extent(unsigned int width, unsigned int height);
		void set_preferred_scale(double scale);
		double scale() const { return _scale; }
		bool scale_refuted() const { return _scale_refuted; }
		void set_overlay_active(bool active);
		mode current_mode() const { return _mode; }

		void enter(double x, double y);
		void leave();
		void absolute_motion(double x, double y);
		void relative_motion(double dx, double dy, uint64_t time);
		void end_batch();

		void axis(double distance);
		void axis_discrete(int steps);
		int end_axis_frame();

		unsigned int x() const { return _position[0]; }
		unsigned int y() const { return _position[1]; }

	private:
		static constexpr unsigned int lock_batches = 2;
		static constexpr uint64_t lock_duration = 25000;
		static constexpr unsigned int stable_batches_required = 30;

		void apply_scale(double scale);
		void set_mode(mode mode);
		bool is_on_edge() const;
		bool is_lock_evident() const { return _lock_evidence >= lock_batches && _last_relative_time - _lock_evidence_start >= lock_duration; }
		double clamp(double value, unsigned int axis) const;
		void publish(const double position[2]);

		unsigned int _extent[2] = { 1, 1 };
		unsigned int _stable_batches = 0;
		double _scale = 1.0;
		bool _scale_refuted = false;

		bool _overlay_active = false;
		mode _mode = mode::passive;
		unsigned int _lock_evidence = 0;
		uint64_t _lock_evidence_start = 0;
		uint64_t _batch_relative_start = 0;
		uint64_t _last_relative_time = 0;
		bool _absolute_in_batch = false;
		bool _relative_in_batch = false;
		double _logical[2] = {};
		double _lock_anchor[2] = {};
		double _absolute[2] = {};
		double _virtual[2] = {};
		double _relative_delta[2] = {};
		unsigned int _position[2] = {};

		int _scroll_steps = 0;
		bool _has_scroll_steps = false;
		double _scroll_distance = 0.0;
	};
}
