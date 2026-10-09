// Copyright 2019-2026 the openage authors. See copying.md for legal info.

#include "presenter.h"

#include "config.h"

#include "presenter/frame_stats.h"

#include <algorithm>
#include <chrono>
#include <eigen3/Eigen/Dense>
#include <iostream>
#include <string>
#include <vector>

#include "coord/pixel.h"
#include "engine/hud_info.h"
#include "event/event_loop.h"
#include "event/evententity.h"
#include "event/eventhandler.h"
#include "event/state.h"
#include "gamestate/event/send_command.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/component/internal/commands/types.h"
#include "gamestate/game.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"
#include "gamestate/map_settings.h"
#include "gamestate/simulation.h"
#include "input/controller/camera/binding_context.h"
#include "input/controller/camera/controller.h"
#include "input/controller/game/binding_context.h"
#include "gamestate/production.h"
#include "input/controller/game/controller.h"
#include "input/controller/hud/binding_context.h"
#include "input/controller/hud/controller.h"
#include "input/input_context.h"
#include "input/input_manager.h"
#include "log/log.h"
#include "renderer/camera/camera.h"
#if WITH_QT
	#include "renderer/gui/gui.h"
	#include "renderer/gui/integration/public/gui_application_with_logger.h"
#endif
#include "renderer/frame_sink.h"
#include "renderer/render_factory.h"
#include "renderer/render_pass.h"
#include "renderer/render_target.h"
#include "renderer/resources/assets/asset_manager.h"
#include "renderer/resources/png_io.h"
#include "renderer/resources/shader_source.h"
#include "renderer/resources/texture_data.h"
#include "renderer/resources/texture_info.h"
#include "renderer/stages/camera/manager.h"
#include "renderer/stages/hud/render_stage.h"
#include "renderer/stages/screen/render_stage.h"
#include "renderer/stages/skybox/render_stage.h"
#include "renderer/stages/terrain/render_stage.h"
#include "renderer/stages/ui/render_stage.h"
#include "renderer/stages/world/render_stage.h"
#include "renderer/texture.h"
#include "time/clock.h"
#include "time/time_loop.h"
#include "ui/aoe_ui_controller.h"
#include "ui/game_ui_controller.h"
#include "util/path.h"


namespace openage::presenter {

namespace {
/// zoom of one wheel notch (input/controller/camera/controller.cpp)
constexpr float zoom_step = 0.05f;
} // namespace

Presenter::Presenter(const util::Path &root_dir,
                     const std::shared_ptr<gamestate::GameSimulation> &simulation,
                     const std::shared_ptr<time::TimeLoop> &time_loop) :
	root_dir{root_dir},
	render_passes{},
	simulation{simulation},
	time_loop{time_loop} {}


void Presenter::stop() {
	*this->stop_requested = true;
}

std::shared_ptr<std::atomic<bool>> Presenter::get_stop_flag() const {
	return this->stop_requested;
}

std::shared_ptr<GameControllerSlot> Presenter::get_game_controller_slot() const {
	return this->controller_slot;
}

void Presenter::set_ui_hooks(std::function<engine::HudInfo()> query_hud,
                             std::function<void(const gamestate::MapSettings &)> restart,
                             std::function<void()> quit,
                             const gamestate::MapSettings &map) {
	this->ui_query_hud = std::move(query_hud);
	this->ui_restart = std::move(restart);
	this->ui_quit = std::move(quit);
	this->ui_map = std::make_shared<gamestate::MapSettings>(map);
}

double Presenter::now() const {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - this->start_time).count();
}

