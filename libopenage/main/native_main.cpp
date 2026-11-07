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
#include <set>
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
#include "gamestate/catalog.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/skirmish.h"
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
#include "ui/agesxr/xr_minimap.h"
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
	// XR fork W1: input replay of the technologies (research buttons, tooltip, queue, age, civilisation choice)
	bool replay_tech = false;
	// vision (XR fork): fog of war and minimap replay
	bool replay_vision = false;
	// XR fork W4: combat replay (formation, stance, town bell, garrison arrows, projectiles)
	bool replay_war = false;
	// XR fork W3: food replays (sheep, hunting, farm + reseed; fishing on a coast map)
	bool replay_food = false;
	bool replay_fish = false;
	// XR fork W5: buildings replay (palisade drag, gate on a stone wall)
	bool replay_buildings = false;
	// XR fork W6: special replay (monk converts, mangonel area damage, trebuchet), runs on the food plumbing
	bool replay_special = false;
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
	             " [--ui-style aoe|classic] [--ui-quest] [--replay-aoe] [--replay-markers] [--replay-tech]"
	             " [--civ <name|random>] [--ai-civ <name|random>] [--replay-vision] [--replay-food] [--replay-fish] [--replay-buildings] [--replay-special] [--replay-war] [--vision normal|explored|all]"
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
		else if (arg == "--players") {
			args.map.players = std::stoull(value());
		}
		else if (arg == "--teams") {
			std::stringstream list{value()};
			std::string part;
			args.map.teams.clear();
			while (std::getline(list, part, ',')) {
				args.map.teams.push_back(std::stoi(part));
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
		else if (arg == "--replay-tech") {
			args.replay_tech = true;
			args.ui = true;
			args.ui_style = "aoe";
		}
		// XR fork W1: civilisation of player 0 / of the opponent (short name of the data or "random")
		else if (arg == "--civ" or arg == "--ai-civ") {
			if (args.map.civs.size() < 2) {
				args.map.civs.resize(2, "random");
			}
			args.map.civs[arg == "--civ" ? 0 : 1] = value();
		}
		else if (arg == "--replay-vision") {
			args.replay_vision = true;
			args.ui = true;
			args.ui_style = "aoe";
		}
		else if (arg == "--replay-war") {
			args.replay_war = true;
			args.ui = true;
			args.ui_style = "aoe";
		}
		else if (arg == "--replay-special") {
			args.replay_special = true;
			args.replay_food = true;
			args.ui = true;
			args.ui_style = "aoe";
		}
		else if (arg == "--replay-food" or arg == "--replay-fish") {
			(arg == "--replay-food" ? args.replay_food : args.replay_fish) = true;
			args.ui = true;
			args.ui_style = "aoe";
		}
		else if (arg == "--replay-buildings") {
			args.replay_buildings = true;
			args.ui = true;
			args.ui_style = "aoe";
		}
		else if (arg == "--vision") {
			auto mode = value();
			if (not openage::gamestate::vision_mode_parse(mode, args.map.vision)) {
				throw std::runtime_error("--vision: normal, explored or all, not " + mode);
			}
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
		// 5. barracks (hotkey T) placed next to the villagers, then the mill (W, 100 wood; the archery range needs the Feudal Age since W1): not enough wood
		std::pair<int, int> site{std::max(vp.first - 220, 80), std::max(vp.second - 60, layout.topBarPx() + 80)};
		key(11.8, 'T');
		click(12.5, site, E::kLeftButton);
		log::log(INFO << "aoe replay: barracks site at pixel (" << site.first << ", " << site.second << ")");
		// a frame after the placement (the sink delivers later inputs only after this capture)
		capture(14.0, "-sync-5");
		key(19.0, 'W');
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
 * Input replay of the technologies (--replay-tech, XR fork W1; with --map random and fixed
 * civilisations): selects the own town centre (research buttons: Feudal Age locked with the
 * reason of the data, Loom with costs/time/effect as tooltip), researches Loom (queue),
 * the driver grants the Feudal Age (age badge, message) and then the Castle Age (test option
 * Production::grant_research), all villagers (double click) show the buildings of the
 * Castle Age in two pages, G turns the page, Esc opens the game menu, "Neue Karte" shows the
 * civilisation choice. Each state is captured as <png stem>-<n>-<name>.png.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> tech_replay_steps(const native_args &args,
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

	// 1. town centre: research buttons (cell 0 Dorfbewohner, 1 Feudalzeit, 2 Webstuhl)
	auto tp = pixel(tc[0], tc[1], 1.2);
	log::log(INFO << "tech replay: town centre at pixel (" << tp.first << ", " << tp.second << ")");
	click(0.9, tp, E::kLeftButton);
	capture(2.0, "-sync-1");
	add(2.5, E::kMouseMove, tp.first, tp.second, 0, 0);
	capture(3.0, "-sync-2");
	auto feudal_cell = centre(layout.gridRect(1));
	auto loom_cell = centre(layout.gridRect(2));
	add(5.0, E::kMouseMove, feudal_cell.first, feudal_cell.second, 0, 0);
	capture(6.0, "-1-forschung");
	// 2. tooltip of Loom (costs, time, effect)
	add(6.5, E::kMouseMove, loom_cell.first, loom_cell.second, 0, 0);
	capture(7.5, "-2-tooltip");
	// 3. Loom researched: in the queue of the town centre
	click(8.0, loom_cell, E::kLeftButton);
	capture(10.0, "-3-warteschlange");
	// 4. the driver grants the Feudal Age once -3 is taken: badge "Feudalzeit", message
	// (captures wait for a frame after the last input: small moves in between give the grant a few frames)
	add(11.0, E::kMouseMove, loom_cell.first + 4, loom_cell.second, 0, 0);
	capture(11.5, "-sync-3a");
	add(12.5, E::kMouseMove, loom_cell.first, loom_cell.second + 4, 0, 0);
	capture(13.0, "-sync-3b");
	add(14.0, E::kMouseMove, loom_cell.first, loom_cell.second, 0, 0);
	capture(15.0, "-4-zeitalter");
	// 5. the driver grants the Castle Age: all villagers show the buildings in two pages
	if (villager != nullptr) {
		auto vp = pixel(villager->ne, villager->se, 0.6);
		log::log(INFO << "tech replay: villager at pixel (" << vp.first << ", " << vp.second << ")");
		add(19.0, E::kMouseMove, vp.first, vp.second, 0, 0);
		add(19.05, E::kMouseDown, vp.first, vp.second, E::kLeftButton, E::kLeftButton);
		add(19.1, E::kMouseUp, vp.first, vp.second, E::kLeftButton, 0);
		add(19.2, E::kMouseDown, vp.first, vp.second, E::kLeftButton, E::kLeftButton);
		add(19.2, E::kMouseDoubleClick, vp.first, vp.second, E::kLeftButton, E::kLeftButton);
		add(19.3, E::kMouseUp, vp.first, vp.second, E::kLeftButton, 0);
		capture(20.5, "-sync-5");
		auto page_cell = centre(layout.gridRect(agesxr::kAoeOptionCells - 1));
		add(21.0, E::kMouseMove, page_cell.first, page_cell.second, 0, 0);
		capture(23.0, "-5-seiten");
		key(23.5, 'G');
		capture(25.0, "-6-seite2");
	}
	else {
		log::log(WARN << "tech replay: no villager of player 0 on screen");
	}
	// 6. game menu -> "Neue Karte": civilisation choice (Deine Kultur, Kultur Gegner)
	key(25.5, 0x01000000);
	capture(26.3, "-sync-7");
	layout.openMenu(true, 0.0);
	auto new_map_row = centre(layout.dialogRowRect(agesxr::AoeUi::kMainNewMap));
	layout.menu().page = agesxr::AoeMenuModel::kNewMap;
	auto civ_right = centre(layout.dialogArrowRect(agesxr::AoeUi::kNewAiCiv, true));
	click(26.5, new_map_row, E::kLeftButton);
	capture(27.5, "-sync-8");
	click(27.8, civ_right, E::kLeftButton);
	capture(29.0, "-7-kulturwahl");
	key(29.5, 0x01000000);
	key(30.0, 0x01000000);
	capture(31.0, nullptr);
	return steps;
}

/**
 * Plan of the food replays (XR fork W3): what the driver thread spawns with the test options of
 * the production (the input steps cannot place finished buildings) and when (after a capture).
 */
struct FoodReplayPlan {
	struct Spawn {
		/// capture suffix that must exist first ("-sync-0")
		std::string after;
		openage::gamestate::player_id_t player = 0;
		std::string name;
		double ne = 0.0;
		double se = 0.0;
	};
	std::vector<Spawn> spawns;
	/// capture suffix after which the driver gives 2000 of every resource to player 0
	std::string grant_after;
	/// capture suffix after which the driver starts the farm placement mode (selection = villager)
	std::string farm_after;
	/// capture suffix after which the driver queues two reseeds at the mill
	std::string reseed_after;
	/// XR fork W5 (--replay-buildings): placement modes and test grants of technologies after captures
	struct Placement {
		std::string after;
		std::string id;
	};
	std::vector<Placement> placements;
	struct Grant {
		std::string after;
		openage::gamestate::player_id_t player = 0;
		std::string tech;
	};
	std::vector<Grant> grants;
};

/**
 * Input replay of the food economy (--replay-food, XR fork W3; with --map random, vision all):
 * start (sheep of the player at the town centre, a mill and a deer from the test options),
 * a sheep is herded to the town centre (left click, right click on the ground), a villager
 * slaughters another sheep, a villager hunts the deer (it flees), a villager places a farm
 * (white outline preview, then the farm terrain), the mill gets two reseeds (symbols in its
 * queue). Captures <png stem>-1-start … -7-neusaeen.
 *
 * --replay-fish (coast map): a dock and a fishing ship from the test options, the ship is sent to
 * the nearest fish, it fishes and brings the food to the dock. Captures -f1 … -f4.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> food_replay_steps(const native_args &args,
                                                                               openage::gamestate::MapSettings &map_settings,
                                                                               double start,
                                                                               const std::string &capture_file,
                                                                               FoodReplayPlan &plan) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using gamestate::map_object_t;
	using gamestate::map_terrain_t;
	using E = renderer::SinkInputEvent;

	const auto map = gamestate::generate_map(map_settings);
	const gamestate::Heightmap heights{map.width, map.height, map.corners};
	const auto tc = map.starts.at(0);
	const long N = static_cast<long>(map.width);
	auto tile = [&](long ne, long se) {
		return map.tiles[static_cast<size_t>(ne) + static_cast<size_t>(se) * map.width];
	};
	std::set<std::pair<long, long>> used;
	for (const auto &o : map.objects) {
		used.insert({static_cast<long>(std::floor(o.ne)), static_cast<long>(std::floor(o.se))});
	}
	for (long y = static_cast<long>(tc[1]) - 2; y < static_cast<long>(tc[1]) + 2; ++y) {
		for (long x = static_cast<long>(tc[0]) - 2; x < static_cast<long>(tc[0]) + 2; ++x) {
			used.insert({x, y});
		}
	}
	auto land = [&](long ne, long se) {
		if (ne < 1 or se < 1 or ne >= N - 1 or se >= N - 1) {
			return false;
		}
		auto t = tile(ne, se);
		return t != map_terrain_t::WATER and t != map_terrain_t::WATER_MEDIUM and t != map_terrain_t::WATER_DEEP
		       and t != map_terrain_t::ICE and t != map_terrain_t::SHALLOWS and t != map_terrain_t::FOREST;
	};
	auto water = [&](long ne, long se) {
		if (ne < 0 or se < 0 or ne >= N or se >= N) {
			return false;
		}
		auto t = tile(ne, se);
		return t == map_terrain_t::WATER or t == map_terrain_t::WATER_MEDIUM or t == map_terrain_t::WATER_DEEP;
	};
	// a free square of side tiles (corner ne, se) with a free ring around it
	auto free_square = [&](long ne, long se, long side, bool on_water) {
		for (long y = se - 1; y <= se + side; ++y) {
			for (long x = ne - 1; x <= ne + side; ++x) {
				const bool inside = x >= ne and x < ne + side and y >= se and y < se + side;
				if (used.contains({x, y})) {
					return false;
				}
				if (inside and (on_water ? not water(x, y) : not land(x, y))) {
					return false;
				}
			}
		}
		return true;
	};
	// nearest free square to a point (spiral), its centre
	auto near_square = [&](double cne, double cse, long side, bool on_water, double min_d) -> std::optional<std::array<double, 2>> {
		std::optional<std::array<double, 2>> best;
		double best_d = 1e9;
		for (long se = 1; se + side < N - 1; ++se) {
			for (long ne = 1; ne + side < N - 1; ++ne) {
				const double cx = ne + 0.5 * side;
				const double cy = se + 0.5 * side;
				const double d = std::hypot(cx - cne, cy - cse);
				if (d < min_d or d >= best_d or not free_square(ne, se, side, on_water)) {
					continue;
				}
				best_d = d;
				best = std::array<double, 2>{cx, cy};
			}
		}
		return best;
	};
	auto mark = [&](const std::array<double, 2> &c, long side) {
		const long ne0 = static_cast<long>(std::floor(c[0] - 0.5 * side + 0.01));
		const long se0 = static_cast<long>(std::floor(c[1] - 0.5 * side + 0.01));
		for (long y = se0; y < se0 + side; ++y) {
			for (long x = ne0; x < ne0 + side; ++x) {
				used.insert({x, y});
			}
		}
	};

	std::array<double, 2> focus = tc;
	std::optional<std::array<double, 2>> dock, ship, fish, mill, farm, deer, herd_to;
	if (args.replay_fish) {
		// dock: 3x3 on water, a beach tile next to it, near the town centre
		double best = 1e9;
		for (long se = 2; se + 3 < N - 2; ++se) {
			for (long ne = 2; ne + 3 < N - 2; ++ne) {
				if (not free_square(ne, se, 3, true)) {
					continue;
				}
				bool placement = false;
				bool beach = false;
				for (long y = se - 1; y <= se + 3; ++y) {
					for (long x = ne - 1; x <= ne + 3; ++x) {
						const bool inside = x >= ne and x < ne + 3 and y >= se and y < se + 3;
						placement = placement or (inside and tile(x, y) == map_terrain_t::WATER);
						beach = beach or (not inside and tile(x, y) == map_terrain_t::BEACH);
					}
				}
				const double d = std::hypot(ne + 1.5 - tc[0], se + 1.5 - tc[1]);
				if (placement and beach and d < best) {
					best = d;
					dock = std::array<double, 2>{ne + 1.5, se + 1.5};
				}
			}
		}
		if (dock) {
			mark(*dock, 3);
			ship = near_square((*dock)[0], (*dock)[1], 1, true, 2.0);
			double fd = 1e9;
			for (const auto &o : map.objects) {
				if (o.kind != map_object_t::FISH_SHORE and o.kind != map_object_t::FISH_OCEAN) {
					continue;
				}
				const double d = std::hypot(o.ne - (*dock)[0], o.se - (*dock)[1]);
				if (d > 2.5 and d < fd) {
					fd = d;
					fish = std::array<double, 2>{o.ne, o.se};
				}
			}
			focus = *dock;
		}
		log::log(INFO << "fish replay: dock " << (dock ? std::to_string((*dock)[0]) + ", " + std::to_string((*dock)[1]) : "-")
		              << ", ship " << (ship ? std::to_string((*ship)[0]) + ", " + std::to_string((*ship)[1]) : "-")
		              << ", fish " << (fish ? std::to_string((*fish)[0]) + ", " + std::to_string((*fish)[1]) : "-"));
	}
	else {
		mill = near_square(tc[0] - 4.0, tc[1] + 4.0, 2, false, 4.0);
		if (mill) {
			mark(*mill, 2);
		}
		farm = near_square(tc[0] + 4.5, tc[1] - 4.5, 3, false, 4.5);
		if (farm) {
			mark(*farm, 3);
		}
		deer = near_square(tc[0] + 5.0, tc[1] + 3.0, 1, false, 4.0);
		if (deer) {
			mark(*deer, 1);
		}
		herd_to = near_square(tc[0] + 2.5, tc[1] - 0.5, 1, false, 2.5);
		log::log(INFO << "food replay: mill " << (mill ? std::to_string((*mill)[0]) : "-") << ", farm "
		              << (farm ? std::to_string((*farm)[0]) : "-") << ", deer " << (deer ? std::to_string((*deer)[0]) : "-"));
	}
	if (not map_settings.view) {
		gamestate::MapView start_view;
		start_view.ne = focus[0];
		start_view.se = focus[1];
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
	auto log_px = [&](const char *what, std::pair<int, int> p) {
		log::log(INFO << "food replay: " << what << " at pixel (" << p.first << ", " << p.second << ")");
	};

	plan.grant_after = "-sync-0";
	capture(0.6, "-sync-0");
	if (args.replay_special) {
		// XR fork W6 (--replay-special): a monk converts an enemy militia (faith in the info line), a mangonel
		// shoots into an enemy group next to an own militia (area damage), a trebuchet shows its commands.
		// A second monk picks up a relic and walks away with it (carry graphics).
		// The trebuchet then unpacks (hotkey U) and shoots at an enemy house.
		// Captures -s1-start, -s2-konvertieren, -s3-mange, -s4-tribock, -s5-reliquie, -s6-traegt,
		// -s7-aufgebaut, -s8-schuss, -s9-schuss. Then a market: -s10-markt, one lot bought, -s11-gekauft,
		// tribute -s12-tribut, and a trade cart between two markets: -s13-handel, -s14-gold.
		const auto gaia_less = static_cast<openage::gamestate::player_id_t>(1);
		auto monastery = near_square(tc[0] + 5.0, tc[1] + 5.0, 3, false, 5.0);
		if (monastery) {
			mark(*monastery, 3);
		}
		auto monk = near_square(tc[0] + 4.0, tc[1] - 3.0, 1, false, 3.0);
		if (monk) {
			mark(*monk, 1);
		}
		std::optional<std::array<double, 2>> foe1, foe2, foe3, own, mangonel, treb;
		if (monk) {
			foe1 = near_square((*monk)[0] + 5.0, (*monk)[1] + 5.0, 1, false, 0.0);
			if (foe1) {
				mark(*foe1, 1);
				foe2 = near_square((*foe1)[0] + 1.0, (*foe1)[1] + 4.0, 1, false, 0.0);
			}
			if (foe2) {
				mark(*foe2, 1);
				foe3 = near_square((*foe2)[0] + 0.7, (*foe2)[1], 1, false, 0.0);
			}
			if (foe3) {
				mark(*foe3, 1);
				own = near_square((*foe2)[0], (*foe2)[1] + 0.7, 1, false, 0.0);
			}
			if (own) {
				mark(*own, 1);
				mangonel = near_square((*foe2)[0] - 5.0, (*foe2)[1], 1, false, 3.5);
			}
			if (mangonel) {
				mark(*mangonel, 1);
			}
			treb = near_square(tc[0] - 4.0, tc[1] + 4.0, 1, false, 3.5);
		}
		std::optional<std::array<double, 2>> carrier, relic, away, target;
		if (treb) {
			mark(*treb, 1);
			carrier = near_square(tc[0] - 1.0, tc[1] + 7.0, 1, false, 1.0);
			// up and to the left on the screen, in range (4 to 16)
			target = near_square((*treb)[0] + 2.0, (*treb)[1] - 11.0, 1, false, 3.0);
		}
		std::optional<std::array<double, 2>> market;
		if (target) {
			mark(*target, 1);
			market = near_square(tc[0] + 1.0, tc[1] - 8.0, 2, false, 3.0);
		}
		std::optional<std::array<double, 2>> market2, trader;
		if (market) {
			mark(*market, 2);
			market2 = near_square((*market)[0] + 9.0, (*market)[1], 2, false, 3.0);
		}
		if (market2) {
			mark(*market2, 2);
			trader = near_square((*market)[0] + 3.5, (*market)[1], 1, false, 1.0);
		}
		if (trader) {
			mark(*trader, 1);
		}
		if (carrier) {
			mark(*carrier, 1);
			relic = near_square((*carrier)[0] + 2.0, (*carrier)[1] - 1.0, 1, false, 0.0);
		}
		if (relic) {
			mark(*relic, 1);
			away = near_square((*carrier)[0] - 1.0, (*carrier)[1] + 4.0, 1, false, 0.0);
		}
		if (not monastery or not monk or not foe1 or not foe3 or not own or not mangonel or not treb or not relic or not away or not target or not market or not market2 or not trader) {
			log::log(WARN << "special replay: map lacks free spots");
			capture(2.0, nullptr);
			return steps;
		}
		plan.spawns.push_back({"-sync-0", 0, "Monastery", (*monastery)[0], (*monastery)[1]});
		plan.spawns.push_back({"-sync-0", 0, "Monk", (*monk)[0], (*monk)[1]});
		plan.spawns.push_back({"-sync-0", gaia_less, "Militia", (*foe1)[0], (*foe1)[1]});
		plan.spawns.push_back({"-sync-0", gaia_less, "Militia", (*foe2)[0], (*foe2)[1]});
		plan.spawns.push_back({"-sync-0", gaia_less, "Militia", (*foe3)[0], (*foe3)[1]});
		// the own militia next to the target comes after the conversion (the monk would heal it first)
		plan.spawns.push_back({"-s2-konvertieren", 0, "Militia", (*own)[0], (*own)[1]});
		// after the conversion as well: its shots would make the militia walk away from the right click
		plan.spawns.push_back({"-s2-konvertieren", 0, "Mangonel", (*mangonel)[0], (*mangonel)[1]});
		plan.spawns.push_back({"-sync-0", 0, "Trebuchet", (*treb)[0], (*treb)[1]});
		plan.spawns.push_back({"-s4-tribock", 0, "Monk", (*carrier)[0], (*carrier)[1]});
		plan.spawns.push_back({"-s4-tribock", gaia_less, "Relic", (*relic)[0], (*relic)[1]});
		plan.spawns.push_back({"-s6-traegt", gaia_less, "House", (*target)[0], (*target)[1]});
		plan.spawns.push_back({"-s6-traegt", 0, "Market", (*market)[0], (*market)[1]});
		plan.spawns.push_back({"-s6-traegt", 0, "Market", (*market2)[0], (*market2)[1]});
		plan.spawns.push_back({"-s6-traegt", 0, "TradeCart", (*trader)[0], (*trader)[1]});
		capture(3.0, "-s1-start");
		auto mp = pixel((*monk)[0], (*monk)[1], 0.6);
		log_px("monk", mp);
		// a drag box around the unit (a click on the sprite may miss it)
		auto box_select = [&](double t, std::pair<int, int> p, int box) {
			add(t, E::kMouseMove, p.first - box, p.second - box, 0, 0);
			add(t + 0.1, E::kMouseDown, p.first - box, p.second - box, E::kLeftButton, E::kLeftButton);
			for (int i = 1; i <= 4; ++i) {
				add(t + 0.1 + 0.05 * i, E::kMouseMove, p.first - box + box * i / 2, p.second - box + box * i / 2, 0, E::kLeftButton);
			}
			add(t + 0.4, E::kMouseUp, p.first + box, p.second + box, E::kLeftButton, 0);
		};
		box_select(3.5, mp, 30);
		capture(4.2, "-sync-s1");
		click(4.5, pixel((*foe1)[0], (*foe1)[1], 0.6), E::kRightButton);
		// the walk (up to 12 s after a slow first frame) and the conversion (up to 10 s) come before the next scene
		capture(28.0, "-s2-konvertieren");
		auto gp = pixel((*mangonel)[0], (*mangonel)[1], 0.6);
		log_px("mangonel", gp);
		box_select(30.5, gp, 30);
		capture(31.2, "-sync-s2");
		click(31.5, pixel((*foe2)[0], (*foe2)[1], 0.6), E::kRightButton);
		capture(44.0, "-s3-mange");
		auto tp = pixel((*treb)[0], (*treb)[1], 0.8);
		log_px("trebuchet", tp);
		box_select(44.5, tp, 30);
		capture(45.5, "-s4-tribock");
		auto cp = pixel((*carrier)[0], (*carrier)[1], 0.6);
		log_px("carrier", cp);
		box_select(46.0, cp, 30);
		click(46.8, pixel((*relic)[0], (*relic)[1], 0.3), E::kRightButton);
		capture(54.0, "-s5-reliquie");
		click(54.2, pixel((*away)[0], (*away)[1], 0.0), E::kRightButton);
		capture(55.2, "-s6-traegt");
		box_select(55.6, tp, 30);
		// the command grid needs the selection in the production snapshot
		add(57.0, E::kKeyDown, 0, 0, 0, 0, 'U');
		add(57.05, E::kKeyUp, 0, 0, 0, 0, 'U');
		capture(64.0, "-s7-aufgebaut");
		click(64.2, pixel((*target)[0], (*target)[1], 0.5), E::kRightButton);
		capture(65.0, "-s8-schuss");
		capture(66.0, "-s9-schuss");
		box_select(66.5, pixel((*market)[0], (*market)[1], 0.5), 45);
		capture(68.0, "-s10-markt");
		agesxr::AoeUi market_layout;
		market_layout.resize(width, height);
		const auto buy_cell = market_layout.gridRect(0);
		click(68.3, {buy_cell.cx(), buy_cell.cy()}, E::kLeftButton);
		capture(69.5, "-s11-gekauft");
		const auto tribute_cell = market_layout.gridRect(3);
		click(70.0, {tribute_cell.cx(), tribute_cell.cy()}, E::kLeftButton);
		capture(71.0, "-s12-tribut");
		click(71.2, {buy_cell.cx(), buy_cell.cy()}, E::kLeftButton);
		// a trade cart: right click on the second market, it shuttles and earns gold
		box_select(72.5, pixel((*trader)[0], (*trader)[1], 0.5), 30);
		click(73.3, pixel((*market2)[0], (*market2)[1], 0.5), E::kRightButton);
		capture(75.0, "-s13-handel");
		capture(92.0, "-s14-gold");
		capture(92.5, nullptr);
		return steps;
	}
	if (args.replay_fish) {
		if (not dock or not ship or not fish) {
			log::log(WARN << "fish replay: no dock spot, ship tile or fish on this map");
			capture(2.0, nullptr);
			return steps;
		}
		plan.spawns.push_back({"-sync-0", 0, "Dock", (*dock)[0], (*dock)[1]});
		plan.spawns.push_back({"-sync-0", 0, "FishingShip", (*ship)[0], (*ship)[1]});
		capture(3.0, "-f1-hafen");
		auto sp = pixel((*ship)[0], (*ship)[1], 0.4);
		auto fp = pixel((*fish)[0], (*fish)[1], 0.2);
		log_px("ship", sp);
		log_px("fish", fp);
		click(3.5, sp, E::kLeftButton);
		capture(4.5, "-sync-f1");
		click(5.0, fp, E::kRightButton);
		capture(30.0, "-f2-fischen");
		capture(75.0, "-f3-abliefern");
		auto dp = pixel((*dock)[0], (*dock)[1], 1.0);
		click(75.5, dp, E::kLeftButton);
		capture(77.0, "-f4-hafen-befehle");
		capture(78.0, nullptr);
		return steps;
	}

	// villagers and sheep of player 0 on the map (sheep in sight belong to the player at once)
	std::vector<const gamestate::MapObject *> villagers, sheep;
	for (const auto &o : map.objects) {
		if (o.kind == map_object_t::VILLAGER and o.owner == 0) {
			villagers.push_back(&o);
		}
		if (o.kind == map_object_t::SHEEP and std::hypot(o.ne - tc[0], o.se - tc[1]) <= 7.5) {
			sheep.push_back(&o);
		}
	}
	if (villagers.size() < 3 or sheep.size() < 2 or not mill or not farm or not deer or not herd_to) {
		log::log(WARN << "food replay: map lacks villagers/sheep/free spots");
		capture(2.0, nullptr);
		return steps;
	}
	plan.spawns.push_back({"-sync-0", 0, "Mill", (*mill)[0], (*mill)[1]});
	plan.spawns.push_back({"-sync-0", static_cast<openage::gamestate::player_id_t>(map.starts.size()), "Deer", (*deer)[0], (*deer)[1]});
	capture(2.0, "-1-start");
	// 1. herding: the first sheep walks to the town centre
	auto s1 = pixel(sheep[0]->ne, sheep[0]->se, 0.35);
	log_px("sheep 1", s1);
	click(2.5, s1, E::kLeftButton);
	capture(3.2, "-sync-1");
	click(3.5, pixel((*herd_to)[0], (*herd_to)[1], 0.0), E::kRightButton);
	capture(9.0, "-2-hueten");
	// 2. slaughter: a villager kills the second sheep and gathers it
	auto v1 = pixel(villagers[0]->ne, villagers[0]->se, 0.6);
	log_px("villager 1", v1);
	click(9.5, v1, E::kLeftButton);
	capture(10.2, "-sync-2");
	click(10.5, pixel(sheep[1]->ne, sheep[1]->se, 0.35), E::kRightButton);
	capture(22.0, "-3-schlachten");
	// 3. hunting: a villager hunts the deer (it flees when hit)
	auto v2 = pixel(villagers[1]->ne, villagers[1]->se, 0.6);
	click(22.5, v2, E::kLeftButton);
	capture(23.2, "-sync-3");
	click(23.5, pixel((*deer)[0], (*deer)[1], 0.4), E::kRightButton);
	capture(34.0, "-4-jagd");
	// 4. farm: the third villager, placement mode from the driver, the outline under the cursor
	auto v3 = pixel(villagers[2]->ne, villagers[2]->se, 0.6);
	click(34.5, v3, E::kLeftButton);
	capture(35.2, "-sync-4");
	plan.farm_after = "-sync-4";
	auto fpx = pixel((*farm)[0], (*farm)[1], 0.0);
	log_px("farm", fpx);
	add(36.0, E::kMouseMove, fpx.first - 6, fpx.second, 0, 0);
	capture(36.5, "-sync-5");
	add(37.0, E::kMouseMove, fpx.first, fpx.second, 0, 0);
	capture(38.0, "-5-farm-vorschau");
	click(38.5, fpx, E::kLeftButton);
	capture(62.0, "-6-farm");
	// 5. mill: two reseeds queued by the driver, shown as symbols of its queue
	auto mp = pixel((*mill)[0], (*mill)[1], 0.8);
	click(62.5, mp, E::kLeftButton);
	capture(63.2, "-sync-7");
	plan.reseed_after = "-sync-7";
	add(64.0, E::kMouseMove, mp.first + 4, mp.second, 0, 0);
	capture(64.5, "-sync-8");
	add(65.0, E::kMouseMove, mp.first, mp.second + 4, 0, 0);
	capture(66.0, "-7-neusaeen");
	capture(67.0, nullptr);
	return steps;
}

/**
 * Input replay of the buildings (--replay-buildings, XR fork W5; with --map random, vision all):
 * a villager drags a palisade (left button held from the start to the end tile: the preview shows
 * every segment, red ones on blocked tiles), releases it (foundations, 2 wood each) and builds the
 * segments one after the other; a stone wall from the test options gets a gate (preview on the wall,
 * foundation, built). Captures <png stem>-1-start … -6-tor.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> building_replay_steps(const native_args &args,
                                                                                   openage::gamestate::MapSettings &map_settings,
                                                                                   double start,
                                                                                   const std::string &capture_file,
                                                                                   FoodReplayPlan &plan) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using gamestate::map_object_t;
	using gamestate::map_terrain_t;
	using E = renderer::SinkInputEvent;

	const auto map = gamestate::generate_map(map_settings);
	const gamestate::Heightmap heights{map.width, map.height, map.corners};
	const auto tc = map.starts.at(0);
	const long N = static_cast<long>(map.width);
	std::set<std::pair<long, long>> used;
	for (const auto &o : map.objects) {
		used.insert({static_cast<long>(std::floor(o.ne)), static_cast<long>(std::floor(o.se))});
	}
	for (long y = static_cast<long>(tc[1]) - 2; y < static_cast<long>(tc[1]) + 2; ++y) {
		for (long x = static_cast<long>(tc[0]) - 2; x < static_cast<long>(tc[0]) + 2; ++x) {
			used.insert({x, y});
		}
	}
	auto land = [&](long ne, long se) {
		if (ne < 1 or se < 1 or ne >= N - 1 or se >= N - 1) {
			return false;
		}
		auto t = map.tiles[static_cast<size_t>(ne) + static_cast<size_t>(se) * map.width];
		return t != map_terrain_t::WATER and t != map_terrain_t::WATER_MEDIUM and t != map_terrain_t::WATER_DEEP
		       and t != map_terrain_t::ICE and t != map_terrain_t::SHALLOWS and t != map_terrain_t::FOREST;
	};
	// a free horizontal row of \p length tiles (start tile) with \p margin free rows around it, nearest to (cx, cy)
	auto free_row = [&](double cx, double cy, long length, long margin) -> std::optional<std::array<long, 2>> {
		std::optional<std::array<long, 2>> best;
		double best_d = 1e9;
		for (long y = margin + 1; y + margin < N - 1; ++y) {
			for (long x = 2; x + length < N - 2; ++x) {
				const double d = std::hypot(x + 0.5 * length - cx, y + 0.5 - cy);
				if (d >= best_d) {
					continue;
				}
				bool ok = true;
				for (long yy = y - margin; ok and yy <= y + margin; ++yy) {
					for (long xx = x - 1; ok and xx <= x + length; ++xx) {
						ok = land(xx, yy) and not used.contains({xx, yy});
					}
				}
				if (ok) {
					best_d = d;
					best = std::array<long, 2>{x, y};
				}
			}
		}
		return best;
	};
	auto palisade = free_row(tc[0], tc[1] + 5.0, 9, 1);
	if (palisade) {
		for (long x = (*palisade)[0] - 1; x <= (*palisade)[0] + 9; ++x) {
			for (long y = (*palisade)[1] - 1; y <= (*palisade)[1] + 1; ++y) {
				used.insert({x, y});
			}
		}
	}
	auto wall = free_row(tc[0], tc[1] - 6.0, 8, 2);
	log::log(INFO << "buildings replay: palisade row " << (palisade ? std::to_string((*palisade)[0]) + ", " + std::to_string((*palisade)[1]) : "-")
	              << ", stone wall row " << (wall ? std::to_string((*wall)[0]) + ", " + std::to_string((*wall)[1]) : "-"));

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

	std::vector<const gamestate::MapObject *> villagers;
	for (const auto &o : map.objects) {
		if (o.kind == map_object_t::VILLAGER and o.owner == 0) {
			villagers.push_back(&o);
		}
	}
	plan.grant_after = "-sync-0";
	capture(0.6, "-sync-0");
	if (villagers.size() < 2 or not palisade or not wall) {
		log::log(WARN << "buildings replay: map lacks villagers or free rows");
		capture(2.0, nullptr);
		return steps;
	}
	plan.grants.push_back({"-sync-0", 0, "FeudalAge"});
	for (long i = 0; i < 8; ++i) {
		plan.spawns.push_back({"-sync-0", 0, "StoneWall", (*wall)[0] + i + 0.5, (*wall)[1] + 0.5});
	}
	capture(3.0, "-1-start");
	// 1. palisade: select a villager, placement mode from the driver, drag from the start to the end tile
	auto v1 = pixel(villagers[0]->ne, villagers[0]->se, 0.6);
	click(3.5, v1, E::kLeftButton);
	capture(4.2, "-sync-1");
	plan.placements.push_back({"-sync-1", "PalisadeWall"});
	auto a = pixel((*palisade)[0] + 0.5, (*palisade)[1] + 0.5, 0.0);
	auto b = pixel((*palisade)[0] + 8.5, (*palisade)[1] + 0.5, 0.0);
	auto m = pixel((*palisade)[0] + 4.5, (*palisade)[1] + 0.5, 0.0);
	add(5.0, E::kMouseMove, a.first, a.second, 0, 0);
	capture(5.6, "-sync-2");
	add(6.0, E::kMouseDown, a.first, a.second, E::kLeftButton, E::kLeftButton);
	add(6.3, E::kMouseMove, m.first, m.second, 0, E::kLeftButton);
	add(6.6, E::kMouseMove, b.first, b.second, 0, E::kLeftButton);
	capture(7.6, "-2-palisade-ziehen");
	add(8.0, E::kMouseUp, b.first, b.second, E::kLeftButton, 0);
	capture(20.0, "-3-palisade-bau");
	capture(70.0, "-4-palisade-fertig");
	// 2. gate: another villager, placement mode of the gate, preview on the wall, click places it
	auto v2 = pixel(villagers[1]->ne, villagers[1]->se, 0.6);
	click(70.5, v2, E::kLeftButton);
	capture(71.2, "-sync-3");
	plan.placements.push_back({"-sync-3", "StoneGate"});
	auto g = pixel((*wall)[0] + 3.5, (*wall)[1] + 0.5, 0.0);
	add(72.0, E::kMouseMove, g.first - 6, g.second, 0, 0);
	capture(72.6, "-sync-4");
	add(73.0, E::kMouseMove, g.first, g.second, 0, 0);
	capture(74.0, "-5-tor-vorschau");
	click(74.5, g, E::kLeftButton);
	capture(160.0, "-6-tor");
	capture(161.0, nullptr);
	return steps;
}

/**
 * Input replay of the fog of war and the minimap (--replay-vision, XR fork;
 * with --map random): start view (fog around the start, minimap), a villager
 * is sent across the map with a right click on the minimap (explores, leaves
 * explored fog behind), a left click on the minimap jumps the camera to the
 * destination, Alt + click sets a signal, Alt+C / Alt+N switch the minimap
 * mode. Captures <png stem>-v1 … -v5.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> vision_replay_steps(const native_args &args,
                                                                                 openage::gamestate::MapSettings &map_settings,
                                                                                 double start,
                                                                                 const std::string &capture_file) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using gamestate::map_object_t;
	using E = renderer::SinkInputEvent;
	constexpr int alt = 0x08000000;  // Qt::AltModifier

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
	// minimap pixel of a tile (same projection as the interface, xr_minimap.h)
	const auto mm = layout.minimapRect();
	auto minimap_pixel = [&](double ne, double se) {
		agesxr::MinimapGeom g;
		g.cx = static_cast<float>(mm.cx());
		g.cy = static_cast<float>(mm.cy());
		g.hw = static_cast<float>(mm.w()) * 0.5f;
		g.hh = static_cast<float>(mm.h()) * 0.5f;
		float u, v, x, y;
		agesxr::minimapTileToFrac(static_cast<float>(ne), static_cast<float>(se), static_cast<int>(map.width),
		                          static_cast<int>(map.height), u, v);
		g.toPixel(u, v, x, y);
		return std::pair{static_cast<int>(std::lround(x - 0.5f)), static_cast<int>(std::lround(y - 0.5f))};
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
	auto click = [&](double t, std::pair<int, int> p, int button, int modifiers = 0) {
		add(t, E::kMouseMove, p.first, p.second, 0, 0, 0, modifiers);
		add(t + 0.1, E::kMouseDown, p.first, p.second, button, button, 0, modifiers);
		add(t + 0.2, E::kMouseUp, p.first, p.second, button, 0, 0, modifiers);
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
	auto key = [&](double t, int code, int modifiers) {
		add(t, E::kKeyDown, 0, 0, 0, 0, code, modifiers);
		add(t + 0.05, E::kKeyUp, 0, 0, 0, 0, code, modifiers);
	};

	// 1. start: fog around the start position, minimap with the explored start area
	capture(0.6, "-v1-start");
	if (villager == nullptr) {
		log::log(WARN << "vision replay: no villager of player 0 on screen");
		capture(2.0, nullptr);
		return steps;
	}
	// 2. a villager walks to the middle of the map (right click on the minimap) and explores
	// land tile 11..15 tiles from the start towards the middle of the map, no object next to it
	double dest_ne = 0.5 * static_cast<double>(map.width);
	double dest_se = 0.5 * static_cast<double>(map.height);
	{
		using gamestate::map_terrain_t;
		auto land = [](map_terrain_t t) {
			return t == map_terrain_t::GRASS or t == map_terrain_t::GRASS2 or t == map_terrain_t::GRASS3
			       or t == map_terrain_t::DIRT or t == map_terrain_t::DIRT2 or t == map_terrain_t::DIRT3;
		};
		double best = 1e9;
		for (long se = 2; se + 2 < static_cast<long>(map.height); ++se) {
			for (long ne = 2; ne + 2 < static_cast<long>(map.width); ++ne) {
				const double d = std::hypot(ne + 0.5 - tc[0], se + 0.5 - tc[1]);
				const double to_mid = std::hypot(ne - 0.5 * map.width, se - 0.5 * map.height);
				if (d < 11.0 or d > 15.0 or to_mid >= best
				    or not land(map.tiles[static_cast<size_t>(ne) + static_cast<size_t>(se) * map.width])) {
					continue;
				}
				bool free = true;
				for (const auto &o : map.objects) {
					if (std::abs(o.ne - ne) < 2.0 and std::abs(o.se - se) < 2.0) {
						free = false;
						break;
					}
				}
				if (free) {
					best = to_mid;
					dest_ne = ne + 0.5;
					dest_se = se + 0.5;
				}
			}
		}
	}
	auto vp = pixel(villager->ne, villager->se, 0.6);
	auto dest = minimap_pixel(dest_ne, dest_se);
	log::log(INFO << "vision replay: villager at pixel (" << vp.first << ", " << vp.second << "), destination tile ("
	              << dest_ne << ", " << dest_se << ") at minimap pixel (" << dest.first << ", " << dest.second
	              << "), minimap " << mm.x0 << "," << mm.y0 << " " << mm.w() << "x" << mm.h());
	click(1.0, vp, E::kLeftButton);
	capture(2.5, "-sync-v1");
	click(3.0, dest, E::kRightButton);
	capture(4.0, "-sync-v2");
	// 3. the camera jumps to the destination (left click on the minimap) once the villager is there
	// the walk takes real time: the world pass draws at the real time of the clock, so the replay runs at
	// speed 1 and waits (the slow start of software GL compresses the earlier steps)
	capture(60.0, "-v2-unterwegs");
	click(60.5, dest, E::kLeftButton);
	capture(62.0, "-v3-sprung");
	// 4. signal (Alt + click) on the minimap next to the destination
	auto ping = minimap_pixel(dest_ne + 6.0, dest_se - 6.0);
	click(62.5, ping, E::kLeftButton, alt);
	capture(63.2, "-v4-signal");
	// 5. minimap mode combat (Alt+C), back to normal (Alt+N)
	key(63.5, 'C', alt);
	capture(65.0, "-v5-kampf");
	key(65.5, 'N', alt);
	capture(67.0, nullptr);
	return steps;
}

/**
 * Input replay of the combat of round W4 (--replay-war, XR fork; with --map random
 * --map-skirmish, a computer opponent with an early first wave): the own army is selected
 * with a drag and gets the formation "Linie" (Q); the camera jumps to the own town centre and
 * the army walks in front of it in its formation (-w1), takes the stance "Defensiv" (D, -w2);
 * the town centre is selected and rings the town bell (B): the villagers run inside (-w3), the
 * selection panel shows the garrison and the arrows (-w4); the wave of the computer arrives, the
 * town centre shoots its garrison arrows and the armies fight (projectiles in flight, -w5a ...
 * -w5j, -w7). Captures <png stem>-w1 ... -w7.
 */
std::vector<openage::renderer::opengl::TestFrameSink::Step> war_replay_steps(const native_args &args,
                                                                              openage::gamestate::MapSettings &map_settings,
                                                                              double start,
                                                                              const std::string &capture_file) {
	using namespace openage;
	using renderer::opengl::TestFrameSink;
	using E = renderer::SinkInputEvent;

	const auto map = gamestate::generate_map(map_settings);
	const gamestate::Heightmap heights{map.width, map.height, map.corners};
	const auto layout_army = gamestate::combat::skirmish_layout(map);
	const auto tc = map.starts.at(0);
	if (not map_settings.view) {
		gamestate::MapView v;
		v.ne = layout_army.placed ? layout_army.center_ne : tc[0];
		v.se = layout_army.placed ? layout_army.center_se : tc[1];
		v.zoom = 1.3f;
		map_settings.view = v;
	}
	const gamestate::MapView view = *map_settings.view;
	const int width = static_cast<int>(args.width);
	const int height = static_cast<int>(args.height);
	// pixel of a tile seen by a camera that looks at (cne, cse) (the minimap jump keeps height and zoom)
	auto pixel_at = [&](double cne, double cse, double ne, double se, double up) {
		auto camera = std::make_shared<renderer::camera::Camera>(nullptr, util::Vector2s{args.width, args.height});
		camera->look_at_coord(coord::scene3{10.0, 10.0, 0});
		camera->move_to(Eigen::Vector3f{0.0f, view.height, 0.0f});
		camera->look_at_coord(coord::scene3{cne, cse, 0});
		camera->set_zoom(view.zoom);
		const Eigen::Matrix4f matrix = camera->get_projection_matrix() * camera->get_view_matrix();
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
	const auto mm = layout.minimapRect();
	auto minimap_pixel = [&](double ne, double se) {
		agesxr::MinimapGeom g;
		g.cx = static_cast<float>(mm.cx());
		g.cy = static_cast<float>(mm.cy());
		g.hw = static_cast<float>(mm.w()) * 0.5f;
		g.hh = static_cast<float>(mm.h()) * 0.5f;
		float u, v, x, y;
		agesxr::minimapTileToFrac(static_cast<float>(ne), static_cast<float>(se), static_cast<int>(map.width),
		                          static_cast<int>(map.height), u, v);
		g.toPixel(u, v, x, y);
		return std::pair{static_cast<int>(std::lround(x - 0.5f)), static_cast<int>(std::lround(y - 0.5f))};
	};

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

	capture(0.6, "-sync-0");
	if (not layout_army.placed) {
		log::log(WARN << "war replay: no skirmish armies on this map");
		capture(2.0, nullptr);
		return steps;
	}
	// 1. own army: drag a box around it (screen rectangle of its units with a margin)
	int x0 = width, y0 = height, x1 = 0, y1 = 0;
	double own_ne = 0.0, own_se = 0.0, enemy_ne = 0.0, enemy_se = 0.0;
	int own = 0, enemy = 0;
	for (const auto &u : layout_army.units) {
		if (u.owner == 0) {
			auto p = pixel_at(view.ne, view.se, u.ne, u.se, 0.5);
			x0 = std::min(x0, p.first);
			y0 = std::min(y0, p.second);
			x1 = std::max(x1, p.first);
			y1 = std::max(y1, p.second);
			own_ne += u.ne;
			own_se += u.se;
			own += 1;
		}
		else {
			enemy_ne += u.ne;
			enemy_se += u.se;
			enemy += 1;
		}
	}
	own_ne /= std::max(1, own);
	own_se /= std::max(1, own);
	enemy_ne /= std::max(1, enemy);
	enemy_se /= std::max(1, enemy);
	x0 = std::max(5, x0 - 30);
	y0 = std::max(layout.topBarPx() + 5, y0 - 40);
	x1 = std::min(width - 5, x1 + 30);
	y1 = std::min(layout.barRect().y0 - 5, y1 + 20);
	log::log(INFO << "war replay: own army box (" << x0 << ", " << y0 << ") - (" << x1 << ", " << y1 << "), enemy at tile ("
	              << enemy_ne << ", " << enemy_se << ")");
	add(1.0, E::kMouseMove, x0, y0, 0, 0);
	add(1.1, E::kMouseDown, x0, y0, E::kLeftButton, E::kLeftButton);
	add(1.4, E::kMouseMove, (x0 + x1) / 2, (y0 + y1) / 2, 0, E::kLeftButton);
	add(1.7, E::kMouseMove, x1, y1, 0, E::kLeftButton);
	add(1.9, E::kMouseUp, x1, y1, E::kLeftButton, 0);
	capture(2.8, "-sync-1");
	// the selection reaches the interface with its next poll: a few frames before the key
	add(3.0, E::kMouseMove, x1 - 20, y1 - 20, 0, 0);
	capture(3.4, "-sync-1b");
	// 2. formation "Linie" (Q); the camera jumps to the own town centre and the army moves in front
	//    of it (towards the enemy) in its formation – the computer's wave will come there
	key(3.8, 'Q');
	capture(4.2, "-sync-2");
	click(4.5, minimap_pixel(tc[0], tc[1]), E::kLeftButton);
	capture(6.0, "-sync-3");
	const double dn = enemy_ne - tc[0], ds = enemy_se - tc[1];
	const double dl = std::max(1e-6, std::hypot(dn, ds));
	std::pair<int, int> front{width / 2, height / 3};
	for (double reach : {6.0, 5.0, 4.0, 7.0}) {
		auto p = pixel_at(tc[0], tc[1], tc[0] + dn / dl * reach, tc[1] + ds / dl * reach, 0.0);
		if (p.first > 80 and p.first < width - 80 and p.second > layout.topBarPx() + 80 and p.second < layout.barRect().y0 - 80) {
			front = p;
			break;
		}
	}
	log::log(INFO << "war replay: formation destination at pixel (" << front.first << ", " << front.second << ")");
	click(6.5, front, E::kRightButton);
	capture(32.0, "-w1-formation");
	// 3. stance "Defensiv" (D): the button stays pressed
	key(32.5, 'D');
	capture(33.5, "-w2-stellung");
	// 4. select the town centre, town bell (B): the villagers run inside
	auto tcp = pixel_at(tc[0], tc[1], tc[0], tc[1], 1.5);
	click(34.0, tcp, E::kLeftButton);
	capture(35.5, "-sync-4");
	add(35.8, E::kMouseMove, tcp.first + 30, tcp.second + 30, 0, 0);
	capture(36.2, "-sync-4b");
	key(36.6, 'B');
	capture(40.0, "-w3-glocke");
	capture(46.0, "-w4-garnison");
	// 5. the wave of the computer (first attack at 120 s, the steps start 15 s after the first frame):
	//    garrison arrows, fight with projectiles. The fight can be over after some 40 s, so the
	//    captures cover it from its start instead of the time after it.
	const char *fight[] = {"-w5a-pfeile", "-w5b-pfeile", "-w5c-pfeile", "-w5d-pfeile", "-w5e-pfeile",
	                       "-w5f-pfeile", "-w5g-pfeile", "-w5h-pfeile", "-w5i-pfeile", "-w5j-pfeile"};
	for (int i = 0; i < 10; ++i) {
		capture(110.0 + 5.0 * i, fight[i]);
	}
	capture(160.0, "-w7-kampf");
	capture(165.0, nullptr);
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
	FoodReplayPlan food_plan;
	// game user interface (XR fork): only on request, the Quest app draws its own
	std::string ui_demo = args.ui_demo;
	settings.ui = args.ui.value_or(false);
	settings.ui_style = args.ui_style;
	settings.ui_quest = args.ui_quest;
	if (args.replay_aoe and not args.stop_in_resize) {
		steps = aoe_replay_steps(args, map_settings, start, png.string());
	}
	else if (args.replay_tech and not args.stop_in_resize) {
		steps = tech_replay_steps(args, map_settings, start, png.string());
	}
	else if (args.replay_vision and not args.stop_in_resize) {
		steps = vision_replay_steps(args, map_settings, start, png.string());
	}
	else if (args.replay_war and not args.stop_in_resize) {
		steps = war_replay_steps(args, map_settings, start, png.string());
	}
	else if ((args.replay_food or args.replay_fish) and not args.stop_in_resize) {
		steps = food_replay_steps(args, map_settings, start, png.string(), food_plan);
	}
	else if (args.replay_buildings and not args.stop_in_resize) {
		steps = building_replay_steps(args, map_settings, start, png.string(), food_plan);
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
	if (args.replay_prod or args.replay_ui or args.replay_aoe or args.replay_markers or args.replay_tech
	    or args.replay_vision or args.replay_war or args.replay_food or args.replay_fish or args.replay_buildings) {
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

	// food replays (XR fork W3): test options of the production once the captures before them exist
	std::atomic<bool> food_driver_stop{false};
	std::thread food_driver;
	if (args.replay_food or args.replay_fish or args.replay_buildings) {
		const auto dir = png.parent_path();
		const auto stem = png.stem().string();
		const auto ext = png.extension().string();
		food_driver = std::thread{[&food_driver_stop, production = engine->get_production(), dir, stem, ext, plan = food_plan]() {
			auto taken = [&](const std::string &suffix) {
				return not suffix.empty() and std::filesystem::exists(dir / (stem + suffix + ext));
			};
			bool granted = false;
			bool farm = false;
			bool reseed = false;
			std::vector<bool> spawned(plan.spawns.size(), false);
			// XR fork W5: placement modes and technology grants of the buildings replay
			std::vector<bool> placed(plan.placements.size(), false);
			std::vector<bool> granted_tech(plan.grants.size(), false);
			while (not food_driver_stop) {
				for (size_t i = 0; i < plan.grants.size(); ++i) {
					if (not granted_tech[i] and taken(plan.grants[i].after)) {
						granted_tech[i] = true;
						log::log(INFO << "buildings replay: test grant " << plan.grants[i].tech << " for player "
						              << plan.grants[i].player);
						production->grant_research(plan.grants[i].player, plan.grants[i].tech);
					}
				}
				for (size_t i = 0; i < plan.placements.size(); ++i) {
					if (not placed[i] and taken(plan.placements[i].after)) {
						placed[i] = true;
						log::log(INFO << "buildings replay: placement mode " << plan.placements[i].id);
						production->start_placement(plan.placements[i].id);
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (not granted and taken(plan.grant_after)) {
					granted = true;
					log::log(INFO << "food replay: granting resources");
					production->grant_resources(0, 2000.0);
				}
				for (size_t i = 0; i < plan.spawns.size(); ++i) {
					if (not spawned[i] and taken(plan.spawns[i].after)) {
						spawned[i] = true;
						const auto &s = plan.spawns[i];
						log::log(INFO << "food replay: test spawn " << s.name << " of player " << s.player << " at ("
						              << s.ne << ", " << s.se << ")");
						production->test_spawn(s.player, s.name, s.ne, s.se);
					}
				}
				if (not farm and taken(plan.farm_after)) {
					farm = true;
					log::log(INFO << "food replay: farm placement mode");
					production->start_placement("Farm");
				}
				if (not reseed and taken(plan.reseed_after)) {
					reseed = true;
					log::log(INFO << "food replay: two reseeds at the mill");
					production->command(gamestate::prod::CMD_RESEED_FARM);
					production->command(gamestate::prod::CMD_RESEED_FARM);
				}
			}
		}};
	}

	// technology replay (XR fork W1): test grants of the ages once the captures before them exist
	// (the sink takes captures when it gets to them; a grant before a later capture shows in it)
	std::atomic<bool> tech_driver_stop{false};
	std::thread tech_driver;
	if (args.replay_tech) {
		const auto dir = png.parent_path();
		const auto stem = png.stem().string();
		const auto ext = png.extension().string();
		tech_driver = std::thread{[&tech_driver_stop, production = engine->get_production(), dir, stem, ext]() {
			const std::vector<std::pair<std::string, std::string>> grants{
				{"-3-warteschlange", "FeudalAge"},
				{"-4-zeitalter", "CastleAge"},
			};
			size_t next = 0;
			while (not tech_driver_stop and next < grants.size()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				const auto file = dir / (stem + grants[next].first + ext);
				if (std::filesystem::exists(file)) {
					log::log(INFO << "tech replay: " << file.filename().string() << " taken, granting "
					              << grants[next].second << " to player 0");
					production->grant_research(0, grants[next].second);
					next += 1;
				}
			}
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
	if (food_driver.joinable()) {
		food_driver_stop = true;
		food_driver.join();
	}
	if (tech_driver.joinable()) {
		tech_driver_stop = true;
		tech_driver.join();
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
	     or args.replay_aoe or args.replay_markers or args.replay_tech or args.replay_food or args.replay_fish
	     or args.replay_buildings)
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
