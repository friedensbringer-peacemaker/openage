// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>


namespace openage::gamestate::trade {

/**
 * Rules of the market, the tribute and the trade (XR fork W7,
 * docs/aoe2-spec/02-wirtschaft.md §2.7, §2.8, §2.9): one market price per
 * good shared by all players, tribute with its fee and the gold of one trade
 * trip. Written from the specification; every number that comes from the nyan
 * data or from a player resource is a parameter (fees, lot size, map width),
 * the constants below are the ones the specification names.
 *
 * This header has no engine dependencies (host test gamestate/trade_check.cpp).
 */

// ------------------------------------------------------------------- market

/// base prices of the goods (spec 02 §2.7): food and wood 1.0, stone 1.3;
/// the fourth good of AoE II is unused and not modeled
constexpr double BASE_PRICE_FOOD = 1.0;
constexpr double BASE_PRICE_WOOD = 1.0;
constexpr double BASE_PRICE_STONE = 1.3;

/// price change of one buy/sell (spec 02 §2.7)
constexpr double PRICE_STEP = 0.03;
/// upper price limit (spec 02 §2.7)
constexpr double PRICE_MAX = 99.99;
/// lower price limit (spec 02 §2.7)
constexpr double PRICE_MIN = 0.2;
/// smallest buy cost (spec 02 §2.7)
constexpr int64_t MIN_BUY_COST = 25;

/// state of one good of the shared market (spec 02 §2.7): the market is the
/// same for all players, one price per good in the world, not per player
struct MarketGood {
	/// current price p (gold per lot before the fee)
	double price;
	/// base price the good returns to when the net counter hits 0
	double base_price;
	/// net counter n: buys minus sells since the last reset
	int net;

	/// a good at its base price with net counter 0 (the state every good
	/// starts in)
	constexpr MarketGood(double base_price) :
		price(base_price),
		base_price(base_price),
		net(0) {}
};

/// cost of buying one lot (spec 02 §2.7): max(25, floor(L·p·(1+g) + 0.5))
/// with the price BEFORE the change; g is the trade fee (player resource 78,
/// 0.3; guilds 0.15), L the lot size (player value, 100)
inline int64_t buy_cost(const MarketGood &good, int lot, double fee) {
	const int64_t cost = static_cast<int64_t>(std::floor(lot * good.price * (1.0 + fee) + 0.5));
	return std::max(MIN_BUY_COST, cost);
}

/// apply one buy to the market state (spec 02 §2.7): n+1 and the price up by
/// 0.03 with the limit 99.99; when n reaches 0 the price returns to the base
/// price
inline void apply_buy(MarketGood &good) {
	good.net += 1;
	if (good.net == 0) {
		good.price = good.base_price;
	}
	else {
		good.price = std::min(good.price + PRICE_STEP, PRICE_MAX);
	}
}

/// apply one sell and return the revenue (spec 02 §2.7): the price change
/// runs BEFORE the revenue calculation, so the sell pays with the NEW price
/// (n-1, price down by 0.03 with the limit 0.2, base price at n = 0, then
/// revenue floor(L·p·(1-g) + 0.5) with the new p)
inline int64_t sell_and_apply(MarketGood &good, int lot, double fee) {
	good.net -= 1;
	if (good.net == 0) {
		good.price = good.base_price;
	}
	else {
		good.price = std::max(good.price - PRICE_STEP, PRICE_MIN);
	}
	return static_cast<int64_t>(std::floor(lot * good.price * (1.0 - fee) + 0.5));
}

/// conditions of a buy (spec 02 §2.7): enough gold for the cost and at least
/// one own market
inline bool can_buy(int64_t gold, int64_t cost, bool has_market) {
	return has_market and gold >= cost;
}

/// conditions of a sell (spec 02 §2.7): the stock covers the lot and at
/// least one own market
inline bool can_sell(int64_t stock, int lot, bool has_market) {
	return has_market and stock >= lot;
}

// ------------------------------------------------------------------ tribute

/// result of a tribute (spec 02 §2.8): the amount T that reaches the
/// receiver and the fee F that is lost on the way; the sender pays T + F
struct TributeResult {
	/// amount that reaches the receiver (T)
	double sent;
	/// fee of the sender (F = f·T, reduced to f·V when the stock is too small)
	double fee;
};

/// tribute of the amount T with the fee rate f (player resource 46 of the
/// sender: 0.3, coinage 0.2, banking 0) from the stock V (spec 02 §2.8):
/// F = f·T; when V < T+F the tribute shrinks to the stock (F = f·V, T = V-F),
/// so T+F never exceeds V. The tribute command checks no market (observed);
/// the "market first" restriction lives in the UI only
inline TributeResult tribute(double amount, double fee_rate, double stock) {
	double sent = amount;
	double fee = fee_rate * sent;
	if (stock < sent + fee) {
		fee = fee_rate * stock;
		sent = stock - fee;
	}
	return {sent, fee};
}

// -------------------------------------------------------------------- trade

/// footprint of a building (spec 02 §2.9): center position and half extent
struct Footprint {
	/// center position of the building
	double x;
	double y;
	/// half extent in x and y (r values of the formulas)
	double rx;
	double ry;
};

/// work rate of the trade units (spec 02 §2.9: cart and cog 0.23)
constexpr double TRADE_WORK_RATE = 0.23;
/// speed of the trade cart (spec 02 §2.9)
constexpr double TRADE_SPEED_CART = 1.0;
/// speed of the trade cog (spec 02 §2.9)
constexpr double TRADE_SPEED_COG = 1.32;
/// caravan multiplies speed AND work rate by 1.5 (spec 02 §2.9): the gold per
/// trip stays the same, only the trips get faster
constexpr double CARAVAN_FACTOR = 1.5;

/// distance that prices the goods at the loading (spec 02 §2.9 formula 1):
/// the half extent of the TARGET building and 1 tile come off; only a
/// preview, the delivery pays the other formula
inline double trade_distance_preview(const Footprint &home, const Footprint &target) {
	const double dx = std::max(0.0, std::abs(home.x - target.x) - 2.0 * target.rx - 1.0);
	const double dy = std::max(0.0, std::abs(home.y - target.y) - 2.0 * target.ry - 1.0);
	return std::max(0.1, std::sqrt(dx * dx + dy * dy));
}

/// distance that pays the gold at the delivery (spec 02 §2.9 formula 2): the
/// half extents of BOTH buildings come off (each 2·r, without the -1)
inline double trade_distance_delivery(const Footprint &home, const Footprint &target) {
	const double dx = std::max(0.0, std::abs(home.x - target.x) - 2.0 * target.rx - 2.0 * home.rx);
	const double dy = std::max(0.0, std::abs(home.y - target.y) - 2.0 * target.ry - 2.0 * home.ry);
	return std::max(0.1, std::sqrt(dx * dx + dy * dy));
}

/// gold of one trip (spec 02 §2.9): G = 2·d·W·(d/S + 0.3)/v with the
/// distance d, the work rate W, the speed v of the trade unit and the map
/// width S in tiles (a data value)
inline double trade_gold(double d, double work_rate, double speed, double map_width) {
	return 2.0 * d * work_rate * (d / map_width + 0.3) / speed;
}

/// trade target check (spec 02 §2.9): the unit must aim at a DIFFERENT
/// building; own markets are allowed (observed in the original, presumed
/// allowed)
inline bool may_trade_with(bool same_building) {
	return not same_building;
}

} // namespace openage::gamestate::trade
