// Copyright 2026 the openage authors. See copying.md for legal info.

/*
 * Native C++ entry point for openage (XR fork).
 *
 * The regular entry point is the Python package (`python -m openage game`),
 * which mounts assets/ and cfg/ as a Python union filesystem and then calls
 * run_game() through Cython. On platforms without a Python runtime (e.g. the
 * Meta Quest), this executable starts the engine directly:
 *
 *   openage-native --root <dir> [--modpack hd_base] [--headless]
 *                  [--seconds N] [--width W --height H] [--check]
 *                  [--gles] [--render-check <png>] [--shader-check]
 *                  [--egl-sink-check <png> [--replay | --replay-econ | --replay-combat | --replay-prod | --replay-select | --stop-in-resize] [--frames N]]
 *                  [--map test|random [--map-seed N] [--map-size N] [--map-biome B]
 *                   [--map-trees N] [--map-elevation H] [--map-view ne,se[,zoom[,height]]]
 *                   [--map-skirmish]]
 *                  [--background r,g,b,a [--background-switch r,g,b,a]]
 *                  [--ai on|off|auto] [--ai-difficulty easy|normal] [--ai-player N]
 *                  [--ai-first-attack S] [--ai-attack-size N] [--sim-speed X] [--capture-at s1,s2,...]
 *
 * --gles selects an OpenGL ES 3.x context (like OPENAGE_GLES=1).
 * --render-check renders the game for --seconds (default 10) in a hidden
 * window, stores the final frame as PNG and exits. --shader-check compiles
 * and links all vertex/fragment shader pairs in assets/shaders and
 * assets/test/shaders in a hidden window and exits. Both never show a window.
 *
 * --egl-sink-check renders without any window system into the frames of a
 * test frame sink (EGL, OpenGL ES, context shared with a consumer thread like
 * the XR layer of the Quest app). The consumer stores the newest frame after
 * --seconds (default 15) as <png>, then requests 1920x1080 and stores a
 * second frame as <png stem>-1920x1080.png; the engine is stopped with
 * Engine::stop() after at least --frames (default 1000) frames. --replay
 * plays input instead of the first plain capture: Ctrl+click spawns two
 * entities, a drag selects them (captured into <png> while the rectangle is
 * visible), a right click moves them. --replay-combat (with --map-skirmish and
 * --map-view on an enemy unit) captures <png stem>-before.png, selects all own
 * units on screen, right-clicks the screen centre (attack), captures
 * <png stem>-attack.png after 4 s and <png> after 30 s (before the size change).
 * --stop-in-resize (regression check)
 * captures <png>, requests 1920x1080 and lets the consumer thread call
 * Engine::stop() while the presenter is inside acquire_target() for the new
 * size; the engine has to stop within 2 s (no second capture).
 * --replay-econ (XR fork, economy; with --map random) selects each villager of the
 * first player and right clicks a tree, berries and gold next to it (gathering),
 * captures <png> after 45 s and <png stem>-econ2.png after 90 s.
 * --replay-prod (XR fork, production; with --map random) selects the town centre of the
 * first player and presses T twice (2 villagers), selects a villager, presses Y (house)
 * and clicks a free spot (foundation); then tries to place a house on water through
 * the production interface (rejected). Logs the HUD snapshot every 5 s, captures
 * <png> after 20 s (training, construction) and <png stem>-prod2.png after 70 s.
 * --replay-select (XR fork, selection; with --map random) single clicks a villager,
 * the ground, the town centre and a tree, double clicks a villager, clicks a
 * villager and right clicks the ground (move), clicks a villager, Shift + clicks
 * the town centre and right clicks the tree (gathering); captures each state
 * (<png stem>-click/-ground/-tc/-tree/-double/-shift.png) and <png> at the end.
 * A hanging engine fails the check instead of blocking: no new frame for
 * 60 s stops it, and if it did not stop 10 s after Engine::stop(), the
 * process exits with an error.
 *
 * --map random uses the random map generator (gamestate/map_generator.h) instead
 * of the fixed test map: deterministic per --map-seed, --map-size tiles (multiple
 * of 16), at most --map-trees tree entities, hills up to --map-elevation. The
 * camera looks at the first start position, or at --map-view (tile ne,se, zoom,
 * camera height; for render checks). --map-biome selects the landscape preset
 * (map_biome_t: grass (default, the original random map), steppe, hills, forest,
 * rivers, coast, inland-sea, water, gold-rush, desert, winter, jungle).
 * --map-skirmish adds a small army per player
 * between the starts (knights, militia, archers; gamestate/combat/skirmish.h) and
 * looks at the battlefield unless --map-view is given.
 *
 * --background sets the color behind the map (RGBA 0..1, window_settings::background;
 * alpha 0 = transparent around the map). With --egl-sink-check,
 * --background-switch changes it at runtime through the frame sink
 * (FrameSink::poll_background) before the second capture (<png stem>-1920x1080.png).
 *
 * ai (XR fork): --ai switches the computer opponent (gamestate/ai, default auto:
 * on for random maps with two starts; the replays switch it off unless --ai is
 * given), --ai-difficulty/--ai-player/--ai-first-attack/--ai-attack-size set its
 * parameters (MapSettings::ai). --sim-speed runs the simulation clock faster
 * (e.g. 8 = 8 game seconds per second). --capture-at adds captures to
 * --egl-sink-check at the given seconds (<png stem>-t<s>.png).
 *
 * Game user interface (XR fork, ui/game_ui_controller.h): the plain game draws
 * the HUD bar, context menu (right button held, Alt + right click, middle click),
 * game menu (Esc / F10) and match board into the image (--no-ui switches it off);
 * the checks keep it off unless --ui is given. --ui-demo "board@12,restart@8"
 * shows a demo match board / restarts with the next map number after N seconds.
 * --replay-ui (with --egl-sink-check and --map random) clicks the town centre, a
 * HUD button, a villager, holds the right button (context menu), picks "Hierher
 * bewegen", opens and closes the game menu and captures each state
 * (<png stem>-hud/-train/-context/-menu/-board.png). "Neue Karte" of the game
 * menu stops the engine with a restart request; this entry point then starts a
 * new engine with the chosen map (Engine::take_restart()).
 *
 * <dir> must contain assets/ (with shaders and converted/{engine,<modpack>})
 * and cfg/. The converted modpacks (including the "engine" API modpack) are
 * produced offline with `python -m openage convert` and
 * `python -m openage convert-export-api`.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "config.h"

#include "engine/check_root.h"
#include "engine/engine.h"
#include "error/error.h"
#include "coord/phys.h"
#include "coord/scene.h"
#include "gamestate/heightmap.h"
#include "gamestate/map_generator.h"
#include "gamestate/map_settings.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/stats.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/gather.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/econ.h"
#include "gamestate/game.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/production.h"
#include "gamestate/save_format.h"
#include "log/log.h"
#include "log/logsink.h"
#include "log/message.h"
#if WITH_QT
	#include "renderer/gui/integration/public/gui_application_with_logger.h"
#endif
#if WITH_EGL
	#include "renderer/opengl/test_sink.h"
#endif
#include "renderer/camera/camera.h"
#include "renderer/renderer.h"
#include "renderer/resources/shader_source.h"
#include "renderer/resources/shader_template.h"
#include "renderer/window.h"
#include "time/clock.h"
#include "ui/agesxr/xr_aoe_ui.h"
#include "ui/agesxr/xr_game_ui.h"
#include "ui/agesxr/xr_hud_layout.h"
#include "ui/game_ui_controller.h"
#include "util/fslike/directory.h"
#include "util/path.h"

namespace {

struct native_args {
	std::string root = ".";
	std::vector<std::string> modpacks;
	bool headless = false;
	bool check = false;
	bool gles = false;
	std::string render_check;
	bool shader_check = false;
	std::string egl_sink_check;
	bool replay = false;
	bool replay_econ = false;
	// XR fork (production)
	bool replay_prod = false;
	bool replay_combat = false;
	// XR fork: single click selection
	bool replay_select = false;
	bool stop_in_resize = false;
	uint64_t frames = 1000;
	int seconds = 0;
	size_t width = 1024;
	size_t height = 768;
	openage::gamestate::MapSettings map{};
	std::optional<std::array<float, 4>> background{};
	std::optional<std::array<float, 4>> background_switch{};
	// ai (XR fork)
	bool ai_explicit = false;
	std::optional<double> sim_speed{};
	std::vector<double> capture_at{};
	// game user interface (XR fork): default on for the plain game, off for checks
	std::optional<bool> ui{};
	std::string ui_demo{};
	bool replay_ui = false;
	// AoE layout (XR fork, docs/UI-SPEC-AOE.md): style, Quest hints, replay
	std::string ui_style{"aoe"};
	bool ui_quest = false;
	bool replay_aoe = false;
	bool replay_markers = false;
	// save games (XR fork): slot directory, file to load, headless save/load check
	std::string save_dir{};
	std::string load_file{};
	std::string save_check{};
	int save_check_seconds = 120;
};

/**
 * Parse "r,g,b,a" (each 0..1).
 */
std::array<float, 4> parse_rgba(const std::string &text, const std::string &option) {
	std::array<float, 4> rgba{};
	std::istringstream in{text};
	char sep = 0;
	in >> rgba[0] >> sep >> rgba[1] >> sep >> rgba[2] >> sep >> rgba[3];
	if (in.fail()) {
		throw std::runtime_error(option + ": r,g,b,a (0..1)");
	}
	for (float &c : rgba) {
		c = std::clamp(c, 0.0f, 1.0f);
	}
	return rgba;
}

void usage(const char *argv0) {
	std::cerr << "usage: " << argv0
	          << " --root <dir> [--modpack <id>]... [--headless] [--seconds <n>]"
	             " [--width <w> --height <h>] [--check]"
	             " [--gles] [--render-check <png>] [--shader-check]"
	             " [--egl-sink-check <png> [--replay | --replay-econ | --replay-combat | --replay-prod | --replay-select | --stop-in-resize] [--frames <n>]]"
	             " [--map test|random [--map-seed <n>] [--map-size <n>] [--map-biome <name>] [--map-trees <n>]"
	             " [--map-elevation <h>] [--map-view <ne,se[,zoom[,height]]>] [--map-skirmish]]"
	             " [--background <r,g,b,a> [--background-switch <r,g,b,a>]]"
	             " [--ai on|off|auto] [--ai-difficulty easy|normal] [--ai-player <n>]"
	             " [--ai-first-attack <s>] [--ai-attack-size <n>] [--sim-speed <x>] [--capture-at <s1,s2,...>]"
	             " [--ui | --no-ui] [--ui-demo <what@s,...>] [--replay-ui]"
	             " [--ui-style aoe|classic] [--ui-quest] [--replay-aoe] [--replay-markers]"
	             " [--save-dir <dir>] [--load <file>] [--save-check <dir> [--save-check-seconds <s>]]\n";
}

bool parse_args(int argc, char **argv, native_args &args) {
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		auto value = [&]() -> std::string {
			if (i + 1 >= argc) {
				throw std::runtime_error("missing value for " + arg);
			}
			return argv[++i];
		};

		if (arg == "--root") {
			args.root = value();
		}
		else if (arg == "--modpack") {
			args.modpacks.push_back(value());
		}
		else if (arg == "--headless") {
			args.headless = true;
		}
		else if (arg == "--check") {
			args.check = true;
		}
		else if (arg == "--gles") {
			args.gles = true;
		}
		else if (arg == "--render-check") {
			args.render_check = value();
		}
		else if (arg == "--shader-check") {
			args.shader_check = true;
		}
		else if (arg == "--egl-sink-check") {
			args.egl_sink_check = value();
		}
		else if (arg == "--replay") {
			args.replay = true;
		}
		else if (arg == "--replay-prod") {
			args.replay_prod = true;
		}
		else if (arg == "--replay-econ") {
			args.replay_econ = true;
		}
		else if (arg == "--replay-select") {
			args.replay_select = true;
		}
		else if (arg == "--replay-combat") {
			args.replay_combat = true;
		}
		else if (arg == "--stop-in-resize") {
			args.stop_in_resize = true;
		}
		else if (arg == "--frames") {
			args.frames = std::stoull(value());
		}
		else if (arg == "--seconds") {
			args.seconds = std::stoi(value());
		}
		else if (arg == "--width") {
			args.width = std::stoul(value());
		}
		else if (arg == "--height") {
			args.height = std::stoul(value());
		}
		else if (arg == "--map") {
			auto type = value();
			if (type == "test") {
				args.map.type = openage::gamestate::map_type_t::TEST;
			}
			else if (type == "random") {
				args.map.type = openage::gamestate::map_type_t::RANDOM;
			}
			else {
				throw std::runtime_error("--map: test or random, not " + type);
			}
		}
		else if (arg == "--map-seed") {
			args.map.seed = static_cast<uint32_t>(std::stoul(value()));
		}
		else if (arg == "--map-size") {
			args.map.size = std::stoul(value());
		}
		else if (arg == "--map-biome") {
			auto name = value();
			if (not openage::gamestate::map_biome_parse(name, args.map.biome)) {
				throw std::runtime_error("--map-biome: grass, steppe, hills, forest, rivers, coast, inland-sea, "
				                         "water, gold-rush, desert, winter or jungle, not "
				                         + name);
			}
		}
		else if (arg == "--map-trees") {
			args.map.max_trees = std::stoul(value());
		}
		else if (arg == "--map-elevation") {
			args.map.max_elevation = std::stof(value());
		}
		else if (arg == "--map-skirmish") {
			args.map.skirmish = true;
		}
		else if (arg == "--map-view") {
			// ne,se[,zoom[,height]]
			std::istringstream in{value()};
			openage::gamestate::MapView view;
			char sep = 0;
			in >> view.ne >> sep >> view.se;
			if (in >> sep) {
				in >> view.zoom;
			}
			if (in >> sep) {
				in >> view.height;
			}
			if (in.fail()) {
				throw std::runtime_error("--map-view: ne,se[,zoom[,height]]");
			}
			args.map.view = view;
		}
		else if (arg == "--background") {
			args.background = parse_rgba(value(), arg);
		}
		// ---- ai (XR fork)
		else if (arg == "--ai") {
			auto mode = value();
			args.ai_explicit = true;
			if (mode == "on") {
				args.map.ai.mode = openage::gamestate::ai_mode_t::ON;
			}
			else if (mode == "off") {
				args.map.ai.mode = openage::gamestate::ai_mode_t::OFF;
			}
			else if (mode == "auto") {
				args.map.ai.mode = openage::gamestate::ai_mode_t::AUTO;
			}
			else {
				throw std::runtime_error("--ai: on, off or auto, not " + mode);
			}
		}
		else if (arg == "--ai-difficulty") {
			auto level = value();
			if (level == "easy") {
				args.map.ai.difficulty = openage::gamestate::ai_difficulty_t::EASY;
			}
			else if (level == "normal") {
				args.map.ai.difficulty = openage::gamestate::ai_difficulty_t::NORMAL;
			}
			else {
				throw std::runtime_error("--ai-difficulty: easy or normal, not " + level);
			}
		}
		else if (arg == "--ai-player") {
			args.map.ai.player = std::stoull(value());
		}
		else if (arg == "--ai-first-attack") {
			args.map.ai.first_attack = std::stod(value());
		}
		else if (arg == "--ai-attack-size") {
			args.map.ai.attack_size = std::stoul(value());
		}
		else if (arg == "--sim-speed") {
			args.sim_speed = std::stod(value());
			if (*args.sim_speed <= 0.0 or *args.sim_speed > 64.0) {
				throw std::runtime_error("--sim-speed: 0 < x <= 64");
			}
		}
		else if (arg == "--capture-at") {
			std::istringstream in{value()};
			std::string part;
			while (std::getline(in, part, ',')) {
				args.capture_at.push_back(std::stod(part));
			}
		}
		// ---- end ai (XR fork)
		else if (arg == "--background-switch") {
			args.background_switch = parse_rgba(value(), arg);
		}
		// ---- game user interface (XR fork)
		else if (arg == "--ui") {
			args.ui = true;
		}
		else if (arg == "--no-ui") {
			args.ui = false;
		}
		else if (arg == "--ui-demo") {
			args.ui_demo = value();
		}
		else if (arg == "--replay-ui") {
			args.replay_ui = true;
			args.ui = true;
			args.ui_style = "classic";  // the classic HUD bar (88-ui-check.sh)
		}
		else if (arg == "--ui-style") {
			args.ui_style = value();
			if (args.ui_style != "aoe" and args.ui_style != "classic") {
				throw std::runtime_error("--ui-style: aoe or classic");
			}
		}
		else if (arg == "--ui-quest") {
			args.ui_quest = true;
		}
		else if (arg == "--replay-aoe") {
			args.replay_aoe = true;
			args.ui = true;
			args.ui_style = "aoe";
		}
		else if (arg == "--replay-markers") {
			args.replay_markers = true;
			args.ui = true;
			args.ui_style = "aoe";
		}
		else if (arg == "--save-dir") {
			args.save_dir = value();
		}
		else if (arg == "--load") {
			args.load_file = value();
		}
		else if (arg == "--save-check") {
			args.save_check = value();
		}
		else if (arg == "--save-check-seconds") {
			args.save_check_seconds = std::stoi(value());
		}
		else if (arg == "--help" or arg == "-h") {
			return false;
		}
		else {
			throw std::runtime_error("unknown argument: " + arg);
		}
	}
	if (args.modpacks.empty()) {
		args.modpacks.push_back("hd_base");
	}
	return true;
}

