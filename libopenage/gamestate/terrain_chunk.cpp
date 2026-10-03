// Copyright 2018-2024 the openage authors. See copying.md for legal info.

#include "terrain_chunk.h"


namespace openage::gamestate {

TerrainChunk::TerrainChunk(const util::Vector2s size,
                           const coord::tile_delta offset,
                           const std::vector<TerrainTile> &&tiles) :
	size{size},
	offset{offset},
	tiles{std::move(tiles)} {
	if (this->size[0] > MAX_CHUNK_WIDTH || this->size[1] > MAX_CHUNK_HEIGHT) {
		throw Error(MSG(err) << "Terrain chunk size exceeds maximum size: "
		                     << this->size[0] << "x" << this->size[1] << " > "
		                     << MAX_CHUNK_WIDTH << "x" << MAX_CHUNK_HEIGHT);
	}
}

void TerrainChunk::set_render_entity(const std::shared_ptr<renderer::terrain::RenderEntity> &entity) {
	this->render_entity = entity;
}

TerrainChunk::TerrainChunk(const util::Vector2s size,
                           const coord::tile_delta offset,
                           const std::vector<TerrainTile> &&tiles,
                           std::vector<float> &&corner_heights) :
	TerrainChunk{size, offset, std::move(tiles)} {
	if (corner_heights.size() != (size[0] + 1) * (size[1] + 1)) {
		throw Error(MSG(err) << "Terrain chunk: " << corner_heights.size()
		                     << " corner heights for " << size[0] << "x" << size[1] << " tiles");
	}
	this->corner_heights = std::move(corner_heights);
}

const util::Vector2s &TerrainChunk::get_size() const {
	return this->size;
}

const coord::tile_delta &TerrainChunk::get_offset() const {
	return this->offset;
}

const std::vector<TerrainTile> &TerrainChunk::get_tiles() const {
	return this->tiles;
}

const TerrainTile &TerrainChunk::get_tile(size_t idx) const {
	return this->tiles.at(idx);
}

const TerrainTile &TerrainChunk::get_tile(const coord::tile &pos) const {
	return this->tiles.at(pos.ne + pos.se * this->size[0]);
}

void TerrainChunk::render_update(const time::time_t &time) {
	if (this->render_entity != nullptr) {
		// TODO: Update individual tiles instead of the whole chunk
		std::vector<std::pair<terrain_elevation_t, std::string>> tiles;
		tiles.reserve(this->tiles.size());
		for (const auto &tile : this->tiles) {
			tiles.emplace_back(tile.elevation, tile.terrain_asset_path);
		}

		if (this->corner_heights.empty()) {
			this->render_entity->update(this->size,
			                            tiles,
			                            time);
		}
		else {
			this->render_entity->update(this->size,
			                            tiles,
			                            this->corner_heights,
			                            time);
		}
	}
}

} // namespace openage::gamestate
