// Copyright 2015-2024 the openage authors. See copying.md for legal info.

#include "texture.h"

#include <epoxy/gl.h>

#include <cstring>
#include <tuple>
#include <vector>

#include "../../datastructure/constexpr_map.h"
#include "../../error/error.h"
#include "../../log/log.h"

#include "../resources/texture_data.h"
#include "context.h"
#include "lookup.h"
#include "render_target.h"


namespace openage {
namespace renderer {
namespace opengl {

namespace {

/// Formats for glTexImage2D. OpenGL ES has no GL_BGR, so BGR data is
/// uploaded as GL_RGB and the red and blue channels are swapped back
/// with texture swizzling (see set_bgr_swizzle).
std::tuple<GLint, GLenum, GLenum> gl_pixel_format(const GlContext &context,
                                                  resources::pixel_format fmt) {
	auto fmt_in_out = GL_PIXEL_FORMAT.get(fmt);
	if (context.get_specs().gles and fmt == resources::pixel_format::bgr8) {
		std::get<1>(fmt_in_out) = GL_RGB;
	}
	return fmt_in_out;
}

/// Swap red and blue when sampling a BGR texture stored as RGB (OpenGL ES).
/// Expects the texture to be bound to GL_TEXTURE_2D.
void set_bgr_swizzle(const GlContext &context, resources::pixel_format fmt) {
	if (context.get_specs().gles and fmt == resources::pixel_format::bgr8) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
	}
}

/// Number of channels of a color pixel format.
size_t channel_count(resources::pixel_format fmt) {
	switch (fmt) {
	case resources::pixel_format::r16ui:
	case resources::pixel_format::r32ui:
		return 1;
	case resources::pixel_format::rgb8:
	case resources::pixel_format::bgr8:
		return 3;
	case resources::pixel_format::rgba8:
	case resources::pixel_format::rgba8ui:
		return 4;
	default:
		throw Error(MSG(err) << "Pixel format has no color channels.");
	}
}

/**
 * Read back a color texture on OpenGL ES, which has no glGetTexImage.
 *
 * The texture is attached to a temporary framebuffer and read with
 * glReadPixels. OpenGL ES only guarantees the read formats GL_RGBA with
 * GL_UNSIGNED_BYTE (normalized) and GL_RGBA_INTEGER with GL_UNSIGNED_INT
 * (unsigned integer), so the pixels are read in one of those and packed
 * into the layout of \p info afterwards. BGR textures are stored as RGB
 * (see gl_pixel_format), so their bytes come back in the original order.
 */
std::vector<uint8_t> read_texture_gles(GLuint handle, const resources::Texture2dInfo &info) {
	const auto fmt = info.get_format();
	if (fmt == resources::pixel_format::depth24) {
		throw Error(MSG(err) << "Reading back depth textures is not supported on OpenGL ES.");
	}

	const size_t width = info.get_size().first;
	const size_t height = info.get_size().second;
	const size_t channels = channel_count(fmt);
	const size_t component_size = resources::pixel_size(fmt) / channels;
	const bool integer = fmt == resources::pixel_format::r16ui
	                     or fmt == resources::pixel_format::r32ui
	                     or fmt == resources::pixel_format::rgba8ui;

	GLint prev_read_fbo;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);

	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, handle, 0);
	glReadBuffer(GL_COLOR_ATTACHMENT0);

	// rows of 4 components are always 4-byte aligned
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	std::vector<uint8_t> rgba8;
	std::vector<uint32_t> rgba32;
	if (integer) {
		rgba32.resize(width * height * 4);
		glReadPixels(0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_INT, rgba32.data());
	}
	else {
		rgba8.resize(width * height * 4);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba8.data());
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
	glDeleteFramebuffers(1, &fbo);

	// pack into the texture's own layout (channel count, component size, row padding)
	std::vector<uint8_t> data(info.get_data_size());
	const size_t row_size = info.get_row_size();
	for (size_t y = 0; y < height; ++y) {
		for (size_t x = 0; x < width; ++x) {
			uint8_t *dst = data.data() + y * row_size + x * channels * component_size;
			for (size_t c = 0; c < channels; ++c) {
				const size_t src = (y * width + x) * 4 + c;
				if (not integer) {
					dst[c] = rgba8[src];
				}
				else if (component_size == 1) {
					dst[c] = static_cast<uint8_t>(rgba32[src]);
				}
				else if (component_size == 2) {
					auto val = static_cast<uint16_t>(rgba32[src]);
					std::memcpy(dst + c * 2, &val, 2);
				}
				else {
					uint32_t val = rgba32[src];
					std::memcpy(dst + c * 4, &val, 4);
				}
			}
		}
	}

