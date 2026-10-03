// Copyright 2015-2026 the openage authors. See copying.md for legal info.

#include "texture_data.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

#include "renderer/resources/png_io.h"

#include "error/error.h"
#include "log/log.h"
#include "renderer/resources/texture_info.h"
#include "renderer/resources/texture_subinfo.h"
#include "util/path.h"


namespace openage::renderer::resources {

/// Tries to guess the alignment of image rows based on image parameters. Kinda
/// black magic and might not actually work.
/// @param width in pixels of the image
/// @param fmt of pixels in the image
/// @param row_size the actual size in bytes of an image row, including padding
static constexpr size_t guess_row_alignment(size_t width, pixel_format fmt, size_t row_size) {
	// Use the highest possible alignment for even-width images.
	if (width % 8 == 0) {
		return 8;
	}
	if (width % 4 == 0) {
		return 4;
	}
	if (width % 2 == 0) {
		return 2;
	}

	// The size of meaningful data in each row.
	size_t pix_bytes = width * pixel_size(fmt);
	// The size of padding.
	size_t padding = row_size - pix_bytes;

	if (padding == 0) {
		return 1;
	}
	if (padding <= 1) {
		return 2;
	}
	if (padding <= 3) {
		return 4;
	}
	if (padding <= 7) {
		return 8;
	}

	// Bail with a sane value.
	return 4;
}

Texture2dData::Texture2dData(const util::Path &path) {
	std::string native_path = path.resolve_native_path();

	// XR fork: libpng instead of QImage, same RGBA8 conversion
	auto image = load_png_rgba8(native_path);

	log::log(MSG(dbg) << "Texture has been loaded from " << native_path);

	auto pix_fmt = pixel_format::rgba8;
	auto w = uint32_t(image.width);
	auto h = uint32_t(image.height);
	size_t row_size = image.width * pixel_size(pix_fmt);

	this->data = std::move(image.data);

	std::vector<Texture2dSubInfo> subtextures;
	// we don't have a texture description file.
	// use the whole image as one texture then.
	Texture2dSubInfo s(0, 0, w, h, w / 2, h / 2, w, h);

	subtextures.push_back(s);

	size_t align = guess_row_alignment(w, pix_fmt, row_size);
	this->info = Texture2dInfo(w, h, pix_fmt, path, align, std::move(subtextures));
}

Texture2dData::Texture2dData(Texture2dInfo const &info) :
	info{info} {
	std::string native_path = info.get_image_path().value().resolve_native_path();

	// XR fork: libpng instead of QImage, same RGBA8 conversion
	auto image = load_png_rgba8(native_path);

	log::log(MSG(dbg) << "Texture has been loaded from " << native_path);

	this->data = std::move(image.data);
}

Texture2dData::Texture2dData(Texture2dInfo const &info, std::vector<uint8_t> &&data) :
	info(info), data(std::move(data)) {}

Texture2dData Texture2dData::flip_y() {
	size_t row_size = this->info.get_row_size();
	size_t height = this->info.get_size().second;

	std::vector<uint8_t> new_data(this->data.size());

	for (size_t y = 0; y < height; ++y) {
		std::copy(this->data.data() + row_size * y, this->data.data() + row_size * (y + 1), new_data.end() - row_size * (y + 1));
	}

	this->data = new_data;

	Texture2dInfo new_info(this->info);

	return Texture2dData(std::move(new_info), std::move(new_data));
}

const Texture2dInfo &Texture2dData::get_info() const {
	return this->info;
}

const uint8_t *Texture2dData::get_data() const {
	return this->data.data();
}

void Texture2dData::store(const util::Path &file) const {
	log::log(MSG(info) << "Saving texture data to " << file);

	if (this->info.get_format() != pixel_format::rgba8) {
		throw Error(MSG(err) << "Storing 2D textures into files is unimplemented. PRs welcome :D");
	}

	auto size = this->info.get_size();

	// XR fork: libpng instead of QImage
	std::string path = file.resolve_native_path_w();
	store_png_rgba8(path, this->data.data(), size.first, size.second, this->info.get_row_size());
}

} // namespace openage::renderer::resources
