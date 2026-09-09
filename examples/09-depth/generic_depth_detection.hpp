#pragma once

#include <cstdint>

namespace depth_detection
{
	// Bit masks permit lossless merging across command lists. Unknown is an
	// explicit bit, so unsupported state cannot accidentally vote for normal Z.
	enum evidence : uint8_t { none = 0, normal = 1, reversed = 2, unknown = 4 };
	constexpr uint8_t clear_evidence(float depth)
	{
		return depth == 1.0f ? normal : depth == 0.0f ? reversed : unknown;
	}

	struct observation
	{
		static constexpr uint32_t frame_limit = 600;
		static constexpr uint32_t required_consistent_frames = 120;
		uint64_t resource = 0;
		uint64_t last_frame = UINT64_MAX;
		uint32_t frames = 0;
		uint32_t consistent_frames = 0;
		uint8_t candidate = none;
		bool finished = false;

		// Call once for each actual frame with a selected, rendered depth buffer.
		// A buffer change resets confidence, but does not extend the total budget.
		uint8_t observe(uint64_t selected, uint64_t frame, uint8_t clears, uint8_t comparisons)
		{
			if (finished || selected == 0 || frame == last_frame)
				return none;
			last_frame = frame;
			++frames;
			if (resource != selected)
			{
				resource = selected;
				candidate = none;
				consistent_frames = 0;
			}
			const uint8_t vote = (clears == normal && comparisons == normal) ? normal :
				(clears == reversed && comparisons == reversed) ? reversed : none;
			if (vote == none || vote != candidate)
			{
				candidate = vote;
				consistent_frames = 0;
			}
			if (vote != none && ++consistent_frames >= required_consistent_frames)
			{
				finished = true;
				return vote;
			}
			finished = frames >= frame_limit;
			return none;
		}
	};
}