	return data;
}

} // namespace

GlTexture2d::GlTexture2d(const std::shared_ptr<GlContext> &context,
                         const resources::Texture2dData &data) :
	Texture2d(data.get_info()),
	GlSimpleObject(context,
                   [](GLuint handle) { glDeleteTextures(1, &handle); }) {
	GLuint handle;
	glGenTextures(1, &handle);
	this->handle = handle;

	glBindTexture(GL_TEXTURE_2D, *this->handle);

	// select pixel format
	auto fmt_in_out = gl_pixel_format(*context, this->info.get_format());

	// store raw pixels to gpu
	auto size = this->info.get_size();

	glPixelStorei(GL_UNPACK_ALIGNMENT, this->info.get_row_alignment());

	glTexImage2D(
		GL_TEXTURE_2D,
		0,
		std::get<0>(fmt_in_out),
		size.first,
		size.second,
		0,
		std::get<1>(fmt_in_out),
		std::get<2>(fmt_in_out),
		data.get_data());

	// drawing settings
	// TODO these are outdated, use sampler settings
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	set_bgr_swizzle(*context, this->info.get_format());

	log::log(MSG(dbg) << "Created OpenGL texture from data (size: "
	                  << size.first << "x" << size.second << ")");
}

GlTexture2d::GlTexture2d(const std::shared_ptr<GlContext> &context,
                         const resources::Texture2dInfo &info) :
	Texture2d(info),
	GlSimpleObject(context,
                   [](GLuint handle) { glDeleteTextures(1, &handle); }) {
	GLuint handle;
	glGenTextures(1, &handle);
	this->handle = handle;

	glBindTexture(GL_TEXTURE_2D, *this->handle);

	auto fmt_in_out = gl_pixel_format(*context, this->info.get_format());

	auto size = this->info.get_size();

	glPixelStorei(GL_UNPACK_ALIGNMENT, this->info.get_row_alignment());

	glTexImage2D(
		GL_TEXTURE_2D,
		0,
		std::get<0>(fmt_in_out),
		size.first,
		size.second,
		0,
		std::get<1>(fmt_in_out),
		std::get<2>(fmt_in_out),
		nullptr);

	// TODO these are outdated, use sampler settings
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	set_bgr_swizzle(*context, this->info.get_format());

	log::log(MSG(dbg) << "Created OpenGL texture from info parameters (size: "
	                  << size.first << "x" << size.second << ")");
}

resources::Texture2dData GlTexture2d::into_data() {
	if (this->context->get_specs().gles) {
		// no glGetTexImage on OpenGL ES
		auto data = read_texture_gles(*this->handle, this->info);
		return resources::Texture2dData(resources::Texture2dInfo(this->info), std::move(data));
	}

	auto fmt_in_out = GL_PIXEL_FORMAT.get(this->info.get_format());
	std::vector<uint8_t> data(this->info.get_data_size());

	glPixelStorei(GL_PACK_ALIGNMENT, this->info.get_row_alignment());
	glBindTexture(GL_TEXTURE_2D, *this->handle);
	// TODO use a Pixel Buffer Object instead
	glGetTexImage(GL_TEXTURE_2D, 0, std::get<1>(fmt_in_out), std::get<2>(fmt_in_out), data.data());

	return resources::Texture2dData(resources::Texture2dInfo(this->info), std::move(data));
}

void GlTexture2d::upload(resources::Texture2dData const &data) {
	if (this->info != data.get_info()) {
		throw Error(MSG(err) << "Tried to upload texture data of different format into an existing GPU texture.");
	}

	glBindTexture(GL_TEXTURE_2D, *this->handle);

	auto size = this->info.get_size();
	auto fmt_in_out = gl_pixel_format(*this->context, this->info.get_format());

	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.first, size.second, std::get<1>(fmt_in_out), std::get<2>(fmt_in_out), data.get_data());
}

} // namespace opengl
} // namespace renderer
} // namespace openage
