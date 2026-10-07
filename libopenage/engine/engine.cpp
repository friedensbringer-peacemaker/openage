// Copyright 2023-2024 the openage authors. See copying.md for legal info.

#include "engine.h"

#include "log/log.h"
#include "log/message.h"

#include "cvar/cvar.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/game.h"
#include "gamestate/game_state.h"
#include "gamestate/simulation.h"
#include "input/controller/game/controller.h"
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
		this->controller_slot = this->presenter->get_game_controller_slot();
		// XR fork: game user interface in the image (window_settings.ui)
		this->presenter->set_ui_hooks(
			[this]() { return this->query_hud(0); },
			[this](const gamestate::MapSettings &settings) { this->request_restart(settings); },
			[this]() { this->stop(); },
			map_settings);
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

HudInfo Engine::query_hud(uint64_t player) const {
	HudInfo info;
	auto simulation = this->stop_simulation.lock();
	if (not simulation) {
		return info;
	}
	auto game = simulation->try_get_game();
	if (not game) {
		return info;
	}
	info.game = true;

	if (auto resources = game->get_player_resources(player)) {
		info.player = true;
		for (size_t i = 0; i < info.resources.size() and i < resources->size(); ++i) {
			info.resources[i] = (*resources)[i];
		}
	}

	const auto &state = game->get_state();
	auto combat = state ? state->get_combat() : nullptr;
	if (combat) {
		auto status = combat->get_match_status();
		if (auto it = status.units.find(player); it != status.units.end()) {
			info.units = it->second;
		}
		if (auto it = status.alive.find(player); it != status.alive.end()) {
			info.units_and_buildings = it->second;
		}
		switch (status.result.state_for(player)) {
		case gamestate::combat::match_state_t::VICTORY:
			info.match = hud_match_t::VICTORY;
			break;
		case gamestate::combat::match_state_t::DEFEAT:
			info.match = hud_match_t::DEFEAT;
			break;
		case gamestate::combat::match_state_t::DRAW:
			info.match = hud_match_t::DRAW;
			break;
		default:
			info.match = hud_match_t::RUNNING;
			break;
		}
		if (info.match != hud_match_t::RUNNING) {
			info.decided_at = status.decided_at.to_double();
		}
	}

	auto controller = this->controller_slot ? this->controller_slot->get() : nullptr;
	if (controller) {
		auto selected = controller->get_selected_copy();
		info.selected.reserve(selected.size());
		for (auto id : selected) {
			if (combat and combat->was_removed(id)) {
				continue;
			}
			info.selected.push_back(id);
		}
		if (not info.selected.empty()) {
			HudEntity first;
			first.id = info.selected.front();
			if (combat) {
				if (auto stats = combat->get_stats_snapshot(first.id)) {
					first.name = stats->name;
					first.unit = stats->unit;
					first.building = stats->building;
					first.villager = stats->villager;
					first.ranged = stats->ranged;
					first.neutral_object = stats->ambient or stats->herdable;
				}
				auto health = combat->get_health({first.id});
				if (not health.empty()) {
					first.health = health.front().health;
					first.max_health = health.front().max_health;
					first.alive = health.front().alive;
				}
			}
			info.first = first;
		}
	}

	return info;
}

std::shared_ptr<gamestate::prod::Production> Engine::get_production() const {
	return this->production;
}

void Engine::request_restart(const gamestate::MapSettings &settings) {
	{
		std::lock_guard<std::mutex> lock{this->restart_mutex};
		this->restart_request = settings;
	}
	log::log(INFO << "Engine: restart requested (map seed " << settings.seed << ", size " << settings.size << ")");
	this->stop();
}

std::optional<gamestate::MapSettings> Engine::take_restart() {
	std::lock_guard<std::mutex> lock{this->restart_mutex};
	auto request = this->restart_request;
	this->restart_request.reset();
	return request;
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