void Presenter::init_ui(const renderer::window_settings &window_settings) {
	if (not window_settings.ui) {
		return;
	}
	ui::UiHooks hooks;
	hooks.query_hud = this->ui_query_hud;
	hooks.production = this->simulation ? this->simulation->get_production() : nullptr;
	hooks.clock = this->time_loop ? this->time_loop->get_clock() : nullptr;
	hooks.send_command = [this](ui::ui_command_t type, int x, int y) {
		this->send_ui_command(type, x, y);
	};
	hooks.restart = this->ui_restart;
	hooks.quit = this->ui_quit;
	if (not hooks.quit) {
		hooks.quit = [this]() { this->stop(); };
	}
	gamestate::MapSettings map = this->ui_map ? *this->ui_map : gamestate::MapSettings{};
	if (window_settings.ui_style == "classic") {
		this->ui_controller = std::make_shared<ui::GameUiController>(map, std::move(hooks), window_settings.ui_demo);
	}
	else {
		ui::AoeHooks aoe;
		// camera view on the map for the minimap placeholder (map fractions)
		aoe.camera_view = [this](float &cx, float &cy, float &cw, float &ch) {
			if (not this->simulation or not this->camera) {
				return false;
			}
			auto game = this->simulation->get_game();
			if (not game or not game->get_state() or not game->get_state()->get_map()) {
				return false;
			}
			auto size = game->get_state()->get_map()->get_size();
			if (size[0] == 0 or size[1] == 0) {
				return false;
			}
			const auto &pos = this->camera->get_scene_pos();
			const float offset = pos[1] * 1.2247449f;  // see apply_map_view()
			cx = std::clamp((pos[0] - offset) / static_cast<float>(size[1]), 0.0f, 1.0f);
			cy = std::clamp(1.0f + (pos[2] - offset) / static_cast<float>(size[0]), 0.0f, 1.0f);
			const Eigen::Matrix4f proj = this->camera->get_projection_matrix();
			const auto &vp = this->camera->get_viewport_size();
			if (proj(0, 0) == 0.0f or vp[0] == 0) {
				return false;
			}
			// scene units per pixel (ortho), ground rows cover twice as much (pitch 30 degrees)
			const float units = 2.0f / (proj(0, 0) * static_cast<float>(vp[0]));
			cw = static_cast<float>(vp[0]) * units / (static_cast<float>(size[1]) * 1.4142136f);
			ch = static_cast<float>(vp[1]) * units * 2.0f / (static_cast<float>(size[0]) * 1.4142136f);
			return true;
		};
		// portrait of a multiple selection: select only this entity
		aoe.select = [this](const std::vector<uint64_t> &ids) {
			auto controller = this->controller_slot->get();
			if (not controller or not this->simulation) {
				return;
			}
			std::vector<gamestate::entity_id_t> sel(ids.begin(), ids.end());
			controller->set_selected(sel);
			if (auto production = this->simulation->get_production()) {
				production->set_selection(sel);
			}
		};
		this->ui_controller = std::make_shared<ui::AoeUiController>(map, std::move(hooks), std::move(aoe),
		                                                            window_settings.ui_demo, window_settings.ui_quest);
	}
	this->ui_controller->init();
	this->ui_renderer = std::make_shared<renderer::ui::UiRenderStage>(
		this->window,
		this->renderer,
		this->root_dir["assets"]["shaders"],
		this->ui_controller);
	this->render_passes.push_back(this->ui_renderer->get_render_pass());
}

void Presenter::send_ui_command(ui::ui_command_t type, int x, int y) {
	auto controller = this->controller_slot->get();
	if (not this->simulation or not controller or not this->camera or not this->time_loop) {
		return;
	}
	auto game = this->simulation->get_game();
	if (not game) {
		return;
	}
	// like the right click of input/controller/game/controller.cpp
	if (auto state = game->get_state()) {
		if (auto combat = state->get_combat()) {
			controller->prune_selected([&combat](gamestate::entity_id_t id) {
				return combat->was_removed(id);
			});
		}
	}
	coord::input mouse{x, y};
	auto mouse_pos = mouse.to_phys3(this->camera);
	using command_t = gamestate::component::command::command_t;
	command_t command = command_t::MOVE;
	bool plain = false;
	switch (type) {
	case ui::ui_command_t::MOVE_PLAIN:
		plain = true;
		break;
	case ui::ui_command_t::GATHER:
		command = command_t::GATHER;
		break;
	case ui::ui_command_t::ATTACK:
		command = command_t::ATTACK;
		break;
	default:
		break;
	}
	log::log(INFO << "Input: context menu command " << static_cast<int>(command) << (plain ? " (plain)" : "")
	              << " at pixel (" << x << ", " << y << ") for " << controller->get_selected().size() << " selected");
	Eigen::Matrix4f cam_matrix = this->camera->get_projection_matrix() * this->camera->get_view_matrix();
	event::EventHandler::param_map::map_t params{
		{"type", command},
		{"target", mouse_pos},
		{"entity_ids", controller->get_selected_copy()},
		{"controlled", controller->get_controlled()},
		{"camera_matrix", cam_matrix},
		{"pick_ndc", Eigen::Vector2f{mouse.to_viewport(this->camera).to_ndc_space(this->camera)}},
		{"plain", plain},
	};
	this->simulation->get_event_loop()->create_event(
		"game.send_command",
		this->simulation->get_commander(),
		game->get_state(),
		this->time_loop->get_clock()->get_time() + time::time_t::from_double(0.001),
		params);
}