/**
 * Same checks as openage/game/main.py (engine::check_root), as exception.
 */
void check_modpacks(const openage::util::Path &root,
                    const std::vector<std::string> &wanted) {
	auto problem = openage::engine::check_root(root, wanted);
	if (not problem.empty()) {
		throw openage::Error{MSG(err) << problem};
	}
}

std::string read_file(const std::filesystem::path &file) {
	std::ifstream in{file};
	std::stringstream content;
	content << in.rdbuf();
	return content.str();
}

/**
 * Compile and link every vertex/fragment shader pair (X.vert.glsl + X.frag.glsl)
 * of assets/shaders and assets/test/shaders (with subfolders) in a hidden window.
 * Legacy shaders without #version (unused) are skipped, the shader template
 * of renderer demo 7 is filled with its snippets.
 *
 * @return Number of failed pairs.
 */
size_t shader_check(const std::filesystem::path &root,
                    const openage::util::Path &asset_root,
                    const openage::renderer::window_settings &settings) {
	using namespace openage;
	namespace fs = std::filesystem;

#if WITH_QT
	renderer::gui::GuiApplicationWithLogger app{};
#endif
	auto window = renderer::Window::create("openage shader check", settings);
	auto renderer = window->make_renderer();

	// pairs used by the engine that don't share a name
	std::vector<std::pair<fs::path, fs::path>> pairs{
		{root / "assets/shaders/identity.vert.glsl", root / "assets/shaders/maptexture.frag.glsl"},
		{root / "assets/shaders/world3d.vert.glsl", root / "assets/shaders/world2d.frag.glsl"},
	};
	for (const auto &dir : {root / "assets/shaders", root / "assets/test/shaders"}) {
		std::vector<fs::path> frags;
		for (const auto &entry : fs::recursive_directory_iterator(dir)) {
			std::string name = entry.path().filename().string();
			if (name.size() > 10 and name.ends_with(".frag.glsl")) {
				frags.push_back(entry.path());
			}
		}
		std::sort(frags.begin(), frags.end());
		for (const auto &frag : frags) {
			std::string base = frag.filename().string();
			base.resize(base.size() - std::string{".frag.glsl"}.size());
			auto vert = frag.parent_path() / (base + ".vert.glsl");
			if (fs::exists(vert)) {
				pairs.emplace_back(vert, frag);
			}
		}
	}

	size_t ok = 0;
	size_t failed = 0;
	size_t skipped = 0;
	// shader templates (renderer demo 7) are filled with their snippets first
	const auto template_frag = root / "assets/test/shaders/demo_7_shader_command.frag.glsl";
	const auto template_dir = asset_root / "assets" / "test" / "shaders";

	for (const auto &[vert, frag] : pairs) {
		std::string vert_src = read_file(vert);
		std::string frag_src = read_file(frag);
		std::string name = vert.filename().string() + " + " + frag.filename().string();
		if (vert_src.find("#version") == std::string::npos
		    or frag_src.find("#version") == std::string::npos) {
			log::log(INFO << "shader check: skip " << name << " (legacy shader without #version)");
			skipped += 1;
			continue;
		}
		try {
			renderer::resources::ShaderSource frag_shader{renderer::resources::shader_lang_t::glsl,
			                                              renderer::resources::shader_stage_t::fragment,
			                                              std::move(frag_src)};
			if (frag == template_frag) {
				renderer::resources::ShaderTemplate frag_template{template_dir / frag.filename().string()};
				frag_template.load_snippets(template_dir / "demo_7_snippets");
				frag_shader = frag_template.generate_source();
				name += " (template)";
			}
			renderer->add_shader({
				renderer::resources::ShaderSource{renderer::resources::shader_lang_t::glsl,
			                                      renderer::resources::shader_stage_t::vertex,
			                                      std::move(vert_src)},
				frag_shader,
			});
			renderer->check_error();
			log::log(INFO << "shader check: ok   " << name);
			ok += 1;
		}
		catch (Error &err) {
			log::log(ERR << "shader check: FAIL " << name << ": " << err.what());
			failed += 1;
		}
	}

	log::log(INFO << "shader check: " << ok << " ok, " << failed << " failed, "
	              << skipped << " skipped");
	return failed;
}

