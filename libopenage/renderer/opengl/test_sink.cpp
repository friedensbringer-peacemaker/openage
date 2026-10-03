// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "test_sink.h"

#include <cstdio>
#include <cstring>
#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include "error/error.h"
#include "log/log.h"
#include "renderer/resources/png_io.h"


namespace openage::renderer::opengl {

namespace {

/// Longest wait of publish() for the consumer.
constexpr auto pace_timeout = std::chrono::milliseconds(100);

/// Consumer rate (like a 90 Hz headset).
constexpr auto consumer_period = std::chrono::microseconds(11111);

std::string egl_hex(EGLint code) {
	char text[16];
	std::snprintf(text, sizeof(text), "0x%04x", static_cast<unsigned>(code));
	return text;
}

/// True if the space separated extension list contains name.
bool has_extension(const char *list, const char *name) {
	if (list == nullptr) {
		return false;
	}
	const size_t len = std::strlen(name);
	for (const char *pos = std::strstr(list, name); pos != nullptr; pos = std::strstr(pos + len, name)) {
		const bool starts = pos == list or pos[-1] == ' ';
		const bool ends = pos[len] == '\0' or pos[len] == ' ';
		if (starts and ends) {
			return true;
		}
	}
	return false;
}

uint64_t pack_size(int width, int height) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) | static_cast<uint32_t>(height);
}

void unpack_size(uint64_t packed, int &width, int &height) {
	width = static_cast<int>(static_cast<uint32_t>(packed >> 32));
	height = static_cast<int>(static_cast<uint32_t>(packed & 0xFFFFFFFFu));
}

void wait_and_delete(void *fence) {
	if (fence != nullptr) {
		GLsync sync = static_cast<GLsync>(fence);
		glWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
		glDeleteSync(sync);
	}
}

void delete_fence(void *fence) {
	if (fence != nullptr) {
		glDeleteSync(static_cast<GLsync>(fence));
	}
}

} // namespace


