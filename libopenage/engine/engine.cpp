// Copyright 2023-2024 the openage authors. See copying.md for legal info.

#include "engine.h"

#include <algorithm>

#include "log/log.h"
#include "log/message.h"

#include "cvar/cvar.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/game.h"
#include "gamestate/game_state.h"
#include "gamestate/production.h"
#include "gamestate/save_game.h"
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
	// XR fork (save games)
	this->map_settings = map_settings;
	this->map_settings.load_file.clear();
	this->save_dir = window_settings.save_dir;

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
			this->map_settings);
		// XR fork (save games): slots of the game menu
		this->presenter->set_ui_save_hooks(
			[this](int slot) { return this->save_slot(slot); },
			[this](int slot) { return this->load_slot(slot); },
			[this]() { return this->list_save_slots(); });
		this->focus_presenter = this->presenter;
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
				// XR fork (AoE layout): names of a multiple selection for the portraits
				for (size_t i = 0; i < info.selected.size() and i < HudInfo::MAX_SELECTED_NAMES; ++i) {
					auto stats = combat->get_stats_snapshot(info.selected[i]);
					info.selected_names.push_back(stats ? stats->name : std::string{});
				}
				if (auto stats = combat->get_stats_snapshot(first.id)) {
					first.name = stats->name;
					first.unit = stats->unit;
					first.building = stats->building;
					first.villager = stats->villager;
					first.ranged = stats->ranged;
					first.neutral_object = stats->ambient or stats->herdable;
					// combat values for the selection panel
					first.has_stats = stats->alive;
					for (const auto &effect : stats->attack.effects) {
						first.attack = std::max(first.attack, effect.amount);
					}
					for (const auto &[type, amount] : stats->armor.block) {
						if (type.find("Melee") != std::string::npos) {
							first.armor_melee = std::max(first.armor_melee, amount);
						}
						else if (type.find("Pierce") != std::string::npos) {
							first.armor_pierce = std::max(first.armor_pierce, amount);
						}
					}
					first.range = stats->ranged ? stats->max_range : 0.0;
					const auto &n = stats->name;
					first.mounted = n.find("Knight") != std::string::npos or n.find("Cavalry") != std::string::npos
					                or n.find("Scout") != std::string::npos or n.find("Camel") != std::string::npos;
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

// ---- save games (XR fork) ----

const gamestate::MapSettings &Engine::get_map_settings() const {
	return this->map_settings;
}

bool Engine::post(gamestate::GameSimulation::Job job) {
	auto simulation = this->stop_simulation.lock();
	if (not simulation) {
		return false;
	}
	simulation->post(std::move(job));
	return true;
}

bool Engine::save_file(const std::string &file,
                       const std::string &title,
                       std::function<void(bool, const std::string &)> done) {
	auto simulation = this->stop_simulation.lock();
	if (not simulation) {
		if (done) {
			done(false, "Kein Spiel");
		}
		return false;
	}
	const gamestate::MapSettings map = this->map_settings;
	simulation->post([file, title, done, map](const std::shared_ptr<gamestate::Game> &game, const time::time_t &time) {
		if (not game) {
			if (done) {
				done(false, "Kein Spiel");
			}
			return;
		}
		auto data = gamestate::save::capture(game->get_state(), map, game->get_generated_entity_range(), time, title);
		std::string error;
		const bool ok = gamestate::save::write_text(file, gamestate::save::serialize(data), error);
		if (ok) {
			log::log(INFO << "Save: written " << file << " (" << data.entities.size() << " entities, t="
			              << data.game_time << " s)");
		}
		else {
			log::log(ERR << "Save: " << error);
		}
		if (done) {
			done(ok, ok ? file : error);
		}
	});
	return true;
}

bool Engine::save_slot(int slot) {
	auto simulation = this->stop_simulation.lock();
	if (not simulation or this->save_dir.empty() or not gamestate::save::slot_valid(slot)) {
		log::log(WARN << "Save: no save directory or no game (slot " << slot << ")");
		return false;
	}
	const gamestate::MapSettings map = this->map_settings;
	const std::string dir = this->save_dir;
	auto production = this->production;
	simulation->post([slot, dir, map, production](const std::shared_ptr<gamestate::Game> &game, const time::time_t &time) {
		if (not game) {
			return;
		}
		const bool autosave = slot == gamestate::save::AUTOSAVE_SLOT;
		const std::string title = (autosave ? "Automatisch: " : "Slot " + std::to_string(slot) + ": ")
		                          + gamestate::save::map_text(map);
		auto data = gamestate::save::capture(game->get_state(), map, game->get_generated_entity_range(), time, title);
		std::string error;
		if (gamestate::save::write_slot(dir, slot, data, error)) {
			log::log(INFO << "Save: slot " << slot << " written (" << gamestate::save::slot_file(dir, slot).string()
			              << ", " << data.entities.size() << " entities, t=" << data.game_time << " s)");
			if (production) {
				production->notify(autosave ? "Automatisch gespeichert" : "Gespeichert: Slot " + std::to_string(slot),
				                   gamestate::prod::status_t::good);
			}
		}
		else {
			log::log(ERR << "Save: slot " << slot << ": " << error);
			if (production) {
				production->notify("Speichern fehlgeschlagen: " + error, gamestate::prod::status_t::warn);
			}
		}
	});
	return true;
}

std::string Engine::load_file(const std::string &file) {
	gamestate::save::SaveData data;
	std::string error;
	if (not gamestate::save::read_save(file, data, error)) {
		log::log(WARN << "Load: " << file << ": " << error);
		if (this->production) {
			this->production->notify("Laden fehlgeschlagen: " + error, gamestate::prod::status_t::warn);
		}
		return error;
	}
	gamestate::MapSettings settings = data.map;
	settings.load_file = file;
	log::log(INFO << "Load: " << file << " (" << data.title << ", t=" << data.game_time << " s): restart requested");
	this->request_restart(settings);
	return {};
}

std::string Engine::load_slot(int slot) {
	if (this->save_dir.empty() or not gamestate::save::slot_valid(slot)) {
		return "Kein Speicherordner";
	}
	return this->load_file(gamestate::save::slot_file(this->save_dir, slot).string());
}

std::vector<gamestate::save::SlotInfo> Engine::list_save_slots() const {
	if (this->save_dir.empty()) {
		return {};
	}
	return gamestate::save::list_slots(this->save_dir);
}

void Engine::focus_entity(uint64_t id) {
	if (auto presenter = this->focus_presenter.lock()) {
		presenter->focus_entity(id);
	}
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
