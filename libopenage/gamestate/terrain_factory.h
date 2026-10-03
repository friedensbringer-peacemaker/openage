// Copyright 2023-2024 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <shared_mutex>
#include <vector>

#include "coord/tile.h"
#include "util/vector.h"


namespace openage {

namespace renderer {
class RenderFactory;
}

namespace gamestate {
class GameState;
class Terrain;
class TerrainChunk;
struct TerrainTile;

/**
 * Creates terrain data (tiles, chunks, etc.) to generate a map.
 */
class TerrainFactory {
public:
	/**
	 * Create a new terrain factory.
	 */
	TerrainFactory() = default;
	~TerrainFactory() = default;

	/**
	 * Create a new empty terrain object.
	 *
	 * @return New terrain object.
	 */
	std::shared_ptr<Terrain> add_terrain(const util::Vector2s &size,
	                                     std::vector<std::shared_ptr<TerrainChunk>> &&chunks);

	/**
	 * Create a new empty terrain chunk.
	 *
	 * @param size Size of the chunk.
	 * @param offset Offset of the chunk.
	 *
	 * @return New terrain chunk.
	 */
	std::shared_ptr<TerrainChunk> add_chunk(const std::shared_ptr<GameState> &gstate,
	                                        const util::Vector2s size,
	                                        const coord::tile_delta offset);

	/**
	 * Create a terrain chunk from given tiles and vertex heights (XR fork, random maps).
	 *
	 * @param size Size of the chunk.
	 * @param offset Offset of the chunk.
	 * @param tiles Tiles (row-major, index ne + se * width).
	 * @param corner_heights (width + 1) * (height + 1) vertex heights.
	 *
	 * @return New terrain chunk.
	 */
	std::shared_ptr<TerrainChunk> add_chunk(const util::Vector2s size,
	                                        const coord::tile_delta offset,
	                                        std::vector<TerrainTile> &&tiles,
	                                        std::vector<float> &&corner_heights);

	// TODO: Add tiles
	// std::shared_ptr<TerrainTile> add_tile(const std::shared_ptr<openage::event::EventLoop> &loop,
	//                                       const std::shared_ptr<GameState> &state,
	//                                       const nyan::fqon_t &nyan_entity);

	/**
	 * Attach a render factory for graphical display.
	 *
	 * This enables rendering for all created terrain chunks.
	 *
	 * @param render_factory Factory for creating connector objects for gamestate->renderer
	 *                       communication.
	 */
	void attach_renderer(const std::shared_ptr<renderer::RenderFactory> &render_factory);

private:
	/**
	 * Factory for creating connector objects to the renderer which make game entities displayable.
	 */
	std::shared_ptr<renderer::RenderFactory> render_factory;

	/**
	 * Mutex for thread safety.
	 */
	std::shared_mutex mutex;
};

} // namespace gamestate
} // namespace openage