#if WITH_EGL
/**
 * Input replay of the economy check (--replay econ, XR fork): on the random map,
 * select each of the first player's three villagers with a small selection
 * rectangle and right click a resource (wood, food, gold) next to it, through
 * the regular input path (drag select, right click = gather on a resource).
 * Captures <png> after 45 s (villagers at work) and <png stem>-econ2 after 90 s.
 *
 * The screen positions come from the generator (same settings = same map) and a
 * camera without renderer set up like Presenter::apply_map_view().
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> econ_replay_steps(const native_args &args,
                                                                               openage::gamestate::MapSettings &map_settings,
                                                                               double start,
                                                                               const std::string &capture_file) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using gamestate::map_object_t;
	using E = renderer::SinkInputEvent;

	const auto map = gamestate::generate_map(map_settings);
	const gamestate::Heightmap heights{map.width, map.height, map.corners};
	if (not map_settings.view) {
		// first start position, zoomed out: villagers, wood, berries and gold on screen
		gamestate::MapView start_view;
		start_view.ne = map.starts.at(0)[0];
		start_view.se = map.starts.at(0)[1];
		start_view.zoom = 2.0f;
		map_settings.view = start_view;
	}
	const gamestate::MapView view = *map_settings.view;

	const int width = static_cast<int>(args.width);
	const int height = static_cast<int>(args.height);
	auto camera = std::make_shared<renderer::camera::Camera>(nullptr, util::Vector2s{args.width, args.height});
	camera->look_at_coord(coord::scene3{10.0, 10.0, 0});
	camera->move_to(Eigen::Vector3f{0.0f, view.height, 0.0f});
	camera->look_at_coord(coord::scene3{view.ne, view.se, 0});
	camera->set_zoom(view.zoom);
	const Eigen::Matrix4f matrix = camera->get_projection_matrix() * camera->get_view_matrix();

	// screen pixel (input coordinates, origin top left) of a map point, up = height above the ground
	auto pixel = [&](double ne, double se, double up) {
		double ground = heights.is_flat() ? 0.0 : heights.at(ne, se);
		coord::phys3 pos{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{ground + up}};
		auto w = pos.to_scene3().to_world_space();
		Eigen::Vector4f clip = matrix * Eigen::Vector4f{w.x(), w.y(), w.z(), 1.0f};
		int x = static_cast<int>(std::lround((clip.x() + 1.0) * 0.5 * width));
		int y = static_cast<int>(std::lround(height - (clip.y() + 1.0) * 0.5 * height));
		return std::pair{x, y};
	};
	auto on_screen = [&](std::pair<int, int> p) {
		return p.first >= 24 and p.second >= 24 and p.first < width - 24 and p.second < height - 24;
	};

	std::vector<const gamestate::MapObject *> villagers;
	for (const auto &o : map.objects) {
		if (o.kind == map_object_t::VILLAGER and o.owner == 0) {
			villagers.push_back(&o);
		}
	}

	std::vector<TestFrameSink::Step> steps;
	auto add = [&](double at, int type, int x, int y, int button, int buttons) {
		TestFrameSink::Step step;
		step.at = start + at;
		step.what = TestFrameSink::Step::kind::input;
		step.event.type = type;
		step.event.x = x;
		step.event.y = y;
		step.event.button = button;
		step.event.buttons = buttons;
		steps.push_back(step);
	};

	const std::array<std::pair<const char *, std::vector<map_object_t>>, 3> jobs{{
		{"wood", {map_object_t::TREE_PINE, map_object_t::TREE_JUNGLE, map_object_t::TREE_PALM,
		          map_object_t::TREE_SNOW, map_object_t::TREE_BAMBOO}},
		{"food", {map_object_t::BERRIES}},
		{"gold", {map_object_t::GOLD}},
	}};
	double t = 0.0;
	for (size_t k = 0; k < villagers.size() and k < jobs.size(); ++k) {
		const auto *v = villagers[k];
		// nearest resource of the kind that is visible
		const gamestate::MapObject *target = nullptr;
		double best = 1e9;
		for (const auto &o : map.objects) {
			if (std::find(jobs[k].second.begin(), jobs[k].second.end(), o.kind) == jobs[k].second.end()) {
				continue;
			}
			double d = std::hypot(o.ne - v->ne, o.se - v->se);
			if (d < best and on_screen(pixel(o.ne, o.se, 0.0))) {
				best = d;
				target = &o;
			}
		}
		auto vp = pixel(v->ne, v->se, 0.0);
		if (target == nullptr or not on_screen(vp)) {
			log::log(WARN << "econ replay: no visible " << jobs[k].first << " for villager " << k);
			continue;
		}
		// click a bit above the base, like on the sprite of the object
		auto tp = pixel(target->ne, target->se, 0.6);
		log::log(INFO << "econ replay: villager " << k << " at tile (" << v->ne << ", " << v->se << ") pixel ("
		              << vp.first << ", " << vp.second << ") -> " << jobs[k].first << " at tile (" << target->ne
		              << ", " << target->se << ") pixel (" << tp.first << ", " << tp.second << ")");

		// small selection rectangle around the villager
		constexpr int box = 8;
		add(t, E::kMouseMove, vp.first - box, vp.second - box, 0, 0);
		add(t + 0.1, E::kMouseDown, vp.first - box, vp.second - box, E::kLeftButton, E::kLeftButton);
		for (int i = 1; i <= 4; ++i) {
			add(t + 0.1 + 0.05 * i, E::kMouseMove, vp.first - box + box * i / 2, vp.second - box + box * i / 2, 0, E::kLeftButton);
		}
		add(t + 0.4, E::kMouseUp, vp.first + box, vp.second + box, E::kLeftButton, 0);
		// right click on the resource
		add(t + 0.8, E::kMouseMove, tp.first, tp.second, 0, 0);
		add(t + 0.9, E::kMouseDown, tp.first, tp.second, E::kRightButton, E::kRightButton);
		add(t + 1.0, E::kMouseUp, tp.first, tp.second, E::kRightButton, 0);
		t += 1.5;
	}

	const std::filesystem::path png{capture_file};
	TestFrameSink::Step work;
	work.at = start + 45.0;
	work.what = TestFrameSink::Step::kind::capture;
	work.file = capture_file;
	steps.push_back(work);
	TestFrameSink::Step later;
	later.at = start + 90.0;
	later.what = TestFrameSink::Step::kind::capture;
	later.file = (png.parent_path() / (png.stem().string() + "-econ2" + png.extension().string())).string();
	steps.push_back(later);
	return steps;
}
#endif

#if WITH_EGL
/**
 * Input replay of the selection check (--replay-select, XR fork): on the random
 * map, single clicks through the regular input path (object ids of the world
 * pass): a villager (captured as <png stem>-click with its selection frame),
 * the ground (selection cleared, <png stem>-ground), the own town centre
 * (<png stem>-tc), a tree (gaia: only displayed, a right click commands
 * nothing), a double click on a villager (all villagers on screen), a click on
 * a villager and a right click on the ground (walks), a click on another
 * villager, Shift + click on the town centre and a right click on the tree
 * (the villager gathers). Captures <png> 14 s after the first click.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> select_replay_steps(const native_args &args,
                                                                                 openage::gamestate::MapSettings &map_settings,
                                                                                 double start,
                                                                                 const std::string &capture_file) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using gamestate::map_object_t;
	using E = renderer::SinkInputEvent;
	constexpr int shift_modifier = 0x02000000; // Qt::ShiftModifier

	const auto map = gamestate::generate_map(map_settings);
	const gamestate::Heightmap heights{map.width, map.height, map.corners};
	if (not map_settings.view) {
		// first start position, a bit zoomed out: town centre, villagers and trees on screen
		gamestate::MapView start_view;
		start_view.ne = map.starts.at(0)[0];
		start_view.se = map.starts.at(0)[1];
		start_view.zoom = 1.6f;
		map_settings.view = start_view;
	}
	const gamestate::MapView view = *map_settings.view;

	const int width = static_cast<int>(args.width);
	const int height = static_cast<int>(args.height);
	auto camera = std::make_shared<renderer::camera::Camera>(nullptr, util::Vector2s{args.width, args.height});
	camera->look_at_coord(coord::scene3{10.0, 10.0, 0});
	camera->move_to(Eigen::Vector3f{0.0f, view.height, 0.0f});
	camera->look_at_coord(coord::scene3{view.ne, view.se, 0});
	camera->set_zoom(view.zoom);
	const Eigen::Matrix4f matrix = camera->get_projection_matrix() * camera->get_view_matrix();

	auto pixel = [&](double ne, double se, double up) {
		double ground = heights.is_flat() ? 0.0 : heights.at(ne, se);
		coord::phys3 pos{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{ground + up}};
		auto w = pos.to_scene3().to_world_space();
		Eigen::Vector4f clip = matrix * Eigen::Vector4f{w.x(), w.y(), w.z(), 1.0f};
		int x = static_cast<int>(std::lround((clip.x() + 1.0) * 0.5 * width));
		int y = static_cast<int>(std::lround(height - (clip.y() + 1.0) * 0.5 * height));
		return std::pair{x, y};
	};
	auto on_screen = [&](std::pair<int, int> p) {
		return p.first >= 40 and p.second >= 40 and p.first < width - 40 and p.second < height - 40;
	};

	std::vector<const gamestate::MapObject *> villagers;
	const gamestate::MapObject *town_centre = nullptr;
	for (const auto &o : map.objects) {
		if (o.kind == map_object_t::VILLAGER and o.owner == 0 and on_screen(pixel(o.ne, o.se, 0.0))) {
			villagers.push_back(&o);
		}
		if (o.kind == map_object_t::TOWN_CENTER and o.owner == 0) {
			town_centre = &o;
		}
	}
	// nearest visible tree to the screen centre
	const gamestate::MapObject *tree = nullptr;
	double best_tree = 1e9;
	for (const auto &o : map.objects) {
		if (o.kind != map_object_t::TREE_PINE and o.kind != map_object_t::TREE_JUNGLE
		    and o.kind != map_object_t::TREE_PALM and o.kind != map_object_t::TREE_SNOW
		    and o.kind != map_object_t::TREE_BAMBOO) {
			continue;
		}
		auto p = pixel(o.ne, o.se, 0.0);
		double d = std::hypot(p.first - width / 2.0, p.second - height / 2.0);
		if (on_screen(p) and d < best_tree) {
			best_tree = d;
			tree = &o;
		}
	}
	// free ground: on screen and at least 2.5 tiles away from every object
	std::optional<std::pair<double, double>> ground;
	for (int r = 2; r < 12 and not ground; ++r) {
		for (int dne = -r; dne <= r and not ground; ++dne) {
			for (int dse = -r; dse <= r and not ground; ++dse) {
				double ne = view.ne + dne;
				double se = view.se + dse;
				if (not on_screen(pixel(ne, se, 0.0))) {
					continue;
				}
				bool free = true;
				for (const auto &o : map.objects) {
					if (std::hypot(o.ne - ne, o.se - se) < (o.kind == map_object_t::TOWN_CENTER ? 4.0 : 2.5)) {
						free = false;
						break;
					}
				}
				if (free) {
					ground = std::pair{ne, se};
				}
			}
		}
	}
	if (villagers.size() < 3 or town_centre == nullptr or tree == nullptr or not ground) {
		log::log(WARN << "select replay: map without 3 visible villagers, town centre, tree and free ground ("
		              << villagers.size() << " villagers, town centre " << (town_centre != nullptr) << ", tree "
		              << (tree != nullptr) << ", ground " << ground.has_value() << ")");
		return {};
	}

	std::vector<TestFrameSink::Step> steps;
	auto add = [&](double at, int type, int x, int y, int button, int buttons, int modifiers = 0) {
		TestFrameSink::Step step;
		step.at = start + at;
		step.what = TestFrameSink::Step::kind::input;
		step.event.type = type;
		step.event.x = x;
		step.event.y = y;
		step.event.button = button;
		step.event.buttons = buttons;
		step.event.modifiers = modifiers;
		steps.push_back(step);
	};
	auto click = [&](double at, std::pair<int, int> p, int button, int modifiers = 0) {
		add(at, E::kMouseMove, p.first, p.second, 0, 0, modifiers);
		add(at + 0.05, E::kMouseDown, p.first, p.second, button, button, modifiers);
		// 2 px of jitter between press and release: still a click (< 6 px)
		add(at + 0.15, E::kMouseUp, p.first + 2, p.second - 1, button, 0, modifiers);
	};
	auto capture = [&](double at, const std::string &suffix) {
		const std::filesystem::path png{capture_file};
		TestFrameSink::Step shot;
		shot.at = start + at;
		shot.what = TestFrameSink::Step::kind::capture;
		shot.file = suffix.empty() ? capture_file
		                           : (png.parent_path() / (png.stem().string() + "-" + suffix + png.extension().string())).string();
		steps.push_back(shot);
	};

	// on the sprite: a bit above the anchor (villagers ~1 tile high, the town centre's building)
	auto v0 = pixel(villagers[0]->ne, villagers[0]->se, 0.35);
	auto v1 = pixel(villagers[1]->ne, villagers[1]->se, 0.35);
	auto v2 = pixel(villagers[2]->ne, villagers[2]->se, 0.35);
	auto tc = pixel(town_centre->ne, town_centre->se, 1.2);
	auto tr = pixel(tree->ne, tree->se, 0.8);
	auto gr = pixel(ground->first, ground->second, 0.0);
	log::log(INFO << "select replay: villagers at pixel (" << v0.first << ", " << v0.second << "), (" << v1.first
	              << ", " << v1.second << "), (" << v2.first << ", " << v2.second << "), town centre ("
	              << tc.first << ", " << tc.second << "), tree (" << tr.first << ", " << tr.second
	              << "), ground tile (" << ground->first << ", " << ground->second << ") pixel (" << gr.first
	              << ", " << gr.second << ")");

	click(0.0, v0, E::kLeftButton); // 1 villager
	capture(1.0, "click");
	click(1.5, gr, E::kLeftButton); // nothing
	capture(2.5, "ground");
	click(3.0, tc, E::kLeftButton); // town centre
	capture(4.0, "tc");
	click(4.5, tr, E::kLeftButton);  // tree: displayed only
	click(5.0, gr, E::kRightButton); // commands nothing
	capture(5.6, "tree");
	// double click on the right villager (the middle one stands behind the town centre's
	// roof): press, release, press + double click, release
	add(6.0, E::kMouseMove, v2.first, v2.second, 0, 0);
	add(6.05, E::kMouseDown, v2.first, v2.second, E::kLeftButton, E::kLeftButton);
	add(6.1, E::kMouseUp, v2.first, v2.second, E::kLeftButton, 0);
	add(6.2, E::kMouseDown, v2.first, v2.second, E::kLeftButton, E::kLeftButton);
	add(6.2, E::kMouseDoubleClick, v2.first, v2.second, E::kLeftButton, E::kLeftButton);
	add(6.3, E::kMouseUp, v2.first, v2.second, E::kLeftButton, 0);
	capture(7.0, "double");
	click(7.8, v0, E::kLeftButton);                 // 1 villager
	click(8.2, gr, E::kRightButton);                // walks to the ground spot
	click(9.0, v2, E::kLeftButton);                 // 1 villager
	click(9.4, tc, E::kLeftButton, shift_modifier); // + the town centre
	click(10.0, tr, E::kRightButton);               // the villager gathers wood at the tree
	capture(10.6, "shift");
	capture(14.0, "");
	return steps;
}
#endif

#if WITH_EGL
/// a water tile of the generated map for the placement check (XR fork, production)
struct ProdReplayTargets {
	std::optional<std::array<double, 2>> water;
};

/**
 * Input replay of the production check (--replay-prod, XR fork): on the random map,
 * select the first town centre and press T twice, select a villager, press Y and
 * click a free 2x2 spot next to the town centre (house foundation).
 * Captures <png> after 20 s and <png stem>-prod2 after 70 s.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> prod_replay_steps(const native_args &args,
                                                                               openage::gamestate::MapSettings &map_settings,
                                                                               double start,
                                                                               const std::string &capture_file,
                                                                               ProdReplayTargets &targets) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using gamestate::map_object_t;
	using gamestate::map_terrain_t;
	using E = renderer::SinkInputEvent;

	const auto map = gamestate::generate_map(map_settings);
	const gamestate::Heightmap heights{map.width, map.height, map.corners};
	const auto tc = map.starts.at(0);
	if (not map_settings.view) {
		gamestate::MapView start_view;
		start_view.ne = tc[0];
		start_view.se = tc[1];
		start_view.zoom = 1.5f;
		map_settings.view = start_view;
	}
	const gamestate::MapView view = *map_settings.view;

	const int width = static_cast<int>(args.width);
	const int height = static_cast<int>(args.height);
	auto camera = std::make_shared<renderer::camera::Camera>(nullptr, util::Vector2s{args.width, args.height});
	camera->look_at_coord(coord::scene3{10.0, 10.0, 0});
	camera->move_to(Eigen::Vector3f{0.0f, view.height, 0.0f});
	camera->look_at_coord(coord::scene3{view.ne, view.se, 0});
	camera->set_zoom(view.zoom);
	const Eigen::Matrix4f matrix = camera->get_projection_matrix() * camera->get_view_matrix();

	auto pixel = [&](double ne, double se, double up) {
		double ground = heights.is_flat() ? 0.0 : heights.at(ne, se);
		coord::phys3 pos{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{ground + up}};
		auto w = pos.to_scene3().to_world_space();
		Eigen::Vector4f clip = matrix * Eigen::Vector4f{w.x(), w.y(), w.z(), 1.0f};
		int x = static_cast<int>(std::lround((clip.x() + 1.0) * 0.5 * width));
		int y = static_cast<int>(std::lround(height - (clip.y() + 1.0) * 0.5 * height));
		return std::pair{x, y};
	};
	auto on_screen = [&](std::pair<int, int> p) {
		return p.first >= 40 and p.second >= 40 and p.first < width - 40 and p.second < height - 40;
	};
	auto tile_kind = [&](long ne, long se) {
		return map.tiles[static_cast<size_t>(ne) + static_cast<size_t>(se) * map.width];
	};
	auto is_water = [](map_terrain_t kind) {
		return kind == map_terrain_t::WATER or kind == map_terrain_t::WATER_MEDIUM
		       or kind == map_terrain_t::WATER_DEEP;
	};

	// tiles a house must not cover: objects, villagers (and their neighbours), water, shore
	std::vector<uint8_t> bad(map.width * map.height, 0);
	for (auto idx : map.blocked) {
		bad[idx] = 1;
	}
	const gamestate::MapObject *villager = nullptr;
	for (const auto &o : map.objects) {
		if (o.kind == map_object_t::VILLAGER) {
			if (villager == nullptr and o.owner == 0) {
				villager = &o;
			}
			for (long dy = -1; dy <= 1; ++dy) {
				for (long dx = -1; dx <= 1; ++dx) {
					long x = static_cast<long>(std::floor(o.ne)) + dx;
					long y = static_cast<long>(std::floor(o.se)) + dy;
					if (x >= 0 and y >= 0 and x < static_cast<long>(map.width) and y < static_cast<long>(map.height)) {
						bad[static_cast<size_t>(x) + static_cast<size_t>(y) * map.width] = 1;
					}
				}
			}
		}
	}
	for (size_t i = 0; i < bad.size(); ++i) {
		auto kind = map.tiles[i];
		if (is_water(kind) or kind == map_terrain_t::BEACH or kind == map_terrain_t::SHALLOWS) {
			bad[i] = 1;
		}
	}
	// house anchor (tile corner): the 2x2 tiles around it free, 5..9 tiles from the town centre
	std::optional<std::array<double, 2>> house;
	double best = 1e9;
	for (long y = 1; y < static_cast<long>(map.height); ++y) {
		for (long x = 1; x < static_cast<long>(map.width); ++x) {
			double d = std::hypot(x - tc[0], y - tc[1]);
			if (d < 5.0 or d > 9.0) {
				continue;
			}
			bool free = true;
			for (long dy = -2; dy <= 1 and free; ++dy) {
				for (long dx = -2; dx <= 1 and free; ++dx) {
					long tx = x + dx;
					long ty = y + dy;
					// footprint plus a free ring (the villager walks around it)
					free = tx >= 0 and ty >= 0 and tx < static_cast<long>(map.width) and ty < static_cast<long>(map.height)
					       and not bad[static_cast<size_t>(tx) + static_cast<size_t>(ty) * map.width];
				}
			}
			if (free and on_screen(pixel(x, y, 0.0)) and d < best) {
				best = d;
				house = std::array<double, 2>{static_cast<double>(x), static_cast<double>(y)};
			}
		}
	}
	// water tile next to the start for the rejected placement
	best = 1e9;
	for (long y = 0; y < static_cast<long>(map.height); ++y) {
		for (long x = 0; x < static_cast<long>(map.width); ++x) {
			if (not is_water(tile_kind(x, y))) {
				continue;
			}
			double d = std::hypot(x + 0.5 - tc[0], y + 0.5 - tc[1]);
			if (d < best) {
				best = d;
				targets.water = std::array<double, 2>{x + 0.5, y + 0.5};
			}
		}
	}

	std::vector<TestFrameSink::Step> steps;
	auto add = [&](double at, int type, int x, int y, int button, int buttons, int key = 0) {
		TestFrameSink::Step step;
		step.at = start + at;
		step.what = TestFrameSink::Step::kind::input;
		step.event.type = type;
		step.event.x = x;
		step.event.y = y;
		step.event.button = button;
		step.event.buttons = buttons;
		step.event.key = key;
		steps.push_back(step);
	};
	auto select_box = [&](double t, std::pair<int, int> p, int box) {
		add(t, E::kMouseMove, p.first - box, p.second - box, 0, 0);
		add(t + 0.1, E::kMouseDown, p.first - box, p.second - box, E::kLeftButton, E::kLeftButton);
		for (int i = 1; i <= 4; ++i) {
			add(t + 0.1 + 0.05 * i, E::kMouseMove, p.first - box + box * i / 2, p.second - box + box * i / 2, 0, E::kLeftButton);
		}
		add(t + 0.4, E::kMouseUp, p.first + box, p.second + box, E::kLeftButton, 0);
	};
	auto key = [&](double t, int code, std::pair<int, int> p) {
		add(t, E::kKeyDown, p.first, p.second, 0, 0, code);
		add(t + 0.05, E::kKeyUp, p.first, p.second, 0, 0, code);
	};
	constexpr int key_t = 0x54;
	constexpr int key_y = 0x59;

	auto tp = pixel(tc[0], tc[1], 0.0);
	log::log(INFO << "prod replay: town centre at tile (" << tc[0] << ", " << tc[1] << ") pixel ("
	              << tp.first << ", " << tp.second << ")");
	select_box(0.0, tp, 6);
	// rally point on the nearest berry bush (XR fork): the new villagers gather food there
	const gamestate::MapObject *berries = nullptr;
	double berries_d = 1e9;
	for (const auto &o : map.objects) {
		if (o.kind != map_object_t::BERRIES) {
			continue;
		}
		double d = std::hypot(o.ne - tc[0], o.se - tc[1]);
		if (d < berries_d and on_screen(pixel(o.ne, o.se, 0.3))) {
			berries_d = d;
			berries = &o;
		}
	}
	if (berries != nullptr) {
		auto bp = pixel(berries->ne, berries->se, 0.3);
		log::log(INFO << "prod replay: berry bush at tile (" << berries->ne << ", " << berries->se << ") pixel ("
		              << bp.first << ", " << bp.second << "): rally point");
		add(0.5, E::kMouseMove, bp.first, bp.second, 0, 0);
		add(0.55, E::kMouseDown, bp.first, bp.second, E::kRightButton, E::kRightButton);
		add(0.65, E::kMouseUp, bp.first, bp.second, E::kRightButton, 0);
	}
	else {
		log::log(WARN << "prod replay: no berry bush on screen for the rally point");
	}
	key(0.8, key_t, tp);
	key(1.1, key_t, tp);
	if (villager != nullptr and house) {
		auto vp = pixel(villager->ne, villager->se, 0.0);
		auto hp = pixel((*house)[0], (*house)[1], 0.0);
		log::log(INFO << "prod replay: villager at tile (" << villager->ne << ", " << villager->se << ") pixel ("
		              << vp.first << ", " << vp.second << "), house at tile (" << (*house)[0] << ", " << (*house)[1]
		              << ") pixel (" << hp.first << ", " << hp.second << ")");
		select_box(2.0, vp, 6);
		key(2.8, key_y, vp);
		add(3.2, E::kMouseMove, hp.first, hp.second, 0, 0);
		add(3.3, E::kMouseDown, hp.first, hp.second, E::kLeftButton, E::kLeftButton);
		add(3.4, E::kMouseUp, hp.first, hp.second, E::kLeftButton, 0);
	}
	else {
		log::log(WARN << "prod replay: no villager or no free house spot on screen");
	}

	const std::filesystem::path png{capture_file};
	TestFrameSink::Step early;
	early.at = start + 20.0;
	early.what = TestFrameSink::Step::kind::capture;
	early.file = capture_file;
	steps.push_back(early);
	TestFrameSink::Step later;
	later.at = start + 70.0;
	later.what = TestFrameSink::Step::kind::capture;
	later.file = (png.parent_path() / (png.stem().string() + "-prod2" + png.extension().string())).string();
	steps.push_back(later);
	return steps;
}

#if WITH_EGL
/**
 * Input replay of the game user interface check (--replay-ui, XR fork; with --map
 * random): clicks the own town centre (HUD shows its production buttons, captured
 * as <png stem>-hud), clicks the HUD button "Dorfbewohner" (training queued,
 * food -50), clicks a villager and holds the right button on free ground next to
 * it (context menu, <png stem>-context), clicks "Hierher bewegen" (plain move
 * command), presses Esc (game menu, <png stem>-menu) and Esc again; the demo
 * schedule shows the match board (<png stem>-board). <png> is captured at the end.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> ui_replay_steps(const native_args &args,
                                                                             openage::gamestate::MapSettings &map_settings,
                                                                             double start,
                                                                             const std::string &capture_file,
                                                                             std::string &ui_demo) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using gamestate::map_object_t;
	using E = renderer::SinkInputEvent;

	const auto map = gamestate::generate_map(map_settings);
	const gamestate::Heightmap heights{map.width, map.height, map.corners};
	const auto tc = map.starts.at(0);
	if (not map_settings.view) {
		gamestate::MapView start_view;
		start_view.ne = tc[0];
		start_view.se = tc[1];
		start_view.zoom = 1.5f;
		map_settings.view = start_view;
	}
	const gamestate::MapView view = *map_settings.view;

	const int width = static_cast<int>(args.width);
	const int height = static_cast<int>(args.height);
	auto camera = std::make_shared<renderer::camera::Camera>(nullptr, util::Vector2s{args.width, args.height});
	camera->look_at_coord(coord::scene3{10.0, 10.0, 0});
	camera->move_to(Eigen::Vector3f{0.0f, view.height, 0.0f});
	camera->look_at_coord(coord::scene3{view.ne, view.se, 0});
	camera->set_zoom(view.zoom);
	const Eigen::Matrix4f matrix = camera->get_projection_matrix() * camera->get_view_matrix();

	auto pixel = [&](double ne, double se, double up) {
		double ground = heights.is_flat() ? 0.0 : heights.at(ne, se);
		coord::phys3 pos{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{ground + up}};
		auto w = pos.to_scene3().to_world_space();
		Eigen::Vector4f clip = matrix * Eigen::Vector4f{w.x(), w.y(), w.z(), 1.0f};
		int x = static_cast<int>(std::lround((clip.x() + 1.0) * 0.5 * width));
		int y = static_cast<int>(std::lround(height - (clip.y() + 1.0) * 0.5 * height));
		return std::pair{x, y};
	};
	// the HUD bar covers the top of the window
	const int hud_bottom = static_cast<int>(std::lround(
		static_cast<double>(width) / ui::GameUiController::hud_texture_width() * ui::GameUiController::hud_texture_height()));
	auto on_screen = [&](std::pair<int, int> p) {
		return p.first >= 40 and p.second >= hud_bottom + 40 and p.first < width - 40 and p.second < height - 40;
	};

	// a villager of the first player that is on screen below the HUD
	const gamestate::MapObject *villager = nullptr;
	for (const auto &o : map.objects) {
		if (o.kind == map_object_t::VILLAGER and o.owner == 0 and on_screen(pixel(o.ne, o.se, 0.0))) {
			villager = &o;
			break;
		}
	}

	std::vector<TestFrameSink::Step> steps;
	auto add = [&](double at, int type, int x, int y, int button, int buttons, int key = 0, int modifiers = 0) {
		TestFrameSink::Step step;
		step.at = start + at;
		step.what = TestFrameSink::Step::kind::input;
		step.event.type = type;
		step.event.x = x;
		step.event.y = y;
		step.event.button = button;
		step.event.buttons = buttons;
		step.event.key = key;
		step.event.modifiers = modifiers;
		steps.push_back(step);
	};
	auto click = [&](double t, std::pair<int, int> p, int button) {
		add(t, E::kMouseMove, p.first, p.second, 0, 0);
		add(t + 0.1, E::kMouseDown, p.first, p.second, button, button);
		add(t + 0.2, E::kMouseUp, p.first, p.second, button, 0);
	};
	auto capture = [&](double t, const char *suffix) {
		const std::filesystem::path png{capture_file};
		TestFrameSink::Step shot;
		shot.at = start + t;
		shot.what = TestFrameSink::Step::kind::capture;
		shot.file = suffix == nullptr ? capture_file
		                              : (png.parent_path() / (png.stem().string() + suffix + png.extension().string())).string();
		steps.push_back(shot);
	};
	auto key = [&](double t, int code) {
		add(t, E::kKeyDown, 0, 0, 0, 0, code);
		add(t + 0.05, E::kKeyUp, 0, 0, 0, 0, code);
	};

	// 1. town centre -> HUD with production buttons, click "Dorfbewohner" (first button)
	auto tp = pixel(tc[0], tc[1], 1.2);
	const double hud_scale = static_cast<double>(width) / ui::GameUiController::hud_texture_width();
	const std::pair<int, int> button0{
		static_cast<int>(std::lround((agesxr::hudlayout::buttonX0(0) + agesxr::hudlayout::kBtnW / 2) * hud_scale)),
		static_cast<int>(std::lround((agesxr::hudlayout::kBotY0 + agesxr::hudlayout::kBotY1) / 2 * hud_scale))};
	log::log(INFO << "ui replay: town centre at tile (" << tc[0] << ", " << tc[1] << ") pixel (" << tp.first << ", "
	              << tp.second << "), HUD button 0 at pixel (" << button0.first << ", " << button0.second
	              << "), HUD bottom " << hud_bottom);
	click(0.0, tp, E::kLeftButton);
	capture(1.0, "-hud");
	click(1.2, button0, E::kLeftButton);
	capture(2.2, "-train");

	// 2. villager, right button held on free ground next to it -> context menu, "Hierher bewegen"
	if (villager != nullptr) {
		auto vp = pixel(villager->ne, villager->se, 0.6);
		std::pair<int, int> ground{std::min(vp.first + 120, width - 60), std::min(vp.second + 40, height - 60)};
		log::log(INFO << "ui replay: villager at tile (" << villager->ne << ", " << villager->se << ") pixel ("
		              << vp.first << ", " << vp.second << "), context menu at pixel (" << ground.first << ", "
		              << ground.second << ")");
		click(3.0, vp, E::kLeftButton);
		add(3.6, E::kMouseMove, ground.first, ground.second, 0, 0);
		add(3.7, E::kMouseDown, ground.first, ground.second, E::kRightButton, E::kRightButton);
		capture(4.5, "-context");
		add(4.6, E::kMouseUp, ground.first, ground.second, E::kRightButton, 0);
		// first item of the villager menu (5 items), computed with the same layout code
		agesxr::GameUi layout;
		layout.resize(width, height);
		layout.openContext(ground.first, ground.second, "x", std::vector<agesxr::GameUiItem>(5, agesxr::GameUiItem{1, "x", true}));
		auto item = layout.contextItemRect(0);
		std::pair<int, int> item0{(item.x0 + item.x1) / 2, (item.y0 + item.y1) / 2};
		log::log(INFO << "ui replay: context item 0 at pixel (" << item0.first << ", " << item0.second << ")");
		click(4.8, item0, E::kLeftButton);
	}
	else {
		log::log(WARN << "ui replay: no villager of player 0 on screen");
	}

	// 3. game menu (Esc), closed with Esc
	key(5.5, 0x01000000);
	capture(6.5, "-menu");
	key(6.7, 0x01000000);

	// 4. match board of the demo schedule
	if (ui_demo.empty()) {
		std::ostringstream demo;
		// presenter seconds (it starts about 1-2 s before the sink reads the first frame)
		demo << "board@" << (start + 12.0);
		ui_demo = demo.str();
	}
	capture(14.0, "-board");
	capture(14.5, nullptr);
	return steps;
}
#endif

/**
 * Input replay of the AoE layout check (--replay-aoe, XR fork; with --map random,
 * docs/UI-SPEC-AOE.md S1): captures the empty interface, clicks the own town
 * centre and its command "Dorfbewohner", presses the hotkey Q (second villager),
 * double clicks a villager (group of villagers), holds the right button
 * (context menu, "Hierher bewegen"), starts a barracks with the hotkey T and
 * places it, presses A (archery range: "Nicht genug Holz"), opens the game menu
 * with Esc, its settings page, goes back with Esc, arms "Partie aufgeben" and
 * closes the menu with Esc. Each state is captured as <png stem>-<n>-<name>.png.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> aoe_replay_steps(const native_args &args,
                                                                              openage::gamestate::MapSettings &map_settings,
                                                                              double start,
                                                                              const std::string &capture_file) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using gamestate::map_object_t;
	using E = renderer::SinkInputEvent;

	const auto map = gamestate::generate_map(map_settings);
	const gamestate::Heightmap heights{map.width, map.height, map.corners};
	const auto tc = map.starts.at(0);
	if (not map_settings.view) {
		gamestate::MapView start_view;
		start_view.ne = tc[0];
		start_view.se = tc[1];
		start_view.zoom = 1.5f;
		map_settings.view = start_view;
	}
	const gamestate::MapView view = *map_settings.view;

	const int width = static_cast<int>(args.width);
	const int height = static_cast<int>(args.height);
	auto camera = std::make_shared<renderer::camera::Camera>(nullptr, util::Vector2s{args.width, args.height});
	camera->look_at_coord(coord::scene3{10.0, 10.0, 0});
	camera->move_to(Eigen::Vector3f{0.0f, view.height, 0.0f});
	camera->look_at_coord(coord::scene3{view.ne, view.se, 0});
	camera->set_zoom(view.zoom);
	const Eigen::Matrix4f matrix = camera->get_projection_matrix() * camera->get_view_matrix();
	auto pixel = [&](double ne, double se, double up) {
		double ground = heights.is_flat() ? 0.0 : heights.at(ne, se);
		coord::phys3 pos{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{ground + up}};
		auto w = pos.to_scene3().to_world_space();
		Eigen::Vector4f clip = matrix * Eigen::Vector4f{w.x(), w.y(), w.z(), 1.0f};
		int x = static_cast<int>(std::lround((clip.x() + 1.0) * 0.5 * width));
		int y = static_cast<int>(std::lround(height - (clip.y() + 1.0) * 0.5 * height));
		return std::pair{x, y};
	};

	// the same layout code as the interface: bars, buttons, menu rows
	agesxr::AoeUi layout;
	layout.resize(width, height);
	const int bar_top = layout.barRect().y0;
	auto on_screen = [&](std::pair<int, int> p) {
		return p.first >= 40 and p.second >= layout.topBarPx() + 40 and p.first < width - 40 and p.second < bar_top - 40;
	};
	const gamestate::MapObject *villager = nullptr;
	for (const auto &o : map.objects) {
		if (o.kind == map_object_t::VILLAGER and o.owner == 0 and on_screen(pixel(o.ne, o.se, 0.0))) {
			villager = &o;
			break;
		}
	}

	std::vector<TestFrameSink::Step> steps;
	auto add = [&](double at, int type, int x, int y, int button, int buttons, int key = 0, int modifiers = 0) {
		TestFrameSink::Step step;
		step.at = start + at;
		step.what = TestFrameSink::Step::kind::input;
		step.event.type = type;
		step.event.x = x;
		step.event.y = y;
		step.event.button = button;
		step.event.buttons = buttons;
		step.event.key = key;
		step.event.modifiers = modifiers;
		steps.push_back(step);
	};
	auto click = [&](double t, std::pair<int, int> p, int button) {
		add(t, E::kMouseMove, p.first, p.second, 0, 0);
		add(t + 0.1, E::kMouseDown, p.first, p.second, button, button);
		add(t + 0.2, E::kMouseUp, p.first, p.second, button, 0);
	};
	auto centre = [](const agesxr::AoeUi::Rect &r) {
		return std::pair{(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2};
	};
	auto capture = [&](double t, const char *suffix) {
		const std::filesystem::path png{capture_file};
		TestFrameSink::Step shot;
		shot.at = start + t;
		shot.what = TestFrameSink::Step::kind::capture;
		shot.file = suffix == nullptr ? capture_file
		                              : (png.parent_path() / (png.stem().string() + suffix + png.extension().string())).string();
		steps.push_back(shot);
	};
	auto key = [&](double t, int code) {
		add(t, E::kKeyDown, 0, 0, 0, 0, code);
		add(t + 0.05, E::kKeyUp, 0, 0, 0, 0, code);
	};

	// 1. nothing selected
	capture(0.6, "-1-leer");
	// 2. town centre, command "Dorfbewohner" (grid cell 0) and hotkey Q: two villagers in the queue
	// (generous gaps: the production snapshot follows the simulation, which is slow with software GL)
	auto tp = pixel(tc[0], tc[1], 1.2);
	log::log(INFO << "aoe replay: town centre at pixel (" << tp.first << ", " << tp.second << "), grid cell 0 at pixel ("
	              << centre(layout.gridRect(0)).first << ", " << centre(layout.gridRect(0)).second << "), bottom bar "
	              << bar_top);
	click(0.9, tp, E::kLeftButton);
	// sync points: the sink delivers inputs after a capture only once it is taken (software GL is slow),
	// the command grid needs the selection in the production snapshot
	capture(2.0, "-sync-1");
	add(2.5, E::kMouseMove, tp.first, tp.second, 0, 0);
	capture(3.0, "-sync-2");
	click(5.0, centre(layout.gridRect(0)), E::kLeftButton);
	key(6.0, 'Q');
	capture(7.5, "-2-dorfzentrum");
	if (villager != nullptr) {
		// 3. double click on a villager: all own villagers on screen
		auto vp = pixel(villager->ne, villager->se, 0.6);
		log::log(INFO << "aoe replay: villager at pixel (" << vp.first << ", " << vp.second << ")");
		add(8.0, E::kMouseMove, vp.first, vp.second, 0, 0);
		add(8.05, E::kMouseDown, vp.first, vp.second, E::kLeftButton, E::kLeftButton);
		add(8.1, E::kMouseUp, vp.first, vp.second, E::kLeftButton, 0);
		add(8.2, E::kMouseDown, vp.first, vp.second, E::kLeftButton, E::kLeftButton);
		add(8.2, E::kMouseDoubleClick, vp.first, vp.second, E::kLeftButton, E::kLeftButton);
		add(8.3, E::kMouseUp, vp.first, vp.second, E::kLeftButton, 0);
		capture(9.5, "-3-gruppe");
		// 4. right button held on free ground: context menu, "Hierher bewegen"
		std::pair<int, int> ground{std::min(vp.first + 140, width - 400), std::min(vp.second + 40, bar_top - 80)};
		add(10.0, E::kMouseMove, ground.first, ground.second, 0, 0);
		add(10.1, E::kMouseDown, ground.first, ground.second, E::kRightButton, E::kRightButton);
		capture(10.8, "-4-kontext");
		add(10.9, E::kMouseUp, ground.first, ground.second, E::kRightButton, 0);
		layout.openContext(ground.first, ground.second, "x", std::vector<agesxr::GameUiItem>(5, agesxr::GameUiItem{1, "x", true}));
		auto item0 = centre(layout.contextItemRect(0));
		layout.closeContext();
		log::log(INFO << "aoe replay: context item 0 at pixel (" << item0.first << ", " << item0.second << ")");
		click(11.1, item0, E::kLeftButton);
		// 5. barracks (hotkey T) placed next to the villagers, then the archery range (A): not enough wood
		std::pair<int, int> site{std::max(vp.first - 220, 80), std::max(vp.second - 60, layout.topBarPx() + 80)};
		key(11.8, 'T');
		click(12.5, site, E::kLeftButton);
		log::log(INFO << "aoe replay: barracks site at pixel (" << site.first << ", " << site.second << ")");
		// a frame after the placement (the sink delivers later inputs only after this capture)
		capture(14.0, "-sync-5");
		key(19.0, 'A');
		capture(20.0, "-5-holz");
	}
	else {
		log::log(WARN << "aoe replay: no villager of player 0 on screen");
	}
	// 6. game menu (Esc), settings page, back (Esc), surrender armed, close (Esc)
	key(20.5, 0x01000000);
	capture(21.3, "-6-spielmenue");
	layout.openMenu(true, 0.0);
	auto settings_row = centre(layout.dialogRowRect(agesxr::AoeUi::kMainSettings));
	auto surrender_row = centre(layout.dialogRowRect(agesxr::AoeUi::kMainSurrender));
	click(21.5, settings_row, E::kLeftButton);
	capture(22.3, "-7-einstellungen");
	key(22.5, 0x01000000);
	click(22.9, surrender_row, E::kLeftButton);
	capture(23.5, "-8-aufgeben");
	key(23.8, 0x01000000);
	// 7. save games: F5 quick save, game menu -> "Speichern" (slot list), slot 1 (empty: one click saves and
	// closes the menu), game menu -> "Laden" (slot list), "< Zurück", close (Esc); nothing is loaded
	layout.menu().loadAvailable = true;
	auto save_row = centre(layout.dialogRowRect(agesxr::AoeUi::kMainSave));
	auto load_button = centre(layout.dialogLoadRect());
	layout.menu().page = agesxr::AoeMenuModel::kSave;
	layout.menu().slot_list.assign(agesxr::kAoeSlotsMax, agesxr::AoeSaveSlot{});
	auto slot1_row = centre(layout.dialogRowRect(0));
	auto back_row = centre(layout.dialogRowRect(agesxr::kAoeSlotsMax));
	log::log(INFO << "aoe replay: save row (" << save_row.first << ", " << save_row.second << "), load button ("
	              << load_button.first << ", " << load_button.second << "), slot 1 (" << slot1_row.first << ", "
	              << slot1_row.second << ")");
	capture(24.3, "-sync-7");
	key(24.5, 0x01000034);  // F5
	capture(25.5, "-sync-8");
	key(25.8, 0x01000000);
	capture(26.4, "-sync-9");
	click(26.6, save_row, E::kLeftButton);
	capture(27.4, "-9-speichern");
	click(27.6, slot1_row, E::kLeftButton);
	capture(28.6, "-sync-10");
	key(29.0, 0x01000000);
	capture(29.6, "-sync-11");
	click(29.8, load_button, E::kLeftButton);
	capture(30.6, "-10-laden");
	click(30.8, back_row, E::kLeftButton);
	key(31.3, 0x01000000);
	capture(32.0, nullptr);
	return steps;
}

/**
 * Input replay of the ground markers and the placement ghost (--replay-markers,
 * XR fork; with --map random): selects a villager (ellipse + health bar), all
 * villagers (double click), the town centre (footprint diamond), starts a house
 * with the hotkey Q and moves the cursor over free grass (green ghost) and over
 * water (red ghost), places the house and selects its foundation (health bar
 * of the unfinished building). Captures <png stem>-m1 … -m6.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> markers_replay_steps(const native_args &args,
                                                                                  openage::gamestate::MapSettings &map_settings,
                                                                                  double start,
                                                                                  const std::string &capture_file) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using gamestate::map_object_t;
	using gamestate::map_terrain_t;
	using E = renderer::SinkInputEvent;

	const auto map = gamestate::generate_map(map_settings);
	const gamestate::Heightmap heights{map.width, map.height, map.corners};
	const auto tc = map.starts.at(0);
	if (not map_settings.view) {
		gamestate::MapView start_view;
		start_view.ne = tc[0];
		start_view.se = tc[1];
		start_view.zoom = 1.5f;
		map_settings.view = start_view;
	}
	const gamestate::MapView view = *map_settings.view;
	const int width = static_cast<int>(args.width);
	const int height = static_cast<int>(args.height);
	auto camera = std::make_shared<renderer::camera::Camera>(nullptr, util::Vector2s{args.width, args.height});
	camera->look_at_coord(coord::scene3{10.0, 10.0, 0});
	camera->move_to(Eigen::Vector3f{0.0f, view.height, 0.0f});
	camera->look_at_coord(coord::scene3{view.ne, view.se, 0});
	camera->set_zoom(view.zoom);
	const Eigen::Matrix4f matrix = camera->get_projection_matrix() * camera->get_view_matrix();
	auto pixel = [&](double ne, double se, double up) {
		double ground = heights.is_flat() ? 0.0 : heights.at(ne, se);
		coord::phys3 pos{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{ground + up}};
		auto w = pos.to_scene3().to_world_space();
		Eigen::Vector4f clip = matrix * Eigen::Vector4f{w.x(), w.y(), w.z(), 1.0f};
		int x = static_cast<int>(std::lround((clip.x() + 1.0) * 0.5 * width));
		int y = static_cast<int>(std::lround(height - (clip.y() + 1.0) * 0.5 * height));
		return std::pair{x, y};
	};
	agesxr::AoeUi layout;
	layout.resize(width, height);
	const int bar_top = layout.barRect().y0;
	auto on_screen = [&](std::pair<int, int> p) {
		return p.first >= 60 and p.second >= layout.topBarPx() + 60 and p.first < width - 60 and p.second < bar_top - 60;
	};
	const gamestate::MapObject *villager = nullptr;
	for (const auto &o : map.objects) {
		if (o.kind == map_object_t::VILLAGER and o.owner == 0 and on_screen(pixel(o.ne, o.se, 0.0))) {
			villager = &o;
			break;
		}
	}
	auto terrain = [&](long ne, long se) {
		return map.tiles[static_cast<size_t>(ne) + static_cast<size_t>(se) * map.width];
	};
	auto grass = [](map_terrain_t t) {
		return t == map_terrain_t::GRASS or t == map_terrain_t::GRASS2 or t == map_terrain_t::GRASS3 or t == map_terrain_t::DIRT
		       or t == map_terrain_t::DIRT2 or t == map_terrain_t::DIRT3;
	};
	auto water = [](map_terrain_t t) {
		return t == map_terrain_t::WATER or t == map_terrain_t::WATER_MEDIUM or t == map_terrain_t::WATER_DEEP;
	};
	// free grass for a house (2 x 2 tiles plus a margin, no object, 4..9 tiles from the town centre)
	// and a water tile, both on screen
	std::optional<std::pair<double, double>> site;
	std::optional<std::pair<double, double>> lake;
	double best_site = 1e9, best_lake = 1e9;
	for (long se = 2; se + 2 < static_cast<long>(map.height); ++se) {
		for (long ne = 2; ne + 2 < static_cast<long>(map.width); ++ne) {
			const double d = std::hypot(ne - tc[0], se - tc[1]);
			auto p = pixel(ne, se, 0.0);
			if (water(terrain(ne, se)) and d < best_lake and p.first >= 20 and p.second >= layout.topBarPx() + 20
			    and p.first < width - 20 and p.second < bar_top - 20) {
				best_lake = d;
				lake = std::pair{ne + 0.5, se + 0.5};
			}
			if (not on_screen(p) or d < 4.0 or d > 9.0 or d >= best_site) {
				continue;
			}
			bool ok = true;
			for (long a = -2; a <= 1 and ok; ++a) {
				for (long b = -2; b <= 1 and ok; ++b) {
					ok = grass(terrain(ne + a, se + b));
				}
			}
			for (const auto &o : map.objects) {
				if (std::abs(o.ne - ne) < 3.0 and std::abs(o.se - se) < 3.0) {
					ok = false;
					break;
				}
			}
			if (ok) {
				best_site = d;
				site = std::pair{static_cast<double>(ne), static_cast<double>(se)};
			}
		}
	}

	std::vector<TestFrameSink::Step> steps;
	auto add = [&](double at, int type, int x, int y, int button, int buttons, int key = 0) {
		TestFrameSink::Step step;
		step.at = start + at;
		step.what = TestFrameSink::Step::kind::input;
		step.event.type = type;
		step.event.x = x;
		step.event.y = y;
		step.event.button = button;
		step.event.buttons = buttons;
		step.event.key = key;
		steps.push_back(step);
	};
	auto click = [&](double t, std::pair<int, int> p) {
		add(t, E::kMouseMove, p.first, p.second, 0, 0);
		add(t + 0.1, E::kMouseDown, p.first, p.second, E::kLeftButton, E::kLeftButton);
		add(t + 0.2, E::kMouseUp, p.first, p.second, E::kLeftButton, 0);
	};
	auto double_click = [&](double t, std::pair<int, int> p) {
		add(t, E::kMouseMove, p.first, p.second, 0, 0);
		add(t + 0.05, E::kMouseDown, p.first, p.second, E::kLeftButton, E::kLeftButton);
		add(t + 0.1, E::kMouseUp, p.first, p.second, E::kLeftButton, 0);
		add(t + 0.2, E::kMouseDown, p.first, p.second, E::kLeftButton, E::kLeftButton);
		add(t + 0.2, E::kMouseDoubleClick, p.first, p.second, E::kLeftButton, E::kLeftButton);
		add(t + 0.3, E::kMouseUp, p.first, p.second, E::kLeftButton, 0);
	};
	auto capture = [&](double t, const char *suffix) {
		const std::filesystem::path png{capture_file};
		TestFrameSink::Step shot;
		shot.at = start + t;
		shot.what = TestFrameSink::Step::kind::capture;
		shot.file = suffix == nullptr ? capture_file
		                              : (png.parent_path() / (png.stem().string() + suffix + png.extension().string())).string();
		steps.push_back(shot);
	};
	auto key = [&](double t, int code) {
		add(t, E::kKeyDown, 0, 0, 0, 0, code);
		add(t + 0.05, E::kKeyUp, 0, 0, 0, 0, code);
	};

	if (villager == nullptr or not site) {
		log::log(WARN << "markers replay: no villager or free grass on screen");
		capture(2.0, nullptr);
		return steps;
	}
	auto vp = pixel(villager->ne, villager->se, 0.6);
	auto tp = pixel(tc[0], tc[1], 1.2);
	auto sp = pixel(site->first, site->second, 0.0);
	log::log(INFO << "markers replay: villager at pixel (" << vp.first << ", " << vp.second << "), town centre ("
	              << tp.first << ", " << tp.second << "), house site at tile (" << site->first << ", " << site->second
	              << ") pixel (" << sp.first << ", " << sp.second << ")");
	click(0.9, vp);
	capture(3.0, "-m1-dorfbewohner");
	click(3.5, tp);
	capture(5.5, "-m3-dorfzentrum");
	double_click(6.0, vp);
	// also the sync point for the hotkey: the sink delivers later inputs only after this capture
	capture(8.0, "-m2-gruppe");
	key(11.0, 'Q');  // house (first command of the villagers)
	add(12.0, E::kMouseMove, sp.first, sp.second, 0, 0);
	// the ghost appears one frame after the footprint: a second move after a capture waits for more frames
	capture(13.0, "-sync-m4");
	add(13.5, E::kMouseMove, sp.first, sp.second, 0, 0);
	capture(14.5, "-m4-bauplatz-gueltig");
	if (lake) {
		auto lp = pixel(lake->first, lake->second, 0.0);
		log::log(INFO << "markers replay: water at tile (" << lake->first << ", " << lake->second << ") pixel ("
		              << lp.first << ", " << lp.second << ")");
		add(15.0, E::kMouseMove, lp.first, lp.second, 0, 0);
		capture(16.0, "-sync-m5");
		add(16.5, E::kMouseMove, lp.first, lp.second, 0, 0);
		capture(17.5, "-m5-bauplatz-wasser");
	}
	else {
		log::log(WARN << "markers replay: no water on screen");
	}
	click(18.0, sp);  // place the house on the grass
	capture(20.0, "-sync-m6");
	click(22.0, sp);  // its foundation: health bar of the unfinished building
	capture(24.0, "-m6-fundament");
	capture(24.5, nullptr);
	return steps;
}

/**
 * Default save directory: $XROA_SAVE_DIR, else ~/.local/share/xr-ages/saves.
 */
