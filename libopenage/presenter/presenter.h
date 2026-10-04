// Copyright 2019-2024 the openage authors. See copying.md for legal info.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "renderer/window.h"
#include "util/path.h"


namespace qtgui {
class GuiApplication;
}

namespace openage {

namespace gamestate {
class GameSimulation;
}

namespace input {
class InputManager;
namespace game {
class Controller;
} // namespace game
}

namespace time {
class TimeLoop;
}

namespace renderer {
class RenderPass;
class Renderer;
class Texture2d;
class ShaderProgram;
class Window;

namespace camera {
class Camera;
class CameraManager;
} // namespace camera

namespace gui {
class GUI;
}

namespace hud {
class HudRenderStage;
}

namespace screen {
class ScreenRenderStage;
}

namespace skybox {
class SkyboxRenderStage;
}

namespace terrain {
class TerrainRenderStage;
}

namespace world {
class WorldRenderStage;
}

namespace resources {
class AssetManager;
}

} // namespace renderer

namespace presenter {

/**
 * Thread-safe handle to the game controller of a presenter (XR fork).
 *
 * The presenter creates its game controller in its own thread (init_input)
 * and is destroyed there. Other threads (e.g. the HUD of an embedder) keep
 * this slot instead of a reference to the presenter.
 */
class GameControllerSlot {
public:
	/// the game controller, nullptr before init_input or after the presenter is gone
	std::shared_ptr<input::game::Controller> get() const {
		std::lock_guard<std::mutex> lock{this->mutex};
		return this->controller.lock();
	}

	void set(const std::shared_ptr<input::game::Controller> &controller) {
		std::lock_guard<std::mutex> lock{this->mutex};
		this->controller = controller;
	}

private:
	mutable std::mutex mutex;
	std::weak_ptr<input::game::Controller> controller;
};

class Presenter {
public:
	/**
	 * Create a new presenter.
	 *
	 * @param path Root directory path.
	 * @param simulation Game simulation. Can be set later with \p set_engine()
	 * @param time_loop Time loop which controls simulation time. Can be set later with \p set_time_loop()
	 */
	Presenter(const util::Path &path,
	          const std::shared_ptr<gamestate::GameSimulation> &simulation = nullptr,
	          const std::shared_ptr<time::TimeLoop> &time_loop = nullptr);

	~Presenter() = default;

	/**
	 * Start the presenter and initialize subsystems.
	 *
	 * @param window_settings The settings to customize the display window (e.g. size, display mode, vsync).
	 */
	void run(const renderer::window_settings window_settings = {});

	/**
	 * Ask the draw loop to exit after the current frame (like closing the window).
	 * Safe to call from any thread.
	 */
	void stop();

	/**
	 * Flag behind stop(). Lets other threads request a stop without holding
	 * a reference to the presenter (it must be destroyed in its own GL thread).
	 */
	std::shared_ptr<std::atomic<bool>> get_stop_flag() const;

	/**
	 * Slot of the game controller (XR fork), filled once the input is set up.
	 * Safe to keep and read from any thread.
	 */
	std::shared_ptr<GameControllerSlot> get_game_controller_slot() const;

	/**
	 * Set the game simulation controlled by this presenter.
	 *
	 * @param simulation Game simulation.
	 */
	void set_simulation(const std::shared_ptr<gamestate::GameSimulation> &simulation);

	/**
	 * Set the time loop controlled by this presenter.
	 *
	 * @param time_loop Time loop.
	 */
	void set_time_loop(const std::shared_ptr<time::TimeLoop> &time_loop);

	/**
	 * Initialize the Qt application managing the graphical views. Required
	 * for creating windows.
	 *
	 * @returns Pointer to openage's application wrapper,
	 */
	static std::shared_ptr<qtgui::GuiApplication> init_window_system();

protected:
	/**
	 * Initialize all graphics subsystems of the presenter, i.e.
	 *     - window creation
	 *     - main renderer
	 *     - component renderers (Terrain, Game Entities, GUI)
	 */
	void init_graphics(const renderer::window_settings &window_settings = {});

	/**
	 * Initialize the GUI.
	 */
	void init_gui();

	/**
	 * Initialize the input management.
	 */
	void init_input();

