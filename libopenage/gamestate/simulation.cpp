// Copyright 2013-2024 the openage authors. See copying.md for legal info.

#include "simulation.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <thread>

#include "log/log.h"
#include "log/message.h"

#include "assets/mod_manager.h"
#include "event/event_loop.h"
#include "gamestate/combat/events.h"
#include "gamestate/entity_factory.h"
#include "gamestate/event/drag_select.h"
#include "gamestate/event/process_command.h"
#include "gamestate/event/send_command.h"
#include "gamestate/event/spawn_entity.h"
#include "gamestate/event/wait.h"
#include "gamestate/production.h"
#include "gamestate/save_format.h"
#include "gamestate/save_game.h"
#include "gamestate/terrain_factory.h"
#include "time/clock.h"
#include "time/time_loop.h"

// TODO
#include "gamestate/game.h"
#include "gamestate/game_state.h"

namespace openage::gamestate {

GameSimulation::GameSimulation(const util::Path &root_dir,
                               const std::shared_ptr<cvar::CVarManager> &cvar_manager,
                               const std::shared_ptr<openage::time::TimeLoop> time_loop) :
	running{false},
	root_dir{root_dir},
	cvar_manager{cvar_manager},
	time_loop{time_loop},
	event_loop{std::make_shared<openage::event::EventLoop>()},
	entity_factory{std::make_shared<gamestate::EntityFactory>()},
	terrain_factory{std::make_shared<gamestate::TerrainFactory>()},
	mod_manager{std::make_shared<assets::ModManager>(this->root_dir / "assets" / "converted")},
	spawner{std::make_shared<gamestate::event::Spawner>(this->event_loop)},
	commander{std::make_shared<gamestate::event::Commander>(this->event_loop)},
	production{std::make_shared<gamestate::prod::Production>()} {
	auto mods = mod_manager->enumerate_modpacks(root_dir / "assets" / "converted");
	for (const auto &mod : mods) {
		this->mod_manager->register_modpack(mod);
	}

	log::log(MSG(info) << "Created game simulation");
}


void GameSimulation::run() {
	this->start();
	time::time_t last_time = -1;
	// XR fork: share of the time the simulation works (log every 5 s)
	using clock_t = std::chrono::steady_clock;
	auto stats_start = clock_t::now();
	double busy = 0.0;
	double busy_max = 0.0;
	size_t steps = 0;
	while (this->running) {
		auto step_start = clock_t::now();
		time::time_t current_time = this->time_loop->get_clock()->get_time();
		this->event_loop->reach_time(current_time, this->game->get_state());
		// XR fork (production): requests of the HUD/input, training queues
		this->production->update(this->game->get_state(), this->event_loop, this->entity_factory, current_time);
		// XR fork: posted jobs (save games, lookups)
		this->run_jobs(current_time);

		auto step_end = clock_t::now();
		double step = std::chrono::duration<double>(step_end - step_start).count();
		busy += step;
		busy_max = std::max(busy_max, step);
		steps += 1;
		double elapsed = std::chrono::duration<double>(step_end - stats_start).count();
		if (elapsed >= 5.0) {
			log::log(INFO << "Simulation: busy " << static_cast<int>(100.0 * busy / elapsed + 0.5) << " % of "
			              << elapsed << " s, " << steps << " steps, longest " << 1000.0 * busy_max << " ms, game time "
			              << current_time.to_double() << " s");
			stats_start = step_end;
			busy = busy_max = 0.0;
			steps = 0;
		}

		if (current_time == last_time) {
			// The clock advances in whole milliseconds (and not at all while paused).
			// Without a pause here the loop would keep a core at 100 % (battery and
			// heat on standalone headsets); new events are still handled within 1 ms.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		last_time = current_time;
	}
	log::log(MSG(info) << "Game simulation loop exited");
}


void GameSimulation::start() {
	std::unique_lock lock{this->mutex};

	this->init_event_handlers();

	// XR fork (save games): the file is read first, the game starts at its time
	std::optional<save::SaveData> load;
	time::time_t start_time = time::TIME_ZERO;
	if (not this->map_settings.load_file.empty()) {
		save::SaveData data;
		std::string error;
		if (save::read_save(this->map_settings.load_file, data, error)) {
			load = std::move(data);
			start_time = time::time_t::from_double(load->game_time);
			log::log(INFO << "Load: " << this->map_settings.load_file << " (" << load->title << ", " << load->created
			              << ", t=" << load->game_time << " s, " << load->entities.size() << " entities)");
			// the clock continues from the saved time (entities are created at that time)
			this->time_loop->get_clock()->set_time(start_time);
		}
		else {
			log::log(ERR << "Load: " << this->map_settings.load_file << ": " << error << " - starting a new game");
			this->production->notify("Laden fehlgeschlagen: " + error, prod::status_t::warn);
		}
	}

	// TODO: wait for presenter to initialize before starting?
	this->game = std::make_shared<gamestate::Game>(event_loop,
	                                               this->mod_manager,
	                                               this->entity_factory,
	                                               this->terrain_factory,
	                                               this->map_settings,
	                                               start_time);

	// XR fork: the presenter may attach its renderer before the game exists
	if (this->pending_render_factory) {
		this->game->attach_renderer(this->pending_render_factory);
		this->entity_factory->attach_renderer(this->pending_render_factory);
		this->terrain_factory->attach_renderer(this->pending_render_factory);
		this->pending_render_factory = nullptr;
	}

	// ai (XR fork): computer opponents train and build through the production
	this->game->connect_ai_production(this->production);

	// XR fork (save games): apply the saved state to the generated map
	if (load) {
		auto result = save::restore(*load, this->game->get_state(), this->event_loop, this->entity_factory,
		                            this->production, this->game->get_generated_entity_range(), start_time);
		if (result.ok) {
			this->production->notify("Spielstand geladen: " + (load->title.empty() ? save::map_text(load->map) : load->title),
			                         prod::status_t::good);
		}
		else {
			log::log(ERR << "Load: " << result.error << " - the map is played from the start");
			this->production->notify("Laden fehlgeschlagen: " + result.error, prod::status_t::warn);
		}
	}

	this->running = not this->stop_requested;

	log::log(MSG(info) << "Game simulation started");
}


void GameSimulation::post(Job job) {
	std::lock_guard<std::mutex> lock{this->jobs_mutex};
	this->jobs.push_back(std::move(job));
}

void GameSimulation::run_jobs(const time::time_t &time) {
	std::vector<Job> todo;
	{
		std::lock_guard<std::mutex> lock{this->jobs_mutex};
		if (this->jobs.empty()) {
			return;
		}
		todo.swap(this->jobs);
	}
	for (auto &job : todo) {
		try {
			job(this->game, time);
		}
		catch (std::exception &err) {
			log::log(ERR << "Simulation: job failed: " << err.what());
		}
	}
}

void GameSimulation::stop() {
	std::unique_lock lock{this->mutex};

	this->stop_requested = true;
	this->running = false;

	log::log(MSG(info) << "Game simulation stopped");
}


const util::Path &GameSimulation::get_root_dir() {
	std::shared_lock lock{this->mutex};

	return this->root_dir;
}

const std::shared_ptr<cvar::CVarManager> GameSimulation::get_cvar_manager() {
	std::shared_lock lock{this->mutex};

	return this->cvar_manager;
}

const std::shared_ptr<gamestate::Game> GameSimulation::get_game() {
	std::shared_lock lock{this->mutex};

	return this->game;
}

std::shared_ptr<gamestate::Game> GameSimulation::try_get_game() {
	std::shared_lock lock{this->mutex, std::try_to_lock};
	if (not lock.owns_lock()) {
		return nullptr;
	}

	return this->game;
}

const std::shared_ptr<openage::event::EventLoop> GameSimulation::get_event_loop() {
	std::shared_lock lock{this->mutex};

	return this->event_loop;
}

const std::shared_ptr<gamestate::event::Spawner> GameSimulation::get_spawner() {
	std::shared_lock lock{this->mutex};

	return this->spawner;
}

const std::shared_ptr<gamestate::event::Commander> GameSimulation::get_commander() {
	std::shared_lock lock{this->mutex};

	return this->commander;
}

const std::shared_ptr<gamestate::prod::Production> GameSimulation::get_production() {
	// set in the constructor, never reassigned
	return this->production;
}

void GameSimulation::attach_renderer(const std::shared_ptr<renderer::RenderFactory> &render_factory) {
	std::unique_lock lock{this->mutex};

	// XR fork: before start() there is no game yet; start() attaches the renderer
	// (the factories too, so that the map objects get exactly one render entity)
	if (not this->game) {
		this->pending_render_factory = render_factory;
		return;
	}

	this->game->attach_renderer(render_factory);
	this->entity_factory->attach_renderer(render_factory);
	this->terrain_factory->attach_renderer(render_factory);
}

void GameSimulation::set_modpacks(const std::vector<std::string> &modpacks) {
	std::unique_lock lock{this->mutex};

	std::vector<std::string> mods{"engine"};
	mods.insert(mods.end(), modpacks.begin(), modpacks.end());

	this->mod_manager->activate_modpacks(mods);

	// TODO: Prevent setting modpacks if a game is already running
}

void GameSimulation::set_map_settings(const MapSettings &settings) {
	std::unique_lock lock{this->mutex};

	this->map_settings = settings;
}

void GameSimulation::init_event_handlers() {
	auto drag_select_handler = std::make_shared<gamestate::event::DragSelectHandler>();
	auto spawn_handler = std::make_shared<gamestate::event::SpawnEntityHandler>(this->event_loop,
	                                                                            this->entity_factory);
	auto command_handler = std::make_shared<gamestate::event::SendCommandHandler>();
	auto manager_handler = std::make_shared<gamestate::event::ProcessCommandHandler>();
	auto wait_handler = std::make_shared<gamestate::event::WaitHandler>();
	this->event_loop->add_event_handler(drag_select_handler);
	this->event_loop->add_event_handler(spawn_handler);
	this->event_loop->add_event_handler(command_handler);
	this->event_loop->add_event_handler(manager_handler);
	this->event_loop->add_event_handler(wait_handler);
	// XR fork: combat (attack ticks, auto attack scan, removal of dead entities)
	combat::add_event_handlers(this->event_loop);
}

} // namespace openage::gamestate