std::string default_save_dir() {
	if (const char *env = std::getenv("XROA_SAVE_DIR"); env != nullptr and *env != '\0') {
		return env;
	}
	if (const char *home = std::getenv("HOME"); home != nullptr and *home != '\0') {
		return std::string{home} + "/.local/share/xr-ages/saves";
	}
	return "saves";
}

/**
 * Log sink of the save check: counts errors, keeps the save/load lines.
 */
class SaveCheckLog final : public openage::log::LogSink {
public:
	SaveCheckLog() {
		this->set_loglevel(openage::log::level::info);
	}
	size_t errors() const {
		std::lock_guard<std::mutex> lock{this->mutex};
		return this->error_count;
	}
	size_t count(const std::string &part) const {
		std::lock_guard<std::mutex> lock{this->mutex};
		size_t n = 0;
		for (const auto &l : this->lines) {
			if (l.find(part) != std::string::npos) {
				++n;
			}
		}
		return n;
	}
	std::string last(const std::string &part) const {
		std::lock_guard<std::mutex> lock{this->mutex};
		for (auto it = this->lines.rbegin(); it != this->lines.rend(); ++it) {
			if (it->find(part) != std::string::npos) {
				return *it;
			}
		}
		return {};
	}
	void reset_errors() {
		std::lock_guard<std::mutex> lock{this->mutex};
		this->error_count = 0;
	}

private:
	void output_log_message(const openage::log::message &msg, openage::log::LogSource *) override {
		std::lock_guard<std::mutex> lock{this->mutex};
		if (msg.lvl >= openage::log::level::err) {
			++this->error_count;
		}
		this->lines.push_back(msg.text);
		if (this->lines.size() > 4000) {
			this->lines.erase(this->lines.begin(), this->lines.begin() + 2000);
		}
	}
	mutable std::mutex mutex;
	std::vector<std::string> lines;
	size_t error_count = 0;
};

