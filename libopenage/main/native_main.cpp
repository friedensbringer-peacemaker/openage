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
 *                  [--egl-sink-check <png> [--replay] [--frames N]]
 *                  [--map test|random [--map-seed N] [--map-size N]
 *                   [--map-trees N] [--map-elevation H] [--map-view ne,se[,zoom[,height]]]]
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
 * visible), a right click moves them.
 *
 * --map random uses the random map generator (gamestate/map_generator.h) instead
 * of the fixed test map: deterministic per --map-seed, --map-size tiles (multiple
 * of 16), at most --map-trees tree entities, hills up to --map-elevation. The
 * camera looks at the first start position, or at --map-view (tile ne,se, zoom,
 * camera height; for render checks).
 *
 * <dir> must contain assets/ (with shaders and converted/{engine,<modpack>})
 * and cfg/. The converted modpacks (including the "engine" API modpack) are
 * produced offline with `python -m openage convert` and
 * `python -m openage convert-export-api`.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"

#include "engine/check_root.h"
#include "engine/engine.h"
#include "error/error.h"
#include "gamestate/map_settings.h"
#include "log/log.h"
#if WITH_QT
	#include "renderer/gui/integration/public/gui_application_with_logger.h"
#endif
#if WITH_EGL
	#include "renderer/opengl/test_sink.h"
#endif
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
	uint64_t frames = 1000;
	int seconds = 0;
	size_t width = 1024;
	size_t height = 768;
	openage::gamestate::MapSettings map{};
};

void usage(const char *argv0) {
	std::cerr << "usage: " << argv0
	          << " --root <dir> [--modpack <id>]... [--headless] [--seconds <n>]"
	             " [--width <w> --height <h>] [--check]"
	             " [--gles] [--render-check <png>] [--shader-check]"
	             " [--egl-sink-check <png> [--replay] [--frames <n>]]"
	             " [--map test|random [--map-seed <n>] [--map-size <n>] [--map-trees <n>]"
	             " [--map-elevation <h>] [--map-view <ne,se[,zoom[,height]]>]]\n";
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

	std::vector<TestFrameSink::Step> steps;
	if (args.replay) {
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
	TestFrameSink::Step resize;
	resize.at = last + 1.0;
	resize.what = TestFrameSink::Step::kind::resize;
	resize.width = 1920;
	resize.height = 1080;
	steps.push_back(resize);
	TestFrameSink::Step shot;
	shot.at = last + 4.0;
	shot.what = TestFrameSink::Step::kind::capture;
	shot.file = png_resized.string();
	steps.push_back(shot);

	// declared before the engine: destroyed after the engine threads are joined
	auto sink = std::make_shared<TestFrameSink>(width, height, steps, args.frames);
	settings.sink = sink;

	auto engine = std::make_unique<engine::Engine>(engine::Engine::mode::FULL, root, args.modpacks, settings, args.map);

	// stop the engine once the sink is done (or failed, or after a generous timeout)
	std::atomic<bool> loop_finished{false};
	bool sink_done = false;
	clock::time_point stop_time{};
	std::thread watcher{[&]() {
		const auto deadline = clock::now() + std::chrono::seconds(static_cast<int>(last) + 900);
		while (not loop_finished and clock::now() < deadline) {
			if (sink->wait_done(std::chrono::milliseconds(100))) {
				sink_done = true;
				break;
			}
			if (not sink->get_error().empty()) {
				break;
			}
		}
		stop_time = clock::now();
		log::log(INFO << "egl sink check: stopping engine");
		engine->stop();
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
	watcher.join();
	if (not loop_error.empty()) {
		engine.reset();
		return false;
	}
	// joins the time loop and presenter threads
	engine.reset();
	const double stop_seconds = std::chrono::duration<double>(clock::now() - stop_time).count();

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
	if (not sink_done) {
		log::log(ERR << "egl sink check: sink not done (frames or steps missing)");
		ok = false;
	}
	if (stats.gl_errors != 0) {
		ok = false;
	}
	if (stop_seconds >= 1.0) {
		log::log(ERR << "egl sink check: stop took " << stop_seconds << " s (limit 1 s)");
		ok = false;
	}
	for (const auto &file : {png, png_resized}) {
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