void Presenter::run(const renderer::window_settings window_settings) {
	log::log(INFO << "Presenter: Launching subsystems...");

	this->init_graphics(window_settings);

	this->init_input();

	using clock_t = std::chrono::steady_clock;
	auto seconds = [](clock_t::time_point from, clock_t::time_point to) {
		return std::chrono::duration<double>(to - from).count();
	};
	const auto start = clock_t::now();
	size_t frames = 0;
	// XR fork: frame rate and frame times in the log every 5 s
	FrameStats stats;
	auto stats_start = start;

	while (not this->window->should_close() and not *this->stop_requested) {
		const auto t_frame = clock_t::now();
#if WITH_QT
		if (this->gui_app) {
			this->gui_app->process_events();
		}
#endif
		// TODO: pass button presses and events from GUI to controller

		const auto t_render = clock_t::now();
		this->render();

		this->renderer->check_error();
		const auto t_present = clock_t::now();

		frames += 1;
		if (not window_settings.capture_file.empty()) {
			std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
			if (elapsed.count() >= window_settings.capture_delay) {
				log::log(INFO << "Presenter: capturing frame " << frames
				              << " after " << elapsed.count() << " s");
				this->capture_frame(window_settings.capture_file);
				break;
			}
		}

		this->window->update();

		const auto t_end = clock_t::now();
		stats.add(seconds(t_frame, t_render), seconds(t_render, t_present), seconds(t_present, t_end));
		if (stats.due(seconds(stats_start, t_end))) {
			auto size = this->window->get_size();
			log::log(INFO << "Presenter: " << stats.report(seconds(stats_start, t_end)) << ", window "
			              << size[0] << "x" << size[1] << ", objects drawn "
			              << this->world_renderer->get_drawn_objects() << "/"
			              << this->world_renderer->get_object_count());
			stats.reset();
			stats_start = t_end;
		}
	}

	log::log(MSG(info) << "Presenter: Draw loop exited");

	if (this->simulation) {
		this->simulation->stop();
	}

	if (this->time_loop) {
		this->time_loop->stop();
	}

	this->window->close();
}

void Presenter::set_simulation(const std::shared_ptr<gamestate::GameSimulation> &simulation) {
	this->simulation = simulation;
	auto render_factory = std::make_shared<renderer::RenderFactory>(this->terrain_renderer, this->world_renderer);
	this->simulation->attach_renderer(render_factory);
}

void Presenter::set_time_loop(const std::shared_ptr<time::TimeLoop> &time_loop) {
	this->time_loop = time_loop;
}

std::shared_ptr<qtgui::GuiApplication> Presenter::init_window_system() {
#if WITH_QT
	return std::make_shared<renderer::gui::GuiApplicationWithLogger>();
#else
	// XR fork: without Qt, the window system belongs to the embedder
	return nullptr;
#endif
}

