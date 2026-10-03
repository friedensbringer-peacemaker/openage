// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>


namespace openage::renderer::resources {

/**
 * Image decoded to 8-bit RGBA, rows top to bottom without padding.
 */
struct rgba8_image {
	size_t width = 0;
	size_t height = 0;
	std::vector<uint8_t> data;
};

/**
 * Load a PNG file with libpng and convert it to 8-bit RGBA (XR fork,
 * replaces QImage).
 *
 * Conversion like QImage::convertTo(QImage::Format_RGBA8888): palette and
 * gray images are expanded, transparency chunks become alpha, 16-bit
 * channels are scaled to 8 bit, images without alpha get alpha 255.
 * No gamma correction is applied.
 *
 * @param file Native path of the PNG file.
 *
 * @throw openage::error::Error if the file can't be read or isn't a valid PNG.
 */
rgba8_image load_png_rgba8(const std::string &file);

/**
 * Store 8-bit RGBA pixel data as PNG file with libpng (XR fork, replaces
 * QImage::save()).
 *
 * Writes the same file as QImage::save() of a Format_RGBA8888 image:
 * default zlib compression and a pHYs chunk with 96 dpi.
 *
 * @param file Native path of the PNG file.
 * @param data Pixel data, rows top to bottom.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param row_size Size of one row in bytes (at least width * 4).
 *
 * @throw openage::error::Error if the file can't be written.
 */
void store_png_rgba8(const std::string &file,
                     const uint8_t *data,
                     size_t width,
                     size_t height,
                     size_t row_size);

} // namespace openage::renderer::resources