TestFrameSink::TestFrameSink(int width, int height, std::vector<Step> steps, uint64_t min_frames) :
	steps{std::move(steps)},
	min_frames{min_frames},
	size{pack_size(width, height)} {
	// headless: Mesa's surfaceless platform needs neither X nor Wayland
	const char *client_ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	EGLDisplay dpy = EGL_NO_DISPLAY;
	if (has_extension(client_ext, "EGL_MESA_platform_surfaceless")
	    and has_extension(client_ext, "EGL_EXT_platform_base")) {
		dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
		log::log(MSG(info) << "Test sink: EGL platform surfaceless (Mesa)");
	}
	if (dpy == EGL_NO_DISPLAY) {
		dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		log::log(MSG(info) << "Test sink: default EGL display");
	}
	if (dpy == EGL_NO_DISPLAY) {
		throw Error{MSG(err) << "Test sink: no EGL display"};
	}
	EGLint major = 0;
	EGLint minor = 0;
	if (not eglInitialize(dpy, &major, &minor)) {
		throw Error{MSG(err) << "Test sink: eglInitialize failed: " << egl_hex(eglGetError())};
	}
	this->display = dpy;
	log::log(MSG(info) << "Test sink: EGL " << major << "." << minor << " " << eglQueryString(dpy, EGL_VENDOR));

	if (not eglBindAPI(EGL_OPENGL_ES_API)) {
		throw Error{MSG(err) << "Test sink: eglBindAPI failed: " << egl_hex(eglGetError())};
	}

	const EGLint config_attribs[] = {
		EGL_RENDERABLE_TYPE,
		EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE,
		EGL_PBUFFER_BIT,
		EGL_RED_SIZE,
		8,
		EGL_GREEN_SIZE,
		8,
		EGL_BLUE_SIZE,
		8,
		EGL_ALPHA_SIZE,
		8,
		EGL_NONE,
	};
	EGLConfig cfg = nullptr;
	EGLint count = 0;
	if (not eglChooseConfig(dpy, config_attribs, &cfg, 1, &count) or count < 1) {
		throw Error{MSG(err) << "Test sink: no EGL config for OpenGL ES 3 (RGBA8)"};
	}
	this->config = cfg;

	EGLContext ctx = EGL_NO_CONTEXT;
	for (EGLint minor_version : {2, 0}) {
		const EGLint context_attribs[] = {
			EGL_CONTEXT_MAJOR_VERSION,
			3,
			EGL_CONTEXT_MINOR_VERSION,
			minor_version,
			EGL_NONE,
		};
		ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, context_attribs);
		if (ctx != EGL_NO_CONTEXT) {
			break;
		}
	}
	if (ctx == EGL_NO_CONTEXT) {
		throw Error{MSG(err) << "Test sink: eglCreateContext failed: " << egl_hex(eglGetError())};
	}
	this->context = ctx;

	this->surfaceless = has_extension(eglQueryString(dpy, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context");
	if (not this->surfaceless) {
		const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
		this->consumer_surface = eglCreatePbufferSurface(dpy, cfg, pbuffer_attribs);
		if (this->consumer_surface == EGL_NO_SURFACE) {
			eglDestroyContext(dpy, ctx);
			throw Error{MSG(err) << "Test sink: no surfaceless context and no pbuffer: " << egl_hex(eglGetError())};
		}
	}

	this->consumer = std::thread{[this]() { this->consumer_main(); }};
}


TestFrameSink::~TestFrameSink() {
	this->stop = true;
	this->cv.notify_all();
	if (this->consumer.joinable()) {
		this->consumer.join();
	}

	EGLDisplay dpy = static_cast<EGLDisplay>(this->display);
	if (this->consumer_surface != nullptr) {
		eglDestroySurface(dpy, static_cast<EGLSurface>(this->consumer_surface));
	}
	eglDestroyContext(dpy, static_cast<EGLContext>(this->context));
	eglTerminate(dpy);
}


void *TestFrameSink::egl_display() const {
	return this->display;
}

void *TestFrameSink::egl_config() const {
	return this->config;
}

void *TestFrameSink::share_context() const {
	return this->context;
}

void TestFrameSink::frame_size(int &width, int &height) const {
	unpack_size(this->size.load(), width, height);
}

unsigned TestFrameSink::acquire_target(int width, int height) {
	if (width <= 0 or height <= 0) {
		return 0;
	}

	int index = 0;
	void *wait_before_write = nullptr;
	void *discard = nullptr;
	{
		std::lock_guard<std::mutex> lock{this->mutex};
		// never the newest and never the slot that is read: one of 3 is always free
		for (int i = 0; i < slot_count; ++i) {
			if (i != this->latest and i != this->reading) {
				index = i;
				break;
			}
		}
		Slot &slot = this->slots[index];
		wait_before_write = slot.read_done;
		discard = slot.written;
		slot.read_done = nullptr;
		slot.written = nullptr;
		this->writing = index;
	}
	// the consumer is done with this slot (GPU side), a never read frame is dropped
	wait_and_delete(wait_before_write);
	delete_fence(discard);

	Slot &slot = this->slots[index];
	if (slot.texture == 0 or slot.width != width or slot.height != height) {
		if (slot.producer_fbo != 0) {
			glDeleteFramebuffers(1, &slot.producer_fbo);
		}
		if (slot.texture != 0) {
			glDeleteTextures(1, &slot.texture);
		}
		GLuint texture = 0;
		GLuint fbo = 0;
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindTexture(GL_TEXTURE_2D, 0);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
		const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		std::lock_guard<std::mutex> lock{this->mutex};
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			glDeleteFramebuffers(1, &fbo);
			glDeleteTextures(1, &texture);
			slot.texture = slot.producer_fbo = 0;
			slot.width = slot.height = 0;
			slot.generation += 1;
			log::log(MSG(err) << "Test sink: slot " << index << " framebuffer incomplete");
			return 0;
		}
		slot.texture = texture;
		slot.producer_fbo = fbo;
		slot.width = width;
		slot.height = height;
		slot.generation += 1;
		log::log(MSG(info) << "Test sink: slot " << index << " " << width << "x" << height
		                   << " (generation " << slot.generation << ")");
	}
	return slot.producer_fbo;
}

