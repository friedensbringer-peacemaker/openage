// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>


namespace openage::gamestate {

/**
 * Player resources of the economy (XR fork).
 *
 * The four stockpile resources of AoE II. nyan resources are mapped by the
 * last part of their name (e.g. "hd_base.data.util.resource.types.Wood").
 *
 * This header has no engine dependencies, so it can be tested without the engine.
 */
enum class resource_t : size_t {
	FOOD,
	WOOD,
	GOLD,
	STONE,
	COUNT,
};

constexpr size_t RESOURCE_COUNT = static_cast<size_t>(resource_t::COUNT);

/// amounts per resource, indexed by resource_t
using resource_amounts_t = std::array<double, RESOURCE_COUNT>;

/// AoE II standard start: 200 food, 200 wood, 100 gold, 200 stone
constexpr resource_amounts_t DEFAULT_START_RESOURCES{200.0, 200.0, 100.0, 200.0};

inline const char *to_string(resource_t type) {
	switch (type) {
	case resource_t::FOOD:
		return "food";
	case resource_t::WOOD:
		return "wood";
	case resource_t::GOLD:
		return "gold";
	case resource_t::STONE:
		return "stone";
	default:
		return "?";
	}
}

/**
 * Resource type of a nyan resource object name.
 *
 * @param fqon Fully qualified name, e.g. "hd_base.data.util.resource.types.Food".
 *
 * @return Resource type, or nothing for other resources (e.g. population space).
 */
inline std::optional<resource_t> resource_from_name(const std::string &fqon) {
	auto dot = fqon.rfind('.');
	auto name = dot == std::string::npos ? fqon : fqon.substr(dot + 1);
	if (name == "Food") {
		return resource_t::FOOD;
	}
	if (name == "Wood") {
		return resource_t::WOOD;
	}
	if (name == "Gold") {
		return resource_t::GOLD;
	}
	if (name == "Stone") {
		return resource_t::STONE;
	}
	return std::nullopt;
}

/**
 * Resource stockpile of a player.
 *
 * Thread-safe: the simulation adds and spends, presenters (HUD) read snapshots.
 */
class ResourceStock {
public:
	explicit ResourceStock(const resource_amounts_t &start = DEFAULT_START_RESOURCES) :
		amounts{start} {}

	ResourceStock(const ResourceStock &other) :
		amounts{other.get()} {}

	ResourceStock &operator=(const ResourceStock &other) {
		if (this != &other) {
			auto values = other.get();
			std::lock_guard<std::mutex> lock{this->mutex};
			this->amounts = values;
		}
		return *this;
	}

	/// snapshot of all amounts
	resource_amounts_t get() const {
		std::lock_guard<std::mutex> lock{this->mutex};
		return this->amounts;
	}

	double get(resource_t type) const {
		std::lock_guard<std::mutex> lock{this->mutex};
		return this->amounts[static_cast<size_t>(type)];
	}

	/// add an amount (negative amounts are ignored), returns the new amount
	double add(resource_t type, double amount) {
		std::lock_guard<std::mutex> lock{this->mutex};
		auto &value = this->amounts[static_cast<size_t>(type)];
		if (amount > 0.0) {
			value += amount;
		}
		return value;
	}

	/// spend all costs if every amount is available (nothing is spent otherwise)
	bool spend(const resource_amounts_t &costs) {
		std::lock_guard<std::mutex> lock{this->mutex};
		for (size_t i = 0; i < RESOURCE_COUNT; ++i) {
			if (costs[i] > this->amounts[i]) {
				return false;
			}
		}
		for (size_t i = 0; i < RESOURCE_COUNT; ++i) {
			this->amounts[i] -= costs[i];
		}
		return true;
	}

private:
	mutable std::mutex mutex;
	resource_amounts_t amounts;
};

} // namespace openage::gamestate