void Presenter::init_graphics(const renderer::window_settings &window_settings) {
	log::log(INFO << "Presenter: Initializing graphics subsystems...");

	// Start up rendering framework
	// (XR fork: a frame sink embedder owns the window system, no Qt GUI)
	if (not window_settings.sink) {
		this->gui_app = this->init_window_system();
	}
	this->sink = window_settings.sink;

	// Window and renderer
	this->window = renderer::Window::create("xr.ages", window_settings);
	this->renderer = this->window->make_renderer();

	// Asset mangement
	this->asset_manager = std::make_shared<renderer::resources::AssetManager>(
		this->renderer,
		this->root_dir / "assets" / "converted");
	auto missing_tex = this->root_dir / "assets" / "test" / "textures" / "test_missing.sprite";
	this->asset_manager->set_placeholder_animation(missing_tex);

	// Camera
	this->camera = std::make_shared<renderer::camera::Camera>(this->renderer, this->window->get_size());
	this->camera->look_at_coord(coord::scene3{10.0, 10.0, 0}); // Center camera on the map
	this->window->add_resize_callback([this](size_t w, size_t h, double /*scale*/) {
		this->camera->resize(w, h);
	});

	// Camera manager
	this->camera_manager = std::make_shared<renderer::camera::CameraManager>(this->camera);
	// TODO: Make boundaries dynamic based on map size.
	this->camera_manager->set_camera_boundaries(
		renderer::camera::CameraBoundaries{
			renderer::camera::X_BOUND_MIN,
			renderer::camera::X_BOUND_MAX,
			renderer::camera::Y_BOUND_MIN,
			renderer::camera::Y_BOUND_MAX,
			renderer::camera::Z_BOUND_MIN,
			renderer::camera::Z_BOUND_MAX});

	// Skybox
	this->skybox_renderer = std::make_shared<renderer::skybox::SkyboxRenderStage>(
		this->window,
		this->renderer,
		this->root_dir["assets"]["shaders"]);
	// XR fork: configurable, alpha 0 = transparent around the map
	const auto &bg = window_settings.background;
	this->skybox_renderer->set_color(bg[0], bg[1], bg[2], bg[3]);
	this->render_passes.push_back(this->skybox_renderer->get_render_pass());

	// Terrain
	this->terrain_renderer = std::make_shared<renderer::terrain::TerrainRenderStage>(
		this->window,
		this->renderer,
		this->camera,
		this->root_dir["assets"]["shaders"],
		this->asset_manager,
		this->time_loop->get_clock());
	this->render_passes.push_back(this->terrain_renderer->get_render_pass());

	// Units/buildings
	this->world_renderer = std::make_shared<renderer::world::WorldRenderStage>(
		this->window,
		this->renderer,
		this->camera,
		this->root_dir["assets"]["shaders"],
		this->asset_manager,
		this->time_loop->get_clock());
	this->render_passes.push_back(this->world_renderer->get_render_pass());

	// HUD
	this->hud_renderer = std::make_shared<renderer::hud::HudRenderStage>(
		this->window,
		this->renderer,
		this->camera,
		this->root_dir["assets"]["shaders"],
		this->asset_manager,
		this->time_loop->get_clock());
	this->render_passes.push_back(this->hud_renderer->get_render_pass());

	// game user interface in the image (XR fork), after the HUD pass
	this->init_ui(window_settings);

	// the GUI is optional (XR fork): builds without Qt have none, and a
	// missing QML setup only disables it
	if (this->gui_app) {
		try {
			this->init_gui();
		}
		catch (Error &err) {
			log::log(WARN << "Presenter: GUI disabled: " << err.what());
			this->gui = nullptr;
		}
	}
	else {
		log::log(INFO << "Presenter: no GUI (no window system application)");
	}
	this->init_final_render_pass();

	if (this->simulation) {
		auto render_factory = std::make_shared<renderer::RenderFactory>(this->terrain_renderer, this->world_renderer);
		this->simulation->attach_renderer(render_factory);
	}

	log::log(INFO << "Presenter: Graphics subsystems initialized");
}

void Presenter::init_gui() {
#if WITH_QT
	log::log(INFO << "Presenter: Initializing GUI with Qt backend");

	//// -- gui initialization
	// TODO: Do not use test GUI
	util::Path qml_root = this->root_dir / "assets" / "test" / "qml";
	log::log(INFO << "Presenter: Setting QML root to " << qml_root.resolve_native_path());
	if (not qml_root.is_dir()) {
		throw Error{ERR << "could not find qml root folder " << qml_root};
	}

	util::Path qml_assets = this->root_dir / "assets";
	log::log(INFO << "Presenter: Setting QML asset path to " << qml_assets.resolve_native_path());
	if (not qml_assets.is_dir()) {
		throw Error{ERR << "could not find asset root folder " << qml_assets};
	}

	util::Path qml_root_file = qml_root / "main.qml";
	log::log(INFO << "Presenter: Setting QML root file to " << qml_root_file.resolve_native_path());
	if (not qml_root_file.is_file()) {
		throw Error{ERR << "could not find main.qml file " << qml_root_file};
	}

	// TODO: in order to support qml-mods, the fslike and filelike
	//       library has to be integrated into qt. For now,
	//       figure out the absolute paths here and pass them in.

	this->gui = std::make_shared<renderer::gui::GUI>(
		this->gui_app, // Qt application wrapper
		this->window,  // window for the gui
		qml_root_file, // entry qml file, absolute path.
		qml_root,      // directory to watch for qml file changes
		qml_assets,    // qml data: Engine *, the data directory, ...
		this->renderer // openage renderer
	);

	auto gui_pass = this->gui->get_render_pass();
	this->render_passes.push_back(gui_pass);
#else
	throw Error{ERR << "no GUI in builds without Qt"};
#endif
}

