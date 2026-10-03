// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "renderer/frame_sink.h"


namespace openage::renderer::opengl {

/**
 * Frame sink for checks on a desktop without a window (XR fork,
 * openage-native --egl-sink-check). Plays the role of the XR layer of the
 * Quest app:
 *
 *  - own EGL display (Mesa surfaceless platform if available) and an own
 *    OpenGL ES 3 context, which the engine context shares
 *  - a consumer thread with that context reads the newest of 3 slots,
 *    synchronized with GLsync fences (written: producer -> consumer,
 *    read done: consumer -> producer, glFlush after every glFenceSync)
 *  - timed steps: input events (replay), size changes and captures of the
 *    newest slot as PNG (read back by the consumer thread)
 *
 * Has to be destroyed after the producer (the engine) has stopped.
 */
class TestFrameSink final : public FrameSink {
public:
	/// Timed step, seconds after the first frame the consumer has read.
	struct Step {
		enum class kind {
			input,
			resize,
			capture,
		};
		double at = 0.0;
		kind what = kind::input;
		SinkInputEvent event{};
		int width = 0, height = 0;
		std::string file{};
	};

	/// Counters of the run.
	struct Stats {
		uint64_t published = 0;
		uint64_t frames_read = 0;
		uint64_t reads = 0;
		uint64_t gl_errors = 0;
		size_t steps_done = 0;
		std::vector<std::string> captures{};
	};

	/**
	 * Create the EGL display and the consumer context and start the consumer thread.
	 *
	 * @param width Initial frame width.
	 * @param height Initial frame height.
	 * @param steps Timed steps, sorted by time.
	 * @param min_frames Frames the consumer must read before the run is done.
	 */
	TestFrameSink(int width, int height, std::vector<Step> steps, uint64_t min_frames);
	~TestFrameSink();

	TestFrameSink(const TestFrameSink &) = delete;
	TestFrameSink &operator=(const TestFrameSink &) = delete;

	// ---- FrameSink (producer side)
	void *egl_display() const override;
	void *egl_config() const override;
	void *share_context() const override;
	void frame_size(int &width, int &height) const override;
	unsigned acquire_target(int width, int height) override;
	void publish() override;
	size_t poll_input(SinkInputEvent *out, size_t max) override;
	bool should_close() const override;
	bool paused() const override;

	/**
	 * Wait until all steps are done and min_frames frames were read,
	 * or the consumer failed.
	 *
	 * @return false on timeout or failure.
	 */
	bool wait_done(std::chrono::milliseconds timeout);

	/// Error text of the consumer thread (empty if none).
	std::string get_error() const;

	/// Counters of the run.
	Stats get_stats() const;

	/**
	 * Steps for the input replay: Ctrl + left click spawns two entities, a drag
	 * selects them (captured while the rectangle is visible), a right click
	 * moves them.
	 *
	 * @param start Time of the first step.
	 * @param width Frame width (positions are relative to the frame size).
	 * @param height Frame height.
	 * @param capture_file PNG file for the capture during the drag.
	 */
	static std::vector<Step> replay_steps(double start, int width, int height, const std::string &capture_file);

private:
	static constexpr int slot_count = 3;

	/// GL objects and fences of a slot.
	struct Slot {
		unsigned texture = 0;
		unsigned producer_fbo = 0;
		int width = 0, height = 0;
		uint32_t generation = 0;
		void *written = nullptr;
		void *read_done = nullptr;
		unsigned read_fbo = 0;
		uint32_t read_generation = 0;
	};

	void consumer_main();
	void consumer_loop();
	void consumer_cleanup();
	bool capture(const Slot &slot, const std::string &file);
	void fail(const std::string &text);

	void *display = nullptr;
	void *config = nullptr;
	void *context = nullptr;
	bool surfaceless = false;
	void *consumer_surface = nullptr;

	mutable std::mutex mutex;
	std::condition_variable cv;
	std::array<Slot, slot_count> slots{};
	int latest = -1;
	int reading = -1;
	int writing = -1;
	uint64_t published = 0;
	uint64_t consumed = 0;

	std::deque<SinkInputEvent> input;
	std::vector<Step> steps;
	size_t next_step = 0;
	uint64_t min_frames;
	Stats stats{};
	std::string error{};
	bool done = false;

	std::atomic<uint64_t> size;
	std::atomic<bool> stop{false};
	std::thread consumer;
};

} // namespace openage::renderer::opengl
