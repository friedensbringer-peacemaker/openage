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
 *                  [--egl-sink-check <png> [--replay | --replay-econ | --replay-combat | --stop-in-resize] [--frames N]]
 *                  [--map test|random [--map-seed N] [--map-size N]
 *                   [--map-trees N] [--map-elevation H] [--map-view ne,se[,zoom[,height]]]
 *                   [--map-skirmish]]
 *                  [--background r,g,b,a [--background-switch r,g,b,a]]
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
 * A hanging engine fails the check instead of blocking: no new frame for
 * 60 s stops it, and if it did not stop 10 s after Engine::stop(), the
 * process exits with an error.
 *
 * --map random uses the random map generator (gamestate/map_generator.h) instead
 * of the fixed test map: deterministic per --map-seed, --map-size tiles (multiple
 * of 16), at most --map-trees tree entities, hills up to --map-elevation. The
 * camera looks at the first start position, or at --map-view (tile ne,se, zoom,
 * camera height; for render checks). --map-skirmish adds a small army per player
 * between the starts (knights, militia, archers; gamestate/combat/skirmish.h) and
 * looks at the battlefield unless --map-view is given.
 *
 * --background sets the color behind the map (RGBA 0..1, window_settings::background;
 * alpha 0 = transparent around the map). With --egl-sink-check,
 * --background-switch changes it at runtime through the frame sink
 * (FrameSink::poll_background) before the second capture (<png stem>-1920x1080.png).
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
#include "log/log.h"
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
	bool replay_combat = false;
	bool stop_in_resize = false;
	uint64_t frames = 1000;
	int seconds = 0;
	size_t width = 1024;
	size_t height = 768;
	openage::gamestate::MapSettings map{};
	std::optional<std::array<float, 4>> background{};
	std::optional<std::array<float, 4>> background_switch{};
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
	             " [--egl-sink-check <png> [--replay | --replay-econ | --replay-combat | --stop-in-resize] [--frames <n>]]"
	             " [--map test|random [--map-seed <n>] [--map-size <n>] [--map-trees <n>]"
	             " [--map-elevation <h>] [--map-view <ne,se[,zoom[,height]]>] [--map-skirmish]]"
	             " [--background <r,g,b,a> [--background-switch <r,g,b,a>]]\n";
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
		else if (arg == "--replay-econ") {
			args.replay_econ = true;
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
		else if (arg == "--background-switch") {
			args.background_switch = parse_rgba(value(), arg);
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
		{"wood", {map_object_t::TREE_PINE, map_object_t::TREE_JUNGLE}},
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
	if (args.replay_econ and not args.stop_in_resize) {
		steps = econ_replay_steps(args, map_settings, start, png.string());
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

	auto engine = std::make_unique<engine::Engine>(engine::Engine::mode::FULL, root, args.modpacks, settings, map_settings);

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
		engine::Engine engine{mode, root, args.modpacks, win_settings, args.map};

		std::jthread timer;
		if (args.seconds > 0 and not render_check) {
			timer = std::jthread{[&engine, seconds = args.seconds]() {
				std::this_thread::sleep_for(std::chrono::seconds(seconds));
				log::log(INFO << "--seconds reached, stopping engine");
				engine.stop();
			}};
		}

		engine.loop();
		log::log(INFO << "engine loop finished");

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
