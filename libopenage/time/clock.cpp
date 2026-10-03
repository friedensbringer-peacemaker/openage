// Copyright 2022-2023 the openage authors. See copying.md for legal info.

#include "clock.h"

#include <thread>

#include "log/log.h"


namespace openage::time {

Clock::Clock() :
	state{ClockState::INIT},
	max_tick_time{50},
	speed{1.0f},
	last_check{simclock_t::now()},
	start_time{simclock_t::now()},
	sim_time{0},
	sim_real_time{0} {
}

ClockState Clock::get_state() {
	std::shared_lock lock{this->mutex};
	return this->state;
}

void Clock::update_time() {
	bool advanced = false;
	if (this->state == ClockState::RUNNING) {
		std::unique_lock lock{this->mutex};

		auto now = simclock_t::now();
		auto passed = std::chrono::duration_cast<std::chrono::milliseconds>(now - this->last_check);
		if (passed.count() > this->max_tick_time) {
			// if too much real time passes between two time updates, we only advance time by a small amount
			// this prevents the simulation from getting out of control during unplanned stops,
			// e.g. when debugging or if you close your laptop lid
			this->sim_time += this->speed * this->max_tick_time;
			this->sim_real_time += this->max_tick_time;
			this->last_check = now;
			advanced = true;
		}
		else if (passed.count() > 0) {
			this->sim_time += this->speed * passed.count();
			this->sim_real_time += passed.count();
			// advance by whole milliseconds only, so the sub-millisecond
			// remainder is not lost and the clock does not run slow
			this->last_check += passed;
			advanced = true;
		}
		// TODO: Stop clock if it reaches 0.0s with negative speed?
	}

	if (not advanced) {
		// Nothing to do yet (less than 1 ms passed, or the clock is paused).
		// Sleep outside the lock: get_time() readers (simulation, presenter)
		// must not wait for this, and a paused clock must not spin a core.
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

time::time_t Clock::get_time() {
	std::shared_lock lock{this->mutex};

	// convert time unit from milliseconds to seconds
	return this->sim_time / 1000;
}

time::time_t Clock::get_real_time() {
	std::shared_lock lock{this->mutex};

	// convert time unit from milliseconds to seconds
	return this->sim_real_time / 1000;
}

speed_t Clock::get_speed() {
	std::shared_lock lock{this->mutex};
	return this->speed;
}

void Clock::set_speed(speed_t speed) {
	this->update_time();

	std::unique_lock lock{this->mutex};
	this->speed = speed;

	log::log(MSG(info) << "Clock speed set to " << this->speed);
}

void Clock::start() {
	std::unique_lock lock{this->mutex};

	auto now = simclock_t::now();
	this->start_time = now;
	this->last_check = now;
	this->state = ClockState::RUNNING;
}

void Clock::stop() {
	if (this->state == ClockState::RUNNING) [[likely]] {
		this->update_time();
	}

	std::unique_lock lock{this->mutex};
	this->state = ClockState::STOPPED;

	log::log(MSG(info) << "Clock stopped at "
	                   << this->sim_time << "ms (simulated) / "
	                   << this->sim_real_time << "ms (real)");
}

void Clock::pause() {
	if (this->state == ClockState::RUNNING) [[likely]] {
		this->update_time();
	}

	std::unique_lock lock{this->mutex};
	this->state = ClockState::PAUSED;

	log::log(MSG(info) << "Clock paused at "
	                   << this->sim_time << "ms (simulated) / "
	                   << this->sim_real_time << "ms (real)");
}

void Clock::resume() {
	std::unique_lock lock{this->mutex};

	if (this->state == ClockState::PAUSED) [[likely]] {
		this->last_check = simclock_t::now();
		this->state = ClockState::RUNNING;
	}

	log::log(MSG(info) << "Clock resumed at "
	                   << this->sim_time << "ms (simulated) / "
	                   << this->sim_real_time << "ms (real)");
}

} // namespace openage::time