void Presenter::init_input() {
	log::log(INFO << "Presenter: Initializing input subsystem...");

	this->input_manager = std::make_shared<input::InputManager>();

	// XR fork: the game user interface (window_settings::ui) sees every event
	// first; events on its menus and buttons do not reach the game
	auto ui_takes = [this](const renderer::WindowEvent &ev) {
		return this->ui_controller and this->ui_controller->on_event(ev, this->now());
	};
	this->window->add_key_callback([this, ui_takes](const renderer::WindowEvent &ev) {
		if (ui_takes(ev)) {
			return;
		}
		this->input_manager->process(input::Event{ev});
	});
	this->window->add_mouse_button_callback([this, ui_takes](const renderer::WindowEvent &ev) {
		if (ui_takes(ev)) {
			return;
		}
		// XR fork: every button release in the log (which clicks reach the engine)
		if (ev.type == input::event_type::MouseButtonRelease) {
			log::log(INFO << "Input: button " << ev.button << " released at pixel (" << ev.x << ", " << ev.y
			              << "), modifiers " << ev.modifiers);
		}
		this->input_manager->process(input::Event{ev});
	});
	this->window->add_mouse_move_callback([this, ui_takes](const renderer::WindowEvent &ev) {
		this->input_manager->set_mouse(static_cast<int>(ev.x), static_cast<int>(ev.y));
		if (ui_takes(ev)) {
			return;
		}
		this->input_manager->process(input::Event{ev});
	});
	this->window->add_mouse_wheel_callback([this, ui_takes](const renderer::WindowEvent &ev) {
		if (ui_takes(ev)) {
			return;
		}
		this->input_manager->process(input::Event{ev});
	});

	auto input_ctx = this->input_manager->get_global_context();
	input::setup_defaults(input_ctx);

	// setup simulation controls
	if (this->simulation) {
		log::log(INFO << "Loading game simulation controls");

		// TODO: Remove hardcoding
		auto game_controller = std::make_shared<input::game::Controller>(
			std::unordered_set<size_t>{0, 1, 2, 3}, 0);
		auto engine_context = std::make_shared<input::game::BindingContext>();
		// XR fork: a click selects the entity drawn under the cursor (object ids of the world pass)
		input::game::pick_func_t pick = [world = this->world_renderer](float ndc_x, float ndc_y)
			-> std::optional<gamestate::entity_id_t> {
			auto id = world->pick(ndc_x, ndc_y);
			if (not id) {
				return std::nullopt;
			}
			return static_cast<gamestate::entity_id_t>(*id);
		};
		input::game::setup_defaults(engine_context, this->time_loop, this->simulation, this->camera, pick);
		this->input_manager->set_game_controller(game_controller);
		input_ctx->set_game_bindings(engine_context);
		// XR fork: selection for HUDs in other threads
		this->controller_slot->set(game_controller);
	}

#if WITH_QT
	// attach GUI if it's initialized
	if (this->gui) {
		log::log(INFO << "Loading GUI controls");
		this->input_manager->set_gui(this->gui->get_input_handler());
	}
#endif

	// setup camera controls
	if (this->camera) {
		log::log(INFO << "Loading camera controls");
		auto camera_controller = std::make_shared<input::camera::Controller>();
		auto camera_context = std::make_shared<input::camera::BindingContext>();
		// a frame sink embedder scrolls at the edges through its camera channel
		input::camera::setup_defaults(camera_context, this->camera, this->camera_manager, not this->sink);
		this->input_manager->set_camera_controller(camera_controller);
		input_ctx->set_camera_bindings(camera_context);
	}

	// setup HUD controls
	if (this->hud_renderer) {
		log::log(INFO << "Loading HUD controls");
		auto hud_controller = std::make_shared<input::hud::Controller>();
		auto hud_context = std::make_shared<input::hud::BindingContext>();
		input::hud::setup_defaults(hud_context, this->hud_renderer);
		this->input_manager->set_hud_controller(hud_controller);
		input_ctx->set_hud_bindings(hud_context);
	}

	log::log(INFO << "Presenter: Input subsystem initialized");
}

