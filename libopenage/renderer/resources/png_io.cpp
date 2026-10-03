// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "png_io.h"

#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <png.h>

#include "error/error.h"
#include "log/log.h"


namespace openage::renderer::resources {

namespace {

/**
 * libpng reports errors with longjmp. The functions that call libpng with a
 * setjmp target below only hold trivially destructible values; C++ objects
 * live in the callers.
 */
struct png_error_state {
	char message[256] = "unknown error";
};

void png_error_fn(png_structp png, png_const_charp msg) {
	auto *state = static_cast<png_error_state *>(png_get_error_ptr(png));
	std::snprintf(state->message, sizeof(state->message), "%s", msg);
	png_longjmp(png, 1);
}

void png_warning_fn(png_structp /* png */, png_const_charp msg) {
	log::log(MSG(dbg) << "libpng: " << msg);
}

/// Owns a FILE handle.
struct file_handle {
	std::FILE *file;
	~file_handle() {
		if (this->file != nullptr) {
			std::fclose(this->file);
		}
	}
};

/// Read the header and set up the RGBA8 conversion.
bool read_header(png_structp png, png_infop info, png_uint_32 *width, png_uint_32 *height) {
	if (setjmp(png_jmpbuf(png))) {
		return false;
	}

	png_read_info(png, info);

	png_uint_32 w = 0;
	png_uint_32 h = 0;
	int bit_depth = 0;
	int color_type = 0;
	png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, nullptr, nullptr, nullptr);

	// palette -> RGB, gray < 8 bit -> 8 bit, tRNS -> alpha channel
	png_set_expand(png);
	// 16 bit -> 8 bit with rounding
	png_set_scale_16(png);
	if (color_type == PNG_COLOR_TYPE_GRAY or color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
		png_set_gray_to_rgb(png);
	}
	// opaque alpha for images without alpha channel or tRNS chunk
	if (not(color_type & PNG_COLOR_MASK_ALPHA) and not png_get_valid(png, info, PNG_INFO_tRNS)) {
		png_set_filler(png, 0xff, PNG_FILLER_AFTER);
	}
	png_set_interlace_handling(png);
	png_read_update_info(png, info);

	*width = w;
	*height = h;
	return true;
}

/// Decode all rows.
bool read_rows(png_structp png, png_bytepp rows) {
	if (setjmp(png_jmpbuf(png))) {
		return false;
	}
	png_read_image(png, rows);
	png_read_end(png, nullptr);
	return true;
}

/// Encode all rows.
bool write_image(png_structp png,
                 png_infop info,
                 png_uint_32 width,
                 png_uint_32 height,
                 png_bytepp rows) {
	if (setjmp(png_jmpbuf(png))) {
		return false;
	}

	png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB_ALPHA,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	// QImage writes its default resolution of 96 dpi (3780 dots per meter);
	// keep it so the files are byte-identical to the former QImage output.
	png_set_pHYs(png, info, 3780, 3780, PNG_RESOLUTION_METER);
	png_write_info(png, info);
	png_write_image(png, rows);
	png_write_end(png, info);
	return true;
}

} // namespace


rgba8_image load_png_rgba8(const std::string &file) {
	file_handle fp{std::fopen(file.c_str(), "rb")};
	if (fp.file == nullptr) {
		throw Error{MSG(err) << "Could not open image " << file << ": " << std::strerror(errno)};
	}

	png_byte signature[8];
	if (std::fread(signature, 1, sizeof(signature), fp.file) != sizeof(signature)
	    or png_sig_cmp(signature, 0, sizeof(signature)) != 0) {
		throw Error{MSG(err) << "Image " << file << " is not a PNG file."};
	}

	png_error_state error_state{};
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &error_state,
	                                         png_error_fn, png_warning_fn);
	if (png == nullptr) {
		throw Error{MSG(err) << "Could not create PNG reader for " << file};
	}
	png_infop info = png_create_info_struct(png);
	if (info == nullptr) {
		png_destroy_read_struct(&png, nullptr, nullptr);
		throw Error{MSG(err) << "Could not create PNG reader for " << file};
	}

	png_init_io(png, fp.file);
	png_set_sig_bytes(png, sizeof(signature));

	png_uint_32 width = 0;
	png_uint_32 height = 0;
	bool ok = read_header(png, info, &width, &height);

	rgba8_image image;
	std::vector<png_bytep> rows;
	if (ok) {
		size_t row_size = png_get_rowbytes(png, info);
		if (row_size != size_t{width} * 4) {
			png_destroy_read_struct(&png, &info, nullptr);
			throw Error{MSG(err) << "Image " << file << ": unexpected row size "
			                     << row_size << " after RGBA conversion"};
		}
		image.width = width;
		image.height = height;
		image.data.resize(row_size * height);
		rows.resize(height);
		for (size_t y = 0; y < height; ++y) {
			rows[y] = image.data.data() + y * row_size;
		}
		ok = read_rows(png, rows.data());
	}

	png_destroy_read_struct(&png, &info, nullptr);

	if (not ok) {
		throw Error{MSG(err) << "Could not decode PNG " << file << ": " << error_state.message};
	}

	return image;
}


void store_png_rgba8(const std::string &file,
                     const uint8_t *data,
                     size_t width,
                     size_t height,
                     size_t row_size) {
	if (row_size < width * 4) {
		throw Error{MSG(err) << "Cannot store PNG " << file << ": row size "
		                     << row_size << " too small for width " << width};
	}

	file_handle fp{std::fopen(file.c_str(), "wb")};
	if (fp.file == nullptr) {
		throw Error{MSG(err) << "Could not create image " << file << ": " << std::strerror(errno)};
	}

	png_error_state error_state{};
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &error_state,
	                                          png_error_fn, png_warning_fn);
	if (png == nullptr) {
		throw Error{MSG(err) << "Could not create PNG writer for " << file};
	}
	png_infop info = png_create_info_struct(png);
	if (info == nullptr) {
		png_destroy_write_struct(&png, nullptr);
		throw Error{MSG(err) << "Could not create PNG writer for " << file};
	}

	png_init_io(png, fp.file);

	std::vector<png_bytep> rows(height);
	for (size_t y = 0; y < height; ++y) {
		// libpng doesn't modify the rows while writing
		rows[y] = const_cast<png_bytep>(data + y * row_size);
	}

	bool ok = write_image(png, info, static_cast<png_uint_32>(width),
	                      static_cast<png_uint_32>(height), rows.data());
	png_destroy_write_struct(&png, &info);

	if (not ok) {
		throw Error{MSG(err) << "Could not encode PNG " << file << ": " << error_state.message};
	}
	if (std::fflush(fp.file) != 0) {
		throw Error{MSG(err) << "Could not write image " << file << ": " << std::strerror(errno)};
	}
}

} // namespace openage::renderer::resources
