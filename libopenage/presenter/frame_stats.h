// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>


namespace openage::presenter {

/**
 * Frame times of the presenter loop (XR fork), summed over a report period
 * (default 5 s) for one log line. Dependency-free for host tests
 * (gamestate/selection_check.cpp).
 *
 * Times in seconds: events (window system/GUI events), render (issue the
 * render passes), present (window update: swap buffers or publish to the
 * frame sink, which waits for vsync or the consumer).
 */
class FrameStats {
public:
	explicit FrameStats(double period = 5.0) :
		period{period} {}

	/**
	 * Add a frame.
	 *
	 * @param events Time of the event processing.
	 * @param render Time of the render passes.
	 * @param present Time of the buffer swap/publish.
	 */
	void add(double events, double render, double present) {
		double frame = events + render + present;
		this->frames += 1;
		this->sum_events += events;
		this->sum_render += render;
		this->sum_present += present;
		this->sum_frame += frame;
		this->max_frame = std::max(this->max_frame, frame);
	}

	/**
	 * Whether the report period is over.
	 *
	 * @param elapsed Seconds since the last reset.
	 */
	bool due(double elapsed) const {
		return elapsed >= this->period and this->frames > 0;
	}

	/// frames since the last reset
	size_t count() const {
		return this->frames;
	}

	/// frames per second over \p elapsed seconds
	double fps(double elapsed) const {
		return elapsed > 0.0 ? static_cast<double>(this->frames) / elapsed : 0.0;
	}

	/// average frame time in milliseconds
	double avg_frame_ms() const {
		return this->frames > 0 ? 1000.0 * this->sum_frame / static_cast<double>(this->frames) : 0.0;
	}

	/**
	 * Log line, e.g. "58.9 fps over 5.0 s, frame avg 17.0 ms max 33.1 ms
	 * (events 0.2, render 4.1, present 12.7 ms avg)".
	 *
	 * @param elapsed Seconds since the last reset.
	 */
	std::string report(double elapsed) const {
		double n = this->frames > 0 ? static_cast<double>(this->frames) : 1.0;
		char line[256];
		std::snprintf(line, sizeof(line),
		              "%.1f fps over %.1f s, frame avg %.1f ms max %.1f ms "
		              "(events %.1f, render %.1f, present %.1f ms avg)",
		              this->fps(elapsed), elapsed, this->avg_frame_ms(), 1000.0 * this->max_frame,
		              1000.0 * this->sum_events / n, 1000.0 * this->sum_render / n,
		              1000.0 * this->sum_present / n);
		return line;
	}

	/// start a new period
	void reset() {
		*this = FrameStats{this->period};
	}

private:
	double period;
	size_t frames = 0;
	double sum_events = 0.0;
	double sum_render = 0.0;
	double sum_present = 0.0;
	double sum_frame = 0.0;
	double max_frame = 0.0;
};

} // namespace openage::presenter