void TestFrameSink::publish() {
	GLsync written = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	// fence and commands have to reach the other context
	glFlush();

	std::unique_lock<std::mutex> lock{this->mutex};
	if (this->writing < 0) {
		lock.unlock();
		glDeleteSync(written);
		log::log(MSG(warn) << "Test sink: publish() without acquire_target()");
		return;
	}
	this->slots[this->writing].written = written;
	this->latest = this->writing;
	this->writing = -1;
	this->published += 1;
	this->stats.published = this->published;
	this->cv.notify_all();

	// pacing: wait until the consumer took the frame (like the headset rate)
	this->cv.wait_for(lock, pace_timeout, [this] {
		return this->consumed >= this->published or this->stop;
	});
}

size_t TestFrameSink::poll_input(SinkInputEvent *out, size_t max) {
	std::lock_guard<std::mutex> lock{this->mutex};
	size_t count = 0;
	while (count < max and not this->input.empty()) {
		out[count++] = this->input.front();
		this->input.pop_front();
	}
	return count;
}

bool TestFrameSink::should_close() const {
	return false;
}

bool TestFrameSink::paused() const {
	return false;
}

bool TestFrameSink::wait_done(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock{this->mutex};
	this->cv.wait_for(lock, timeout, [this] {
		return this->done or not this->error.empty();
	});
	return this->done and this->error.empty();
}

std::string TestFrameSink::get_error() const {
	std::lock_guard<std::mutex> lock{this->mutex};
	return this->error;
}

TestFrameSink::Stats TestFrameSink::get_stats() const {
	std::lock_guard<std::mutex> lock{this->mutex};
	return this->stats;
}

void TestFrameSink::fail(const std::string &text) {
	log::log(MSG(err) << "Test sink: " << text);
	std::lock_guard<std::mutex> lock{this->mutex};
	if (this->error.empty()) {
		this->error = text;
	}
	this->cv.notify_all();
}

void TestFrameSink::consumer_main() {
	EGLDisplay dpy = static_cast<EGLDisplay>(this->display);
	EGLSurface surface = static_cast<EGLSurface>(this->consumer_surface);
	eglBindAPI(EGL_OPENGL_ES_API);
	if (not eglMakeCurrent(dpy, surface, surface, static_cast<EGLContext>(this->context))) {
		this->fail("consumer eglMakeCurrent failed: " + egl_hex(eglGetError()));
		return;
	}
	log::log(MSG(info) << "Test sink: consumer context "
	                   << reinterpret_cast<const char *>(glGetString(GL_VERSION)));

	try {
		this->consumer_loop();
	}
	catch (std::exception &exc) {
		this->fail(std::string{"consumer: "} + exc.what());
	}

	this->consumer_cleanup();
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglReleaseThread();
}

