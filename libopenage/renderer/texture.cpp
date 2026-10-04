// Copyright 2015-2018 the openage authors. See copying.md for legal info.

#include <cstring>

#include "texture.h"
#include "../error/error.h"


namespace openage {
namespace renderer {

Texture2d::Texture2d(const resources::Texture2dInfo& info)
	: info(info) {}

Texture2d::~Texture2d() = default;

const resources::Texture2dInfo& Texture2d::get_info() const {
	return this->info;
}

uint32_t Texture2d::read_texel_uint(size_t x, size_t y) {
	auto data = this->into_data();
	// read_pixel counts rows from the top
	return data.read_pixel<uint32_t>(x, static_cast<size_t>(this->info.get_size().second) - y - 1);
}

}} // namespace openage::renderer