void Presenter::init_final_render_pass() {
	// Final output to window
	this->screen_renderer = std::make_shared<renderer::screen::ScreenRenderStage>(
		this->window,
		this->renderer,
		this->root_dir["assets"]["shaders"]);
	std::vector<std::shared_ptr<renderer::RenderTarget>> targets{};
	for (auto pass : this->render_passes) {
		targets.push_back(pass->get_target());
	}
	this->screen_renderer->set_render_targets(targets);
	this->render_passes.push_back(this->screen_renderer->get_render_pass());

	// Update final render pass if the textures are reassigned on resize
	// TODO: This REQUIRES that all other render passes have already been
	//       resized
	this->window->add_resize_callback([this](size_t, size_t, double /*scale*/) {
		// Acquire the render targets for all previous passes
		std::vector<std::shared_ptr<renderer::RenderTarget>> targets{};
		for (size_t i = 0; i < this->render_passes.size() - 1; ++i) {
			targets.push_back(this->render_passes[i]->get_target());
		}
		this->screen_renderer->set_render_targets(targets);
	});
}

void Presenter::capture_frame(const std::string &file) {
	// Render the final pass again, into a texture instead of the window.
	// The default framebuffer of a hidden window has no defined content,
	// and reading the texture back also covers the texture readback path.
	auto size = this->window->get_size() * this->window->get_scale();
	auto texture = this->renderer->add_texture(
		renderer::resources::Texture2dInfo(size[0], size[1], renderer::resources::pixel_format::rgba8));
	auto target = this->renderer->create_texture_target({texture});

	auto pass = this->screen_renderer->get_render_pass();
	auto display = pass->get_target();
	pass->set_target(target);
	this->renderer->render(pass);
	pass->set_target(display);
	this->renderer->check_error();

	// OpenGL rows start at the bottom
	auto image = texture->into_data().flip_y();
	// stored directly: Texture2dData::store() needs a writable util::Path,
	// which a plain fslike::Directory does not provide
	renderer::resources::store_png_rgba8(file, image.get_data(), size[0], size[1],
	                                     image.get_info().get_row_size());
	log::log(INFO << "Presenter: stored frame " << size[0] << "x" << size[1] << " to " << file);
}

void Presenter::apply_sink_camera() {
	float dx = 0.0f;
	float dy = 0.0f;
	float zoom = 0.0f;
	const bool moving = this->sink and this->sink->poll_camera(dx, dy, zoom)
	                    and (dx != 0.0f or dy != 0.0f or zoom != 0.0f);
	if (moving) {
		if (not this->sink_camera_active) {
			const auto &pos = this->camera->get_scene_pos();
			log::log(INFO << "Presenter: camera channel active at scene (" << pos[0] << ", "
			              << pos[1] << ", " << pos[2] << "), zoom " << this->camera->get_zoom());
		}
		this->camera->move_screen(dx, dy, this->camera_manager->get_camera_boundaries());
		if (zoom > 0.0f) {
			this->camera_manager->zoom_frame(renderer::camera::ZoomDirection::IN, zoom * zoom_step);
		}
		else if (zoom < 0.0f) {
			this->camera_manager->zoom_frame(renderer::camera::ZoomDirection::OUT, -zoom * zoom_step);
		}
		this->sink_camera_active = true;
		this->sink_camera_dx += dx;
		this->sink_camera_dy += dy;
		this->sink_camera_zoom += zoom;
		this->sink_camera_frames += 1;
	}
	else if (this->sink_camera_active) {
		// summary once the channel is idle again (no log line per frame)
		const auto &pos = this->camera->get_scene_pos();
		log::log(INFO << "Presenter: camera channel idle after " << this->sink_camera_frames
		              << " frames: moved " << this->sink_camera_dx << ", " << this->sink_camera_dy
		              << " px, zoom " << this->sink_camera_zoom << " steps -> scene (" << pos[0]
		              << ", " << pos[1] << ", " << pos[2] << "), zoom " << this->camera->get_zoom());
		this->sink_camera_active = false;
		this->sink_camera_dx = this->sink_camera_dy = this->sink_camera_zoom = 0.0f;
		this->sink_camera_frames = 0;
	}
}

