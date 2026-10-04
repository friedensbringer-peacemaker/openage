// Copyright 2023-2024 the openage authors. See copying.md for legal info.

#include "engine.h"

#include "log/log.h"
#include "log/message.h"

#include "cvar/cvar.h"
#include "gamestate/simulation.h"
#include "presenter/presenter.h"
#include "time/clock.h"
#include "time/time_loop.h"


namespace openage::engine {

Engine::Engine(mode mode,
               const util::Path &root_dir,
               const std::vector<std::string> &mods,
               const renderer::window_settings &window_settings,
               const gamestate::MapSettings &map_settings) :
	running{true},
	run_mode{mode},
	root_dir{root_dir},
	threads{} {
	log::log(INFO
	         << "launching engine with root directory"
	         << root_dir);

	// read and apply the configuration files
	this->cvar_manager = std::make_shared<cvar::CVarManager>(this->root_dir["cfg"]);
	cvar_manager->load_all();

	// time loop
	this->time_loop = std::make_shared<time::TimeLoop>();

	// game simulation
	// this is run in the main thread
	this->simulation = std::make_shared<gamestate::GameSimulation>(this->root_dir,
	                                                               this->cvar_manager,
	                                                               this->time_loop);
	this->simulation->set_modpacks(mods);
	this->simulation->set_map_settings(map_settings);

	this->stop_simulation = this->simulation;
	this->stop_time_loop = this->time_loop;
	this->production = this->simulation->get_production();

	// presenter (optional)
	if (this->run_mode == mode::FULL) {
		this->presenter = std::make_shared<presenter::Presenter>(this->root_dir,
		                                                         this->simulation,
		                                                         this->time_loop);
		this->stop_presenter = this->presenter->get_stop_flag();
	}

	// spawn thread to run time loop
	this->threads.emplace_back([this]() {
		this->time_loop->run();

		this->time_loop.reset();
	});

	// if presenter is used, run it in a separate thread
	if (this->run_mode == mode::FULL) {
		// capture the settings by value: the caller's object may be gone
		// before the presenter thread reads it
		this->threads.emplace_back([this, window_settings]() {
			this->presenter->run(window_settings);

			// Make sure that the presenter gets destructed in the same thread
			// otherwise OpenGL complains about missing contexts
			this->presenter.reset();
			this->running = false;
		});
	}

	log::log(INFO << "Using " << this->threads.size() + 1 << " threads "
	              << "(" << std::jthread::hardware_concurrency() << " available)");
}

Engine::~Engine() {
	// the members are destroyed before the jthreads (declared first), but the
	// threads still use them, e.g. the presenter thread resets this->presenter
	for (auto &thread : this->threads) {
		if (thread.joinable()) {
			thread.join();
		}
	}
}

void Engine::stop() {
	if (auto simulation = this->stop_simulation.lock()) {
		simulation->stop();
	}
	if (auto time_loop = this->stop_time_loop.lock()) {
		time_loop->stop();
	}
	if (this->stop_presenter) {
		*this->stop_presenter = true;
	}
}

std::shared_ptr<time::Clock> Engine::get_clock() const {
	if (auto time_loop = this->stop_time_loop.lock()) {
		return time_loop->get_clock();
	}
	return nullptr;
}

std::shared_ptr<gamestate::prod::Production> Engine::get_production() const {
	return this->production;
}

void Engine::loop() {
	// Run the main game simulation loop:
	this->simulation->run();

	// After stopping, clean up the simulation
	this->simulation.reset();
	if (this->run_mode != mode::FULL) {
		this->running = false;
	}
}

} // namespace openage::engine