void TestFrameSink::consumer_loop() {
	using clock = std::chrono::steady_clock;
	auto next_tick = clock::now();
	clock::time_point first_frame{};
	bool have_frame = false;
	uint64_t last_sequence = 0;

	while (not this->stop) {
		// newest frame (or the last one again if there is no new frame)
		int index = -1;
		void *wait_before_read = nullptr;
		bool fresh = false;
		unsigned texture = 0;
		int width = 0;
		int height = 0;
		uint32_t generation = 0;
		{
			std::lock_guard<std::mutex> lock{this->mutex};
			if (this->latest >= 0) {
				index = this->latest;
				this->reading = index;
				Slot &slot = this->slots[index];
				wait_before_read = slot.written;
				slot.written = nullptr;
				texture = slot.texture;
				width = slot.width;
				height = slot.height;
				generation = slot.generation;
				fresh = this->published != last_sequence;
				last_sequence = this->published;
				this->consumed = this->published;
			}
		}
		this->cv.notify_all();

		if (index >= 0) {
			wait_and_delete(wait_before_read);

			// read framebuffers are objects of this context
			Slot &slot = this->slots[index];
			if (slot.read_fbo == 0) {
				glGenFramebuffers(1, &slot.read_fbo);
			}
			if (slot.read_generation != generation) {
				glBindFramebuffer(GL_READ_FRAMEBUFFER, slot.read_fbo);
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
				glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
				slot.read_generation = generation;
			}

			const auto now = clock::now();
			if (not have_frame) {
				first_frame = now;
				have_frame = true;
				log::log(MSG(info) << "Test sink: first frame " << width << "x" << height);
			}
			const double elapsed = std::chrono::duration<double>(now - first_frame).count();

			// timed steps
			while (this->next_step < this->steps.size() and this->steps[this->next_step].at <= elapsed) {
				const Step &step = this->steps[this->next_step];
				if (step.what == Step::kind::input) {
					std::lock_guard<std::mutex> lock{this->mutex};
					this->input.push_back(step.event);
				}
				else if (step.what == Step::kind::resize) {
					log::log(MSG(info) << "Test sink: request " << step.width << "x" << step.height
					                   << " at " << elapsed << " s");
					this->size = pack_size(step.width, step.height);
				}
				else {
					int want_width = 0;
					int want_height = 0;
					unpack_size(this->size.load(), want_width, want_height);
					if (width != want_width or height != want_height) {
						// wait for a frame in the requested size
						break;
					}
					if (not this->capture(slot, step.file)) {
						this->fail("capture failed: " + step.file);
					}
					log::log(MSG(info) << "Test sink: captured frame " << width << "x" << height
					                   << " at " << elapsed << " s to " << step.file);
				}
				this->next_step += 1;
			}

			GLenum gl_error = glGetError();
			uint64_t errors = 0;
			while (gl_error != GL_NO_ERROR) {
				log::log(MSG(err) << "Test sink: consumer GL error " << egl_hex(static_cast<EGLint>(gl_error)));
				errors += 1;
				gl_error = glGetError();
			}

			// read done: back to the producer
			GLsync read_done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			glFlush();
			void *old = nullptr;
			{
				std::lock_guard<std::mutex> lock{this->mutex};
				old = slot.read_done;
				slot.read_done = read_done;
				this->reading = -1;
				this->stats.reads += 1;
				if (fresh) {
					this->stats.frames_read += 1;
				}
				this->stats.gl_errors += errors;
				this->stats.steps_done = this->next_step;
				if (not this->done and this->next_step == this->steps.size()
				    and this->stats.frames_read >= this->min_frames) {
					this->done = true;
					log::log(MSG(info) << "Test sink: done after " << this->stats.frames_read
					                   << " frames, " << elapsed << " s");
				}
			}
			delete_fence(old);
			this->cv.notify_all();
		}

		next_tick += consumer_period;
		const auto now = clock::now();
		if (next_tick < now) {
			next_tick = now;
		}
		std::this_thread::sleep_until(next_tick);
	}
}

bool TestFrameSink::capture(const Slot &slot, const std::string &file) {
	const size_t row = static_cast<size_t>(slot.width) * 4;
	std::vector<uint8_t> pixels(row * slot.height);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, slot.read_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, slot.width, slot.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (glGetError() != GL_NO_ERROR) {
		return false;
	}

	// OpenGL rows start at the bottom
	std::vector<uint8_t> flipped(pixels.size());
	for (int y = 0; y < slot.height; ++y) {
		std::memcpy(flipped.data() + static_cast<size_t>(y) * row,
		            pixels.data() + static_cast<size_t>(slot.height - 1 - y) * row,
		            row);
	}
	resources::store_png_rgba8(file, flipped.data(), slot.width, slot.height, row);

	std::lock_guard<std::mutex> lock{this->mutex};
	this->stats.captures.push_back(file);
	return true;
}