void Presenter::apply_sink_background() {
	float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	if (this->sink and this->sink->poll_background(rgba)) {
		this->skybox_renderer->set_color(rgba[0], rgba[1], rgba[2], rgba[3]);
		log::log(INFO << "Presenter: background (" << rgba[0] << ", " << rgba[1] << ", " << rgba[2]
		              << "), alpha " << rgba[3]);
	}
}

void Presenter::update_selection_markers() {
	auto controller = this->controller_slot->get();
	if (not controller) {
		return;
	}
	auto selected = controller->get_selected_copy();
	std::vector<uint32_t> ids(selected.begin(), selected.end());
	auto controlled = controller->get_controlled();

	std::vector<renderer::hud::SelectionMarker> markers;
	for (const auto &box : this->world_renderer->get_screen_boxes(ids)) {
		markers.push_back(renderer::hud::SelectionMarker{box.ndc, box.player == controlled});
	}
	this->hud_renderer->set_selection_markers(std::move(markers));
}

void Presenter::apply_map_view() {
	if (this->map_view_done or not this->simulation) {
		return;
	}
	auto game = this->simulation->get_game();
	if (not game) {
		return;
	}
	this->map_view_done = true;

	const auto &view = game->get_start_view();
	if (not view) {
		return;
	}
	auto map_size = game->get_state()->get_map()->get_size();

	// camera above the ground at the given height, looking down at the target
	// (look_at keeps the height of the camera and sets x and z)
	this->camera->move_to(Eigen::Vector3f{0.0f, view->height, 0.0f});
	this->camera->look_at_coord(coord::scene3{view->ne, view->se, 0});
	this->camera->set_zoom(view->zoom);

	// limits: camera position = looked-at point + offset of the fixed view direction
	// (renderer::camera::Camera::calc_look_at: height * sqrt(3) / sqrt(2) in x and z)
	const float offset = view->height * 1.2247449f;
	const float max_height = std::max(renderer::camera::Y_BOUND_MAX, view->height);
	// XR fork (AoE layout): the bottom bar lies on the image, so the camera may go
	// further towards the screen bottom by its height (scene units of that many
	// pixels at twice the start zoom, along the ground direction "screen down")
	float bottom = 0.0f;
	if (this->ui_controller and this->ui_controller->camera_bottom_px() > 0) {
		const Eigen::Matrix4f proj = this->camera->get_projection_matrix();
		const auto &vp = this->camera->get_viewport_size();
		if (proj(0, 0) != 0.0f and vp[0] > 0) {
			const float units = 2.0f / (proj(0, 0) * static_cast<float>(vp[0]));
			bottom = static_cast<float>(this->ui_controller->camera_bottom_px()) * units * 2.0f * 2.0f * 0.7071068f;
		}
	}
	this->camera_manager->set_camera_boundaries(
		renderer::camera::CameraBoundaries{
			offset,
			offset + static_cast<float>(map_size[1]) + bottom,
			renderer::camera::Y_BOUND_MIN,
			max_height,
			offset - static_cast<float>(map_size[0]),
			offset + bottom});
	if (bottom > 0.0f) {
		log::log(INFO << "Presenter: camera limit extended by " << bottom << " scene units for the bottom bar ("
		              << this->ui_controller->camera_bottom_px() << " px)");
	}

	log::log(INFO << "Presenter: map view at tile (" << view->ne << ", " << view->se << "), zoom "
	              << view->zoom << ", camera height " << view->height << ", map " << map_size[0] << "x" << map_size[1]);
}

void Presenter::render() {
	// initial camera for the generated map first, then camera input of an embedder
	this->apply_map_view();
	this->apply_sink_camera();
	this->apply_sink_background();

	// TODO: Pass current time to update() instead of fetching it in renderer
	this->camera_manager->update();
	this->terrain_renderer->update();
	this->world_renderer->update();
	this->update_selection_markers();
	this->hud_renderer->update();
	if (this->ui_renderer) {
		this->ui_renderer->update(this->now());
		// XR fork: haptics / focus mode of the embedder
		if (this->sink and this->ui_controller) {
			const auto f = this->ui_controller->feedback();
			this->sink->ui_feedback(f.hover_id, f.clicked_id, f.click_seq, f.armed, f.focus_mode, f.menu_open);
		}
	}
#if WITH_QT
	if (this->gui) {
		this->gui->render();
	}
#endif

	for (auto &pass : this->render_passes) {
		this->renderer->render(pass);
	}
}

} // namespace openage::presenter