	/**
	 * Initialize the final render pass that renders the results of all previous
	 * render passes to the window screen.
	 */
	void init_final_render_pass();

	/**
	 * Render the final pass into a texture and store it as PNG file
	 * (offscreen render checks, see window_settings::capture_file).
	 *
	 * @param file Absolute path of the PNG file.
	 */
	void capture_frame(const std::string &file);

	/**
	 * Random maps (XR fork): once the game exists, look at its start view and
	 * limit the camera to the map size. The test map keeps the default camera.
	 */
	void apply_map_view();

	// void init_audio();

	/**
	 * Apply the analog camera channel of the frame sink (XR fork), once per frame.
	 */
	void apply_sink_camera();

	/**
	 * Background color requested by a frame sink embedder (XR fork,
	 * FrameSink::poll_background), once per frame.
	 */
	void apply_sink_background();

	/**
	 * Frames around the selected entities in the HUD (XR fork), once per frame.
	 */
	void update_selection_markers();

	/**
	 * Render all configured render passes in sequence.
	 */
	void render();

	// TODO: remove and move into our config/settings system
	util::Path root_dir;

	// graphis components
	/**
	 * Windowing GUI Application wrapper.
	 */
	std::shared_ptr<qtgui::GuiApplication> gui_app;

	/**
	 * Display window.
	 */
	std::shared_ptr<renderer::Window> window;

	/**
	 * Set by stop(), checked once per frame by run().
	 */
	std::shared_ptr<std::atomic<bool>> stop_requested = std::make_shared<std::atomic<bool>>(false);

	/**
	 * Game controller for other threads (XR fork), see get_game_controller_slot().
	 */
	std::shared_ptr<GameControllerSlot> controller_slot = std::make_shared<GameControllerSlot>();

	/**
	 * openage's graphics renderer.
	 */
	std::shared_ptr<renderer::Renderer> renderer;

	/**
	 * Camera for viewing things.
	 */
	std::shared_ptr<renderer::camera::Camera> camera;

	/**
	 * Qt-based GUI for interface.
	 */
	std::shared_ptr<renderer::gui::GUI> gui;

	/**
	 * Camera manager for camera controls.
	 */
	std::shared_ptr<renderer::camera::CameraManager> camera_manager;

	/**
	 * Start view of the map applied (or the test map found) (XR fork).
	 */
	bool map_view_done = false;

	/**
	 * Graphics output for the map background.
	 */
	std::shared_ptr<renderer::skybox::SkyboxRenderStage> skybox_renderer;

	/**
	 * Graphics output for terrain.
	 */
	std::shared_ptr<renderer::terrain::TerrainRenderStage> terrain_renderer;

	/**
	 * Graphics output for units/buildings.
	 */
	std::shared_ptr<renderer::world::WorldRenderStage> world_renderer;

	/**
	 * Graphics output for the HUD.
	 */
	std::shared_ptr<renderer::hud::HudRenderStage> hud_renderer;

	/**
	 * Final graphics output to the window screen.
	 */
	std::shared_ptr<renderer::screen::ScreenRenderStage> screen_renderer;

	/**
	 * Manager for loading/storing asset resources.
	 */
	std::shared_ptr<renderer::resources::AssetManager> asset_manager;

	/**
	 * Render passes in the openage renderer.
	 */
	std::vector<std::shared_ptr<renderer::RenderPass>> render_passes;

	/**
	 * Game simulation.
	 */
	std::shared_ptr<gamestate::GameSimulation> simulation;

	/**
	 * Time loop.
	 */
	std::shared_ptr<time::TimeLoop> time_loop;

	/**
	 * Input manager.
	 */
	std::shared_ptr<input::InputManager> input_manager;

	/**
	 * Frame sink of the embedder (XR fork, null without one): analog camera channel.
	 */
	std::shared_ptr<renderer::FrameSink> sink;

	/**
	 * Camera channel activity for the log (movement summed while active).
	 */
	bool sink_camera_active = false;
	float sink_camera_dx = 0.0f;
	float sink_camera_dy = 0.0f;
	float sink_camera_zoom = 0.0f;
	size_t sink_camera_frames = 0;
};

} // namespace presenter
} // namespace openage