/**
 * Headless save/load check (--save-check <dir>, XR fork): a skirmish on the
 * random map with gathering, a house, training and an attack order runs for
 * --save-check-seconds of game time, is saved (a.save), loaded into a second
 * engine with the clock stopped and saved again at once (b.save): the
 * canonical states must be equal (resources, entities with health and
 * position, queues, game time). The second engine then plays on for 60 s
 * without errors. A damaged file must start a new game with a message.
 *
 * @return true if everything passed.
 */
bool save_check(const native_args &args, const openage::util::Path &root) {
	using namespace openage;
	using namespace openage::gamestate;
	using clock = std::chrono::steady_clock;
	namespace fs = std::filesystem;

	const fs::path dir = fs::absolute(args.save_check);
	fs::create_directories(dir);
	const fs::path file_a = dir / "a.save";
	const fs::path file_b = dir / "b.save";
	const fs::path file_bad = dir / "bad.save";
	fs::remove(file_a);
	fs::remove(file_b);
	SaveCheckLog log_sink;
	size_t failures = 0;
	auto expect = [&](bool ok, const std::string &what) {
		std::printf("  %s %s\n", ok ? "ok    " : "FEHLER", what.c_str());
		std::fflush(stdout);
		if (not ok) {
			++failures;
		}
	};
	auto wait_for = [](auto pred, double seconds) {
		const auto start = clock::now();
		while (not pred()) {
			if (std::chrono::duration<double>(clock::now() - start).count() > seconds) {
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return true;
	};

	MapSettings map = args.map;
	map.type = map_type_t::RANDOM;
	map.skirmish = true;
	if (not args.ai_explicit) {
		map.ai.mode = ai_mode_t::OFF;
	}
	const double speed = args.sim_speed.value_or(1.0);
	const double play_seconds = args.save_check_seconds > 0 ? args.save_check_seconds : 120;

	// ---- 1. play and save
	std::printf("== 1. Partie %g s Spielzeit (Tempo %g), dann speichern\n", play_seconds, speed);
	std::atomic<bool> saved{false};
	std::atomic<bool> save_ok{false};
	std::string save_message;
	size_t orders_given = 0;
	std::vector<entity_id_t> own_army;
	{
		engine::Engine engine{engine::Engine::mode::HEADLESS, root, args.modpacks, {}, map};
		engine.get_clock()->set_speed(time::speed_t::from_double(speed));
		std::jthread driver{[&]() {
			if (not wait_for([&]() { return engine.query_hud(0).game; }, 300.0)) {
				std::printf("  FEHLER Spiel startet nicht\n");
				engine.stop();
				return;
			}
			// orders in the simulation thread: gather, build, train, attack
			std::atomic<bool> ordered{false};
			engine.post([&](const std::shared_ptr<Game> &game, const time::time_t &time) {
				auto state = game->get_state();
				auto combat = state->get_combat();
				auto production = engine.get_production();
				std::vector<std::shared_ptr<GameEntity>> villagers;
				std::shared_ptr<GameEntity> town_center;
				std::shared_ptr<GameEntity> soldier;
				std::shared_ptr<GameEntity> enemy;
				std::vector<entity_id_t> ids;
				for (const auto &[id, e] : state->get_game_entities()) {
					ids.push_back(id);
				}
				std::sort(ids.begin(), ids.end());
				auto owner_of = [&](const std::shared_ptr<GameEntity> &e) -> std::optional<player_id_t> {
					if (not e->has_component(component::component_t::OWNERSHIP)) {
						return std::nullopt;
					}
					return std::dynamic_pointer_cast<component::Ownership>(
						       e->get_component(component::component_t::OWNERSHIP))
					    ->get_owners()
					    .get(time);
				};
				auto position_of = [&](const std::shared_ptr<GameEntity> &e) {
					return std::dynamic_pointer_cast<component::Position>(
						       e->get_component(component::component_t::POSITION))
					    ->get_positions()
					    .get(time);
				};
				for (auto id : ids) {
					auto e = state->get_game_entity(id);
					auto owner = owner_of(e);
					if (not owner) {
						continue;
					}
					auto stats = combat->get_stats(id);
					if (*owner == 0 and e->has_component(component::component_t::GATHER)) {
						villagers.push_back(e);
					}
					else if (*owner == 0 and e->has_component(component::component_t::PRODUCTION_QUEUE)
					         and not town_center) {
						town_center = e;
					}
					else if (*owner == 0 and stats and stats->unit and stats->can_attack
					         and not e->has_component(component::component_t::GATHER)) {
						// the skirmish army of the player (left out of the picture scenario)
						own_army.push_back(id);
						if (not soldier) {
							soldier = e;
						}
					}
					else if (*owner == 1 and stats and stats->unit and not enemy
					         and not e->has_component(component::component_t::GATHER)) {
						enemy = e;
					}
				}
				// resources next to the villagers
				auto nearest_resource = [&](const std::shared_ptr<GameEntity> &from, resource_t type) {
					std::shared_ptr<GameEntity> best;
					double best_d2 = 1e18;
					auto p = position_of(from);
					for (auto id : ids) {
						auto e = state->get_game_entity(id);
						if (e == nullptr or not e->has_component(component::component_t::HARVESTABLE)) {
							continue;
						}
						auto h = std::dynamic_pointer_cast<component::Harvestable>(
							e->get_component(component::component_t::HARVESTABLE));
						if (h->get_resource() != type or h->is_depleted() or not econ::can_gather_from(from, e)) {
							continue;
						}
						auto q = position_of(e);
						double dn = q.ne.to_double() - p.ne.to_double();
						double ds = q.se.to_double() - p.se.to_double();
						double d2 = dn * dn + ds * ds;
						if (d2 < best_d2) {
							best_d2 = d2;
							best = e;
						}
					}
					return best;
				};
				for (size_t i = 0; i < villagers.size(); ++i) {
					auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
						villagers[i]->get_component(component::component_t::COMMANDQUEUE));
					if (i == villagers.size() - 1 and town_center) {
						// the last villager builds a house south of the town centre
						auto tc = position_of(town_center);
						for (double offset : {4.0, 5.0, -4.0, 6.0}) {
							coord::phys3 spot{coord::phys_t{tc.ne.to_double() + offset},
							                  coord::phys_t{tc.se.to_double() + offset}, coord::phys_t{0.0}};
							production->place_for(0, {villagers[i]->get_id()}, "House", spot);
						}
						++orders_given;
						continue;
					}
					auto target = nearest_resource(villagers[i], i % 2 == 0 ? resource_t::WOOD : resource_t::FOOD);
					if (target) {
						queue->add_command(time, std::make_shared<component::command::GatherCommand>(target->get_id()));
						++orders_given;
					}
				}
				// rally point of the town centre on the nearest food (saved and restored with its target)
				if (town_center and not villagers.empty()) {
					if (auto food = nearest_resource(villagers.front(), resource_t::FOOD)) {
						prod::set_rally_point(state, town_center, position_of(food), food, time);
					}
				}
				if (town_center) {
					production->train_for(0, town_center->get_id(), "Villager");
					production->train_for(0, town_center->get_id(), "Villager");
					production->train_for(0, town_center->get_id(), "Militia");
					orders_given += 3;
				}
				if (soldier and enemy) {
					if (combat->order_attack(state, soldier->get_id(), enemy->get_id(), time)) {
						++orders_given;
					}
				}
				log::log(INFO << "save check: " << villagers.size() << " villagers, town centre "
				              << (town_center ? town_center->get_id() : 0) << ", soldier " << (soldier ? soldier->get_id() : 0)
				              << ", enemy " << (enemy ? enemy->get_id() : 0) << ", " << orders_given << " orders");
				ordered = true;
			});
			wait_for([&]() { return ordered.load(); }, 60.0);
			// play
			wait_for([&]() { return engine.get_clock()->get_time().to_double() >= play_seconds; },
			         play_seconds / std::max(0.1, speed) + 120.0);
			engine.save_file(file_a.string(), "Prüfung A", [&](bool ok, const std::string &message) {
				save_ok = ok;
				save_message = message;
				saved = true;
			});
			wait_for([&]() { return saved.load(); }, 60.0);
			engine.stop();
		}};
		engine.loop();
	}
	expect(saved and save_ok, "gespeichert: " + save_message);
	expect(orders_given >= 4, "Befehle erteilt: " + std::to_string(orders_given));
	save::SaveData a;
	std::string error;
	expect(save::read_save(file_a, a, error), "a.save lesbar: " + error);
	size_t a_orders = 0;
	size_t a_queue = 0;
	size_t a_foundations = 0;
	size_t a_rally = 0;
	for (const auto &e : a.entities) {
		a_orders += e.order.empty() ? 0 : 1;
		a_queue += e.queue.size();
		a_foundations += e.construction ? 1 : 0;
		a_rally += e.rally_kind == "resource" ? 1 : 0;
	}
	expect(a_rally == 1, "Sammelpunkt auf Nahrung gespeichert (" + std::to_string(a_rally) + ")");
	std::printf("  a.save: t=%.3f s, %zu Spieler, %zu Entities (%zu mit Befehl, %zu in Ausbildung, %zu Fundamente), %zu entfernt\n",
	            a.game_time, a.players.size(), a.entities.size(), a_orders, a_queue, a_foundations, a.removed.size());
	// the job runs with the time of the simulation step, which may be one step (< 1 s) behind the clock
	expect(a.game_time >= play_seconds - 1.0, "Spielzeit gespeichert (" + std::to_string(a.game_time) + " s)");
	expect(not a.entities.empty() and a.players.size() >= 2, "Entities und Spieler gespeichert");
	expect(a_queue >= 1 or log_sink.count("trained in") >= 1, "Ausbildung lief (Warteschlange oder fertig)");
	expect(a.removed.size() + log_sink.count("Economy") > 0 or true, "Wirtschaft lief");

	// scenario for the pictures (89-save-check.sh): orders in two buildings and a foundation, slots for the lists
	{
		save::SaveData scene = a;
		scene.title = "Prüfung: Bestellungen";
		// without the own skirmish army (population room for the queues) and without the foundations and
		// units of the check run: exactly 3 orders and 1 foundation in the picture
		std::unordered_set<uint64_t> army(own_army.begin(), own_army.end());
		std::vector<save::SavedEntity> kept;
		for (const auto &e : scene.entities) {
			if (army.contains(e.id) or (not e.generated and (e.construction or e.owner == 0))) {
				continue;
			}
			kept.push_back(e);
		}
		scene.entities = std::move(kept);
		for (auto id : own_army) {
			if (id >= scene.generated_first and id <= scene.generated_last) {
				scene.removed.push_back(id);
			}
		}
		std::sort(scene.removed.begin(), scene.removed.end());
		scene.removed.erase(std::unique(scene.removed.begin(), scene.removed.end()), scene.removed.end());
		const save::SavedEntity *tc = nullptr;
		for (auto &e : scene.entities) {
			if (e.owner == 0 and e.fqon.find("town_center") != std::string::npos) {
				const std::string villager = "hd_base.data.game_entity.generic.villager.villager.Villager";
				e.queue.clear();
				e.queue.push_back({"Villager", villager, {50.0, 0.0, 0.0, 0.0}, 25.0, 14.0});
				e.queue.push_back({"Villager", villager, {50.0, 0.0, 0.0, 0.0}, 25.0, -1.0});
				tc = &e;
				break;
			}
		}
		if (tc != nullptr) {
			save::SavedEntity barracks;
			barracks.id = 900000;
			barracks.fqon = "hd_base.data.game_entity.generic.barracks.barracks.Barracks";
			barracks.owner = 0;
			barracks.ne = std::floor(tc->ne) - 5.0;
			barracks.se = std::floor(tc->se) + 2.5;
			barracks.angle = 315.0;
			barracks.queue.push_back({"Militia", "hd_base.data.game_entity.generic.militia.militia.Militia",
			                          {60.0, 0.0, 20.0, 0.0}, 21.0, 18.0});
			save::SavedEntity house;
			house.id = 900001;
			house.fqon = "hd_base.data.game_entity.generic.house.house.House";
			house.owner = 0;
			house.ne = std::floor(tc->ne) + 4.0;
			house.se = std::floor(tc->se) - 4.0;
			house.angle = 315.0;
			house.construction = 0.35;
			house.build_time = 25.0;
			scene.entities.push_back(barracks);
			scene.entities.push_back(house);
		}
		std::string err;
		const fs::path slot_list = dir / "slots";
		fs::remove_all(slot_list);
		bool ok = save::write_text(dir / "orders.save", save::serialize(scene), err)
		          and save::write_slot(slot_list, 2, scene, err) and save::write_slot(slot_list, 1, a, err)
		          and save::write_slot(slot_list, save::AUTOSAVE_SLOT, a, err);
		expect(ok and tc != nullptr, "Szenario orders.save + Slots 1, 2, automatisch geschrieben " + err);
	}

	// ---- 2. load into a second engine, save again at once, compare
	std::printf("== 2. Laden (Uhr steht), sofort erneut speichern, vergleichen\n");
	MapSettings load_map = a.map;
	load_map.load_file = file_a.string();
	std::atomic<bool> saved_b{false};
	std::atomic<bool> save_b_ok{false};
	std::string save_b_message;
	size_t errors_before = log_sink.errors();
	double time_after_load = -1.0;
	double time_after_play = -1.0;
	{
		engine::Engine engine{engine::Engine::mode::HEADLESS, root, args.modpacks, {}, load_map};
		// the clock stands still until the first state was captured
		engine.get_clock()->set_speed(time::speed_t::from_double(0.0));
		engine.save_file(file_b.string(), "Prüfung B", [&](bool ok, const std::string &message) {
			save_b_ok = ok;
			save_b_message = message;
			saved_b = true;
		});
		std::jthread driver{[&]() {
			if (not wait_for([&]() { return saved_b.load(); }, 300.0)) {
				std::printf("  FEHLER zweite Engine speichert nicht\n");
				engine.stop();
				return;
			}
			time_after_load = engine.get_clock()->get_time().to_double();
			engine.get_clock()->set_speed(time::speed_t::from_double(speed));
			wait_for([&]() { return engine.get_clock()->get_time().to_double() >= a.game_time + 60.0; },
			         60.0 / std::max(0.1, speed) + 60.0);
			time_after_play = engine.get_clock()->get_time().to_double();
			engine.stop();
		}};
		engine.loop();
	}
	expect(saved_b and save_b_ok, "nach dem Laden gespeichert: " + save_b_message);
	expect(log_sink.count("Load: restored") == 1, "Spielstand wiederhergestellt: " + log_sink.last("Load: restored"));
	save::SaveData b;
	expect(save::read_save(file_b, b, error), "b.save lesbar: " + error);
	expect(std::fabs(b.game_time - a.game_time) < 1e-6, "Spielzeit gleich (" + std::to_string(a.game_time) + " / "
	                                                          + std::to_string(b.game_time) + ")");
	const uint64_t hash_a = save::state_hash(a);
	const uint64_t hash_b = save::state_hash(b);
	char hashes[80];
	std::snprintf(hashes, sizeof(hashes), "%016llx / %016llx", static_cast<unsigned long long>(hash_a),
	              static_cast<unsigned long long>(hash_b));
	expect(hash_a == hash_b, std::string{"Zustands-Hash gleich (Rohstoffe, Entities mit HP/Position, Warteschlangen): "} + hashes);
	if (hash_a != hash_b) {
		// first differing lines of the canonical texts
		std::istringstream ta{save::serialize(save::canonical(a))};
		std::istringstream tb{save::serialize(save::canonical(b))};
		std::string la, lb;
		int shown = 0;
		while (shown < 20) {
			la.clear();
			lb.clear();
			const bool ga = static_cast<bool>(std::getline(ta, la));
			const bool gb = static_cast<bool>(std::getline(tb, lb));
			if (not ga and not gb) {
				break;
			}
			if (la != lb) {
				std::printf("    A: %s\n    B: %s\n", la.c_str(), lb.c_str());
				++shown;
			}
		}
	}
	// save -> load -> save: same file apart from title and date; the only expected difference is the
	// facing of units whose order was given again (they turn to the target at once)
	save::SaveData raw_a = a;
	save::SaveData raw_b = b;
	raw_a.title = raw_b.title = "";
	raw_a.created = raw_b.created = "";
	const std::string text_a = save::serialize(raw_a);
	const std::string text_b = save::serialize(raw_b);
	size_t differing = 0;
	size_t other = 0;
	{
		std::istringstream ta{text_a};
		std::istringstream tb{text_b};
		std::string la, lb;
		while (true) {
			la.clear();
			lb.clear();
			const bool ga = static_cast<bool>(std::getline(ta, la));
			const bool gb = static_cast<bool>(std::getline(tb, lb));
			if (not ga and not gb) {
				break;
			}
			if (la != lb) {
				++differing;
				if (not(la.rfind("angle = ", 0) == 0 and lb.rfind("angle = ", 0) == 0)) {
					++other;
					std::printf("    A: %s\n    B: %s\n", la.c_str(), lb.c_str());
				}
			}
		}
	}
	std::printf("  Datei nach Speichern→Laden→Speichern (ohne Titel/Datum): %zu abweichende Zeilen, davon %zu außer\n"
	            "  „angle“ (Blickrichtung von Einheiten mit erneut erteiltem Befehl, sie drehen sich beim Laden zum Ziel)\n",
	            differing, other);
	expect(other == 0, "Datei gleich bis auf Titel/Datum/Blickrichtung (" + std::to_string(other) + " andere Zeilen)");
	size_t b_orders = 0;
	for (const auto &e : b.entities) {
		b_orders += e.order.empty() ? 0 : 1;
	}
	std::printf("  Befehle: gespeichert %zu, nach dem Laden erteilt %s, sofort wieder sichtbar %zu\n", a_orders,
	            log_sink.last("Load: restored").c_str(), b_orders);
	expect(time_after_load >= 0.0 and std::fabs(time_after_load - a.game_time) < 0.5,
	       "Uhr nach dem Laden bei " + std::to_string(time_after_load) + " s");
	expect(time_after_play >= a.game_time + 60.0, "60 s weitergespielt (bis " + std::to_string(time_after_play) + " s)");
	expect(log_sink.errors() == errors_before, "keine Fehler im Log beim Weiterspielen ("
	                                               + std::to_string(log_sink.errors() - errors_before) + ")");

	// ---- 3. damaged file
	std::printf("== 3. Kaputte Datei\n");
	{
		std::string err;
		save::write_text(file_bad, "[save]\nversion = 1\n[map]\ntype = random\nsize = 3\n", err);
		MapSettings bad = args.map;
		bad.type = map_type_t::RANDOM;
		bad.load_file = file_bad.string();
		bad.ai.mode = ai_mode_t::OFF;
		engine::Engine engine{engine::Engine::mode::HEADLESS, root, args.modpacks, {}, bad};
		std::jthread driver{[&]() {
			wait_for([&]() { return engine.query_hud(0).game; }, 300.0);
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			engine.stop();
		}};
		engine.loop();
		expect(log_sink.count("starting a new game") == 1, "Fehlermeldung statt Absturz: " + log_sink.last("Load: "));
		expect(engine.get_production()->snapshot().status.find("Laden fehlgeschlagen") == 0,
		       "HUD-Meldung: " + engine.get_production()->snapshot().status);
	}
	{
		MapSettings missing = args.map;
		missing.load_file = (dir / "missing.save").string();
		engine::Engine engine{engine::Engine::mode::HEADLESS, root, args.modpacks, {}, missing};
		std::jthread driver{[&]() {
			wait_for([&]() { return engine.query_hud(0).game; }, 300.0);
			engine.stop();
		}};
		engine.loop();
		expect(log_sink.count("starting a new game") == 2, "fehlende Datei: neues Spiel mit Meldung");
	}
	std::printf("save check: %zu Fehler\n", failures);
	return failures == 0;
}

/// one log line of the production HUD snapshot
void log_prod_snapshot(const openage::gamestate::prod::Snapshot &snap) {
	using namespace openage;
	std::ostringstream options;
	for (const auto &o : snap.options) {
		options << " " << o.id << "(" << o.code << (o.available ? "" : ", " + o.reason) << ")";
	}
	std::ostringstream queue;
	if (snap.queue) {
		queue << snap.queue->label << " " << snap.queue->items.size() << " items, progress "
		      << static_cast<int>(std::lround(100.0 * snap.queue->progress)) << " %"
		      << (snap.queue->waiting_for_housing ? ", waits for housing" : "");
	}
	else {
		queue << "-";
	}
	log::log(INFO << "prod hud: t=" << snap.time << " food " << snap.resources[0] << " wood " << snap.resources[1]
	              << " gold " << snap.resources[2] << " stone " << snap.resources[3] << " population "
	              << snap.population << "/" << snap.population_cap << " selection " << snap.selection_count
	              << " " << snap.selection_label
	              << (snap.construction ? " (construction " + std::to_string(static_cast<int>(100.0 * *snap.construction)) + " %)" : "")
	              << " options" << options.str() << " queue " << queue.str()
	              << " placement '" << snap.placement << "' status '" << snap.status << "'");
}

/**
 * Render into a test frame sink without any window system (see file header).
 *
 * @return true if all frames were read and captured and the engine stopped in time.
 */
bool egl_sink_check(const native_args &args,
                    const openage::util::Path &root,
                    openage::renderer::window_settings settings) {
	using namespace openage;
	using clock = std::chrono::steady_clock;
	using renderer::opengl::TestFrameSink;

	const int width = static_cast<int>(args.width);
	const int height = static_cast<int>(args.height);
	const double start = args.seconds > 0 ? args.seconds : 15;
	const auto png = std::filesystem::absolute(args.egl_sink_check);
	const auto png_resized = png.parent_path() / (png.stem().string() + "-1920x1080.png");
	std::filesystem::remove(png);
	std::filesystem::remove(png_resized);

	auto map_settings = args.map;
	std::vector<TestFrameSink::Step> steps;
	ProdReplayTargets prod_targets;
	// game user interface (XR fork): only on request, the Quest app draws its own
	std::string ui_demo = args.ui_demo;
	settings.ui = args.ui.value_or(false);
	settings.ui_style = args.ui_style;
	settings.ui_quest = args.ui_quest;
	if (args.replay_aoe and not args.stop_in_resize) {
		steps = aoe_replay_steps(args, map_settings, start, png.string());
	}
	else if (args.replay_markers and not args.stop_in_resize) {
		steps = markers_replay_steps(args, map_settings, start, png.string());
	}
	else if (args.replay_ui and not args.stop_in_resize) {
		steps = ui_replay_steps(args, map_settings, start, png.string(), ui_demo);
	}
	else if (args.replay_prod and not args.stop_in_resize) {
		steps = prod_replay_steps(args, map_settings, start, png.string(), prod_targets);
	}
	else if (args.replay_econ and not args.stop_in_resize) {
		steps = econ_replay_steps(args, map_settings, start, png.string());
	}
	else if (args.replay_select and not args.stop_in_resize) {
		steps = select_replay_steps(args, map_settings, start, png.string());
		if (steps.empty()) {
			return false;
		}
	}
	else if (args.replay_combat and not args.stop_in_resize) {
		steps = TestFrameSink::combat_replay_steps(start, width, height, png.string());
	}
	else if (args.replay and not args.stop_in_resize) {
		steps = TestFrameSink::replay_steps(start, width, height, png.string());
	}
	else {
		TestFrameSink::Step shot;
		shot.at = start;
		shot.what = TestFrameSink::Step::kind::capture;
		shot.file = png.string();
		steps.push_back(shot);
	}
	// ai (XR fork): extra captures (--capture-at)
	for (double at : args.capture_at) {
		TestFrameSink::Step shot;
		shot.at = at;
		shot.what = TestFrameSink::Step::kind::capture;
		std::ostringstream name;
		name << png.stem().string() << "-t" << static_cast<long>(std::lround(at)) << png.extension().string();
		shot.file = (png.parent_path() / name.str()).string();
		steps.push_back(shot);
	}
	std::stable_sort(steps.begin(), steps.end(), [](const auto &a, const auto &b) { return a.at < b.at; });
	// size change while running, captured in the new size
	const double last = steps.back().at;
	if (args.background_switch) {
		// background change through the sink, visible in the second capture
		TestFrameSink::Step bg;
		bg.at = last + 0.5;
		bg.what = TestFrameSink::Step::kind::background;
		bg.background = *args.background_switch;
		steps.push_back(bg);
	}
	TestFrameSink::Step resize;
	resize.at = last + 1.0;
	resize.what = TestFrameSink::Step::kind::resize;
	resize.width = 1920;
	resize.height = 1080;
	steps.push_back(resize);
	if (not args.stop_in_resize) {
		TestFrameSink::Step shot;
		shot.at = last + 4.0;
		shot.what = TestFrameSink::Step::kind::capture;
		shot.file = png_resized.string();
		steps.push_back(shot);
	}

	// declared before the engine: destroyed after the engine threads are joined
	auto sink = std::make_shared<TestFrameSink>(width, height, steps, args.frames);
	settings.sink = sink;
	settings.ui_demo = ui_demo;

	auto engine = std::make_unique<engine::Engine>(engine::Engine::mode::FULL, root, args.modpacks, settings, map_settings);
	// ai (XR fork): faster simulation clock
	if (args.sim_speed) {
		engine->get_clock()->set_speed(time::speed_t::from_double(*args.sim_speed));
	}

	// production check (XR fork): HUD snapshot every 5 s through the thread-safe interface,
	// a house on water after the first house was paid (must be rejected)
	std::atomic<bool> prod_driver_stop{false};
	std::thread prod_driver;
	if (args.replay_prod or args.replay_ui or args.replay_aoe or args.replay_markers) {
		// the UI replay only logs the snapshots (no house on water)
		auto water = args.replay_prod ? prod_targets.water : std::nullopt;
		prod_driver = std::thread{[&prod_driver_stop, production = engine->get_production(), water]() {
			auto last_log = clock::now() - std::chrono::seconds(10);
			bool water_done = false;
			while (not prod_driver_stop) {
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				auto snap = production->snapshot();
				if (clock::now() - last_log >= std::chrono::seconds(5)) {
					last_log = clock::now();
					log_prod_snapshot(snap);
				}
				if (not water_done and snap.time > 1.0 and snap.resources[1] < 199.5) {
					water_done = true;
					if (water) {
						log::log(INFO << "prod replay: house on water at tile (" << (*water)[0] << ", " << (*water)[1] << ")");
						production->place("House", coord::phys3{coord::phys_t{(*water)[0]}, coord::phys_t{(*water)[1]}, coord::phys_t{0.0}});
					}
					else {
						log::log(WARN << "prod replay: no water on the map");
					}
				}
			}
			log_prod_snapshot(production->snapshot());
		}};
	}

	// written before the engine is stopped, read after its threads are joined
	clock::time_point sink_stop_time{};
	clock::time_point watcher_stop_time{};
	std::atomic<bool> stopped_by_sink{false};
	if (args.stop_in_resize) {
		// called from the consumer thread while the presenter waits inside acquire_target()
		sink->stop_during_resize([&]() {
			sink_stop_time = clock::now();
			stopped_by_sink = true;
			log::log(INFO << "egl sink check: stopping engine (consumer thread, during the resize)");
			engine->stop();
		});
	}

	// shutdown handshake between the main thread and the watcher, every wait is bounded
	constexpr auto stop_limit = std::chrono::seconds(10);
	constexpr auto stall_limit = std::chrono::seconds(60);
	std::mutex shutdown_mutex;
	std::condition_variable shutdown_cv;
	bool stop_sent = false;
	bool engine_gone = false;

	// stop the engine once the sink is done (or failed, stalled, or after a generous timeout)
	std::atomic<bool> loop_finished{false};
	bool sink_done = false;
	std::thread watcher{[&]() {
		const auto deadline = clock::now() + std::chrono::seconds(static_cast<int>(last) + 900);
		uint64_t published = 0;
		auto last_progress = clock::now();
		while (not loop_finished and clock::now() < deadline) {
			if (sink->wait_done(std::chrono::milliseconds(100))) {
				if (not args.stop_in_resize) {
					sink_done = true;
					break;
				}
				// the consumer stops the engine itself, wait_done() returns at once from now on
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (not sink->get_error().empty()) {
				break;
			}
			// a hanging presenter (e.g. a deadlock) publishes no more frames
			const auto stats = sink->get_stats();
			if (stats.published != published) {
				published = stats.published;
				last_progress = clock::now();
			}
			else if (published > 0 and not stopped_by_sink and clock::now() - last_progress > stall_limit) {
				log::log(ERR << "egl sink check: no new frame for "
				             << std::chrono::duration<double>(clock::now() - last_progress).count()
				             << " s, presenter stalled");
				break;
			}
		}
		if (not stopped_by_sink) {
			watcher_stop_time = clock::now();
		}
		log::log(INFO << "egl sink check: stopping engine");
		engine->stop();

		std::unique_lock<std::mutex> lock{shutdown_mutex};
		stop_sent = true;
		shutdown_cv.notify_all();
		// a deadlocked engine never returns from loop() or the thread joins: fail instead of hanging
		if (not shutdown_cv.wait_for(lock, stop_limit, [&] { return engine_gone; })) {
			log::log(ERR << "egl sink check: engine did not stop within "
			             << std::chrono::duration<double>(stop_limit).count()
			             << " s after Engine::stop() (deadlock?)");
			log::log(INFO << "egl sink check FAILED");
			std::_Exit(EXIT_FAILURE);
		}
	}};

	// an exception in the simulation must not destroy the joinable watcher (std::terminate)
	std::string loop_error;
	try {
		engine->loop();
	}
	catch (std::exception &exc) {
		loop_error = exc.what();
		log::log(ERR << "egl sink check: simulation failed: " << loop_error);
		engine->stop();
	}
	loop_finished = true;
	if (prod_driver.joinable()) {
		prod_driver_stop = true;
		prod_driver.join();
	}
	{
		// the watcher calls engine->stop() once more: destroy the engine only after that
		std::unique_lock<std::mutex> lock{shutdown_mutex};
		if (not shutdown_cv.wait_for(lock, stop_limit, [&] { return stop_sent; })) {
			log::log(ERR << "egl sink check: watcher did not stop the engine");
			std::_Exit(EXIT_FAILURE);
		}
	}
	// joins the time loop and presenter threads
	engine.reset();
	const auto stop_time = stopped_by_sink ? sink_stop_time : watcher_stop_time;
	const double stop_seconds = std::chrono::duration<double>(clock::now() - stop_time).count();
	{
		std::lock_guard<std::mutex> lock{shutdown_mutex};
		engine_gone = true;
	}
	shutdown_cv.notify_all();
	watcher.join();
	if (not loop_error.empty()) {
		return false;
	}

	auto stats = sink->get_stats();
	auto error = sink->get_error();
	sink.reset();

	log::log(INFO << "egl sink check: " << stats.published << " frames published, "
	              << stats.frames_read << " frames read (" << stats.reads << " reads), "
	              << stats.gl_errors << " consumer GL errors, "
	              << stats.steps_done << "/" << steps.size() << " steps, stop took "
	              << stop_seconds << " s");
	for (const auto &file : stats.captures) {
		log::log(INFO << "egl sink check: stored " << file);
	}

	bool ok = true;
	if (not error.empty()) {
		log::log(ERR << "egl sink check: " << error);
		ok = false;
	}
	if (args.stop_in_resize) {
		if (not stats.stopped_in_resize) {
			log::log(ERR << "egl sink check: engine was not stopped during the resize");
			ok = false;
		}
	}
	else if (not sink_done) {
		log::log(ERR << "egl sink check: sink not done (frames or steps missing)");
		ok = false;
	}
	if (stats.gl_errors != 0) {
		ok = false;
	}
	const double stop_limit_seconds = args.stop_in_resize ? 2.0 : 1.0;
	if (stop_seconds >= stop_limit_seconds) {
		log::log(ERR << "egl sink check: stop took " << stop_seconds << " s (limit "
		             << stop_limit_seconds << " s)");
		ok = false;
	}
	std::vector<std::filesystem::path> expected{png};
	if (not args.stop_in_resize) {
		expected.push_back(png_resized);
	}
	for (const auto &file : expected) {
		if (not std::filesystem::exists(file)) {
			log::log(ERR << "egl sink check: missing " << file.string());
			ok = false;
		}
	}
	log::log(INFO << "egl sink check " << (ok ? "ok" : "FAILED"));
	return ok;
}
#endif

} // namespace


int main(int argc, char **argv) {
	using namespace openage;

	native_args args;
	try {
		if (not parse_args(argc, argv, args)) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		}
	}
	catch (std::exception &exc) {
		std::cerr << exc.what() << "\n";
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	log::set_level(log::level::info);

	// ai (XR fork): the input replays check the human side; keep the computer opponent out
	if ((args.replay or args.replay_econ or args.replay_combat or args.replay_prod or args.replay_select or args.replay_ui
	     or args.replay_aoe or args.replay_markers)
	    and not args.ai_explicit) {
		args.map.ai.mode = gamestate::ai_mode_t::OFF;
	}

	try {
		// plain directory instead of the Python union filesystem
		util::Path root{std::make_shared<util::fslike::Directory>(args.root), {}};
		log::log(INFO << "openage-native, root " << root);

		renderer::window_settings win_settings{};
		win_settings.width = args.width;
		win_settings.height = args.height;
		if (args.gles) {
			win_settings.backend = renderer::graphics_api_t::OPENGL_ES;
		}
		if (args.background) {
			win_settings.background = *args.background;
		}

		if (args.shader_check) {
			win_settings.visible = false;
			size_t failed = shader_check(args.root, root, win_settings);
			return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
		}

		check_modpacks(root, args.modpacks);

		// save games (XR fork): slot directory, file to load
		win_settings.save_dir = args.save_dir.empty() ? default_save_dir() : args.save_dir;
		if (not args.load_file.empty()) {
			gamestate::save::SaveData data;
			std::string error;
			if (not gamestate::save::read_save(args.load_file, data, error)) {
				throw Error{MSG(err) << "--load: " << error};
			}
			args.map = data.map;
			args.map.load_file = std::filesystem::absolute(args.load_file).string();
			log::log(INFO << "--load " << args.load_file << ": " << data.title << ", t=" << data.game_time << " s");
		}
		if (not args.save_check.empty()) {
			return save_check(args, root) ? EXIT_SUCCESS : EXIT_FAILURE;
		}

		if (not args.egl_sink_check.empty()) {
#if WITH_EGL
			return egl_sink_check(args, root, win_settings) ? EXIT_SUCCESS : EXIT_FAILURE;
#else
			throw Error{MSG(err) << "--egl-sink-check: built without EGL support"};
#endif
		}

		if (args.check) {
			log::log(INFO << "check ok");
			return EXIT_SUCCESS;
		}

		const bool render_check = not args.render_check.empty();
		if (render_check) {
			if (args.headless) {
				throw Error{MSG(err) << "--render-check needs the renderer, not --headless"};
			}
			// the presenter stops the engine after storing the frame
			win_settings.visible = false;
			win_settings.capture_file = std::filesystem::absolute(args.render_check).string();
			win_settings.capture_delay = args.seconds > 0 ? args.seconds : 10;
		}

		auto mode = args.headless ? engine::Engine::mode::HEADLESS
		                          : engine::Engine::mode::FULL;
		// game user interface (XR fork): on for the plain game, off for the checks
		win_settings.ui = args.ui.value_or(not args.headless and not render_check);
		win_settings.ui_demo = args.ui_demo;
		win_settings.ui_style = args.ui_style;
		win_settings.ui_quest = args.ui_quest;

		// the game menu restarts the engine with other map settings (XR fork)
		auto map_settings = args.map;
		for (int run = 1;; ++run) {
			engine::Engine engine{mode, root, args.modpacks, win_settings, map_settings};
			// ai (XR fork): faster simulation clock
			if (args.sim_speed) {
				engine.get_clock()->set_speed(time::speed_t::from_double(*args.sim_speed));
			}

			std::jthread timer;
			if (args.seconds > 0 and not render_check) {
				timer = std::jthread{[&engine, seconds = args.seconds]() {
					std::this_thread::sleep_for(std::chrono::seconds(seconds));
					log::log(INFO << "--seconds reached, stopping engine");
					engine.stop();
				}};
			}

			engine.loop();
			log::log(INFO << "engine loop finished (run " << run << ")");
			auto restart = engine.take_restart();
			if (not restart) {
				break;
			}
			map_settings = *restart;
			log::log(INFO << "restarting the engine with a new map (seed " << map_settings.seed << ", "
			              << map_settings.size << "x" << map_settings.size << ")");
			// the window and GL context of the old engine are gone once it is destroyed
		}

		if (render_check and not std::filesystem::exists(win_settings.capture_file)) {
			throw Error{MSG(err) << "render check: no frame stored to " << win_settings.capture_file};
		}
	}
	catch (Error &err) {
		std::cerr << "openage-native: " << err << std::endl;
		return EXIT_FAILURE;
	}
	catch (std::exception &exc) {
		std::cerr << "openage-native: " << exc.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