void TestFrameSink::consumer_cleanup() {
	// runs after the producer is gone: its context and framebuffers are destroyed,
	// textures and fences belong to the share group and are deleted here
	std::lock_guard<std::mutex> lock{this->mutex};
	for (Slot &slot : this->slots) {
		if (slot.read_fbo != 0) {
			glDeleteFramebuffers(1, &slot.read_fbo);
		}
		if (slot.texture != 0) {
			glDeleteTextures(1, &slot.texture);
		}
		delete_fence(slot.written);
		delete_fence(slot.read_done);
		slot = Slot{};
	}
	this->latest = this->reading = this->writing = -1;
	this->input.clear();
}


std::vector<TestFrameSink::Step> TestFrameSink::replay_steps(double start, int width, int height, const std::string &capture_file) {
	std::vector<Step> steps;
	auto add = [&](double at, int type, int x, int y, int button, int buttons, int key, int modifiers) {
		Step step;
		step.at = start + at;
		step.what = Step::kind::input;
		step.event.type = type;
		step.event.x = x;
		step.event.y = y;
		step.event.button = button;
		step.event.buttons = buttons;
		step.event.key = key;
		step.event.modifiers = modifiers;
		steps.push_back(step);
	};
	using E = SinkInputEvent;
	constexpr int left = E::kLeftButton;
	constexpr int right = E::kRightButton;
	constexpr int ctrl = E::kControlModifier;

	// Ctrl + left click spawns an entity at the cursor (two of them)
	const int cx = width / 2;
	const int cy = height / 2;
	double t = 0.0;
	for (auto [x, y] : {std::pair{cx, cy}, std::pair{cx + width / 10, cy + height / 12}}) {
		add(t + 0.0, E::kMouseMove, x, y, 0, 0, 0, 0);
		add(t + 0.2, E::kKeyDown, x, y, 0, 0, E::kKeyControl, ctrl);
		add(t + 0.4, E::kMouseDown, x, y, left, left, 0, ctrl);
		add(t + 0.6, E::kMouseUp, x, y, left, 0, 0, ctrl);
		add(t + 0.8, E::kKeyUp, x, y, 0, 0, E::kKeyControl, 0);
		t += 1.0;
	}

	// drag a selection rectangle around both
	t += 1.0;
	const int x0 = cx - width / 6;
	const int y0 = cy - height / 6;
	const int x1 = cx + width / 4;
	const int y1 = cy + height / 4;
	add(t, E::kMouseMove, x0, y0, 0, 0, 0, 0);
	add(t + 0.2, E::kMouseDown, x0, y0, left, left, 0, 0);
	constexpr int drag_steps = 10;
	for (int i = 1; i <= drag_steps; ++i) {
		add(t + 0.2 + 0.1 * i,
		    E::kMouseMove,
		    x0 + (x1 - x0) * i / drag_steps,
		    y0 + (y1 - y0) * i / drag_steps,
		    0,
		    left,
		    0,
		    0);
	}
	t += 2.0;

	// capture while the rectangle is visible
	Step shot;
	shot.at = start + t;
	shot.what = Step::kind::capture;
	shot.file = capture_file;
	steps.push_back(shot);

	// release: select, then right click: move the selection
	add(t + 0.5, E::kMouseUp, x1, y1, left, 0, 0, 0);
	add(t + 1.0, E::kMouseMove, cx + width / 4, cy - height / 8, 0, 0, 0, 0);
	add(t + 1.2, E::kMouseDown, cx + width / 4, cy - height / 8, right, right, 0, 0);
	add(t + 1.4, E::kMouseUp, cx + width / 4, cy - height / 8, right, 0, 0, 0);

	return steps;
}

} // namespace openage::renderer::opengl
