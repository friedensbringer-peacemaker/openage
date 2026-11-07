// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the trade rules (XR fork W7, gamestate/trade_math.h):
 *
 *   openage-trade-check
 *
 * Parameter sweeps:
 * - market: base prices, buy cost with the fees 0.3/0.15 at p = 1.0 (minimum
 *   25 for a small L·p), price +0.03 per buy with the limit 99.99, -0.03 per
 *   sell with the limit 0.2, return to the base price at n = 0 in both
 *   directions, sell revenue uses the NEW price (hand computed examples),
 *   sweep of buy/sell sequences: price always inside [0.2, 99.99] and the
 *   rounding floor(x + 0.5) holds
 * - tribute: fee 0.3/0.2/0, enough stock (sender pays T+F, receiver T), too
 *   little stock (T+F = V, never negative), stock 0
 * - trade: preview and delivery differ as in the spec, distance 0 gives
 *   d = 0.1, G grows with d, caravan (W and v each ×1.5) leaves G equal, the
 *   cog earns less per trip than the cart, symmetry with swapped equal
 *   buildings
 * Exit code 0 if all checks pass. No dependencies besides the standard library.
 */

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "gamestate/trade_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::trade;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string &what) {
	checks += 1;
	if (not ok) {
		failures += 1;
		if (failures <= 40) {
			std::cerr << "FAIL: " << what << std::endl;
		}
	}
}

bool near(double a, double b, double eps = 1e-9) {
	return std::abs(a - b) <= eps;
}

/// deterministic pseudo random numbers for the sweeps (xorshift on 32 bit)
uint32_t next_rand(uint32_t &state) {
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

void check_market() {
	// base prices (spec 02 §2.7): food and wood 1.0, stone 1.3, net 0
	const MarketGood food(BASE_PRICE_FOOD);
	check(near(food.price, 1.0) and food.net == 0, "food starts at 1.0");
	const MarketGood wood(BASE_PRICE_WOOD);
	check(near(wood.price, 1.0), "wood starts at 1.0");
	const MarketGood stone(BASE_PRICE_STONE);
	check(near(stone.price, 1.3) and near(stone.base_price, 1.3), "stone starts at 1.3");

	// buy cost at p = 1.0 with both fees: 100·1·1.3+0.5 -> 130, 100·1·1.15+0.5 -> 115
	MarketGood good(BASE_PRICE_FOOD);
	check(buy_cost(good, 100, 0.3) == 130, "buy cost fee 0.3 at p=1.0 is 130");
	check(buy_cost(good, 100, 0.15) == 115, "buy cost fee 0.15 at p=1.0 is 115");
	// stone at 1.3: 100·1.3·1.3+0.5 -> 169, 100·1.3·1.15+0.5 -> 150
	MarketGood st(BASE_PRICE_STONE);
	check(buy_cost(st, 100, 0.3) == 169, "buy cost stone fee 0.3 is 169");
	check(buy_cost(st, 100, 0.15) == 150, "buy cost stone fee 0.15 is 150");

	// minimum cost 25 for a small L·p: price down to the limit 0.2, then buy
	MarketGood cheap(BASE_PRICE_FOOD);
	for (int i = 0; i < 27; i++) {
		sell_and_apply(cheap, 100, 0.3);
	}
	check(near(cheap.price, PRICE_MIN), "27 sells reach the price limit 0.2");
	check(buy_cost(cheap, 10, 0.3) == 25, "small lot pays the minimum 25");
	check(buy_cost(cheap, 100, 0.3) == 26, "100 units at 0.2 with fee 0.3 cost 26");

	// price up: +0.03 per buy, limit 99.99 (1.0 + 0.03·3300 = 100.0 -> capped)
	MarketGood up(BASE_PRICE_FOOD);
	for (int k = 1; k <= 3400; k++) {
		apply_buy(up);
		check(near(up.price, std::min(1.0 + 0.03 * k, PRICE_MAX), 1e-6), "price up by 0.03, capped at 99.99");
		check(up.net == k, "net counter counts the buys");
	}

	// price down: -0.03 per sell, limit 0.2 (1.0 - 0.03·27 = 0.19 -> capped)
	MarketGood down(BASE_PRICE_FOOD);
	for (int k = 1; k <= 120; k++) {
		sell_and_apply(down, 100, 0.3);
		check(near(down.price, std::max(1.0 - 0.03 * k, PRICE_MIN), 1e-6), "price down by 0.03, floored at 0.2");
		check(down.net == -k, "net counter counts the sells");
	}

	// back to the base price at n = 0, direction sell first: n = -2 -> buy up
	MarketGood back(BASE_PRICE_FOOD);
	sell_and_apply(back, 100, 0.3);
	sell_and_apply(back, 100, 0.3);
	check(back.net == -2 and near(back.price, 0.94), "two sells: n=-2, p=0.94");
	apply_buy(back);
	apply_buy(back);
	check(back.net == 0 and near(back.price, back.base_price), "n=0 returns to the base price (sell first)");
	// the following buy moves off the base price again
	apply_buy(back);
	check(back.net == 1 and near(back.price, 1.03, 1e-6), "buys after the reset work normally");

	// direction buy first: n = 5 -> sell down, stone as well
	MarketGood back2(BASE_PRICE_STONE);
	for (int i = 0; i < 5; i++) {
		apply_buy(back2);
	}
	check(near(back2.price, 1.45, 1e-6), "stone after 5 buys at 1.45");
	for (int i = 0; i < 5; i++) {
		sell_and_apply(back2, 100, 0.3);
	}
	check(back2.net == 0 and near(back2.price, BASE_PRICE_STONE), "n=0 returns to the stone base price");

	// sell revenue uses the NEW price (spec 02 §2.7): fresh food good, first
	// sell drops p to 0.97 -> floor(100·0.97·0.7+0.5) = 68 (with the old
	// price 1.0 it would be 70)
	MarketGood s1(BASE_PRICE_FOOD);
	const int64_t e1 = sell_and_apply(s1, 100, 0.3);
	check(e1 == 68 and near(s1.price, 0.97), "first sell pays 68 with the new price 0.97");
	MarketGood s2(BASE_PRICE_FOOD);
	check(sell_and_apply(s2, 100, 0.15) == 82, "first sell with fee 0.15 pays 82");
	// second consecutive sell: p = 0.94 -> floor(100·0.94·0.7+0.5) = 66
	check(sell_and_apply(s1, 100, 0.3) == 66, "second sell pays 66 with the new price 0.94");
	// fresh stone: p drops 1.3 -> 1.27 -> floor(100·1.27·0.7+0.5) = 89
	MarketGood s3(BASE_PRICE_STONE);
	check(sell_and_apply(s3, 100, 0.3) == 89, "stone sell pays 89 with the new price 1.27");
	// sell that reaches n = 0 pays the base price: buy once (p=1.03), the
	// sell resets p to 1.0 -> floor(100·1.0·0.7+0.5) = 70
	MarketGood s4(BASE_PRICE_FOOD);
	apply_buy(s4);
	check(sell_and_apply(s4, 100, 0.3) == 70, "sell back to n=0 pays the base price 70");

	// conditions (spec 02 §2.7)
	check(can_buy(130, 130, true), "buy with exactly enough gold and a market");
	check(not can_buy(129, 130, true), "not enough gold blocks the buy");
	check(not can_buy(1000, 130, false), "no own market blocks the buy");
	check(can_sell(100, 100, true), "sell with exactly enough stock and a market");
	check(not can_sell(99, 100, true), "not enough stock blocks the sell");
	check(not can_sell(100, 100, false), "no own market blocks the sell");
}

void check_market_sweep() {
	// many buy/sell sequences: the price stays inside [0.2, 99.99], the net
	// counter follows and every cost/revenue rounds with floor(x+0.5)
	uint32_t state = 123456789u;
	for (int seq = 0; seq < 20; seq++) {
		MarketGood good(seq % 2 == 0 ? BASE_PRICE_FOOD : BASE_PRICE_STONE);
		const double fee = (seq % 2 == 0) ? 0.3 : 0.15;
		const int lot = 100;
		int net_model = 0;
		for (int step = 0; step < 4000; step++) {
			if (next_rand(state) & 1) {
				const double x = lot * good.price * (1.0 + fee);
				const int64_t cost = buy_cost(good, lot, fee);
				check(cost == std::max<int64_t>(MIN_BUY_COST, static_cast<int64_t>(std::floor(x + 0.5))),
				      "sweep: buy cost rounds with floor(x+0.5), minimum 25");
				apply_buy(good);
				net_model += 1;
			}
			else {
				const int64_t revenue = sell_and_apply(good, lot, fee);
				// the price already changed: the revenue uses the new price
				const double x = lot * good.price * (1.0 - fee);
				check(revenue == static_cast<int64_t>(std::floor(x + 0.5)),
				      "sweep: sell revenue rounds with floor(x+0.5) on the new price");
				net_model -= 1;
			}
			check(good.net == net_model, "sweep: net counter follows the sequence");
			check(good.price >= PRICE_MIN - 1e-9 and good.price <= PRICE_MAX + 1e-9,
			      "sweep: price stays inside [0.2, 99.99]");
		}
	}
}

void check_tribute() {
	// enough stock (spec 02 §2.8): F = f·T, the sender pays T+F
	const TributeResult r1 = tribute(100.0, 0.3, 1000.0);
	check(near(r1.sent, 100.0) and near(r1.fee, 30.0), "tribute fee 0.3: T=100, F=30");
	check(near(r1.sent + r1.fee, 130.0), "sender pays T+F");
	const TributeResult r2 = tribute(100.0, 0.2, 1000.0);
	check(near(r2.sent, 100.0) and near(r2.fee, 20.0), "coinage fee 0.2: F=20");
	const TributeResult r3 = tribute(100.0, 0.0, 1000.0);
	check(near(r3.sent, 100.0) and near(r3.fee, 0.0), "banking fee 0: nothing lost");

	// exactly enough stock: V = T+F keeps the full amount
	const TributeResult r4 = tribute(100.0, 0.3, 130.0);
	check(near(r4.sent, 100.0) and near(r4.fee, 30.0), "V = T+F still sends everything");

	// too little stock: the tribute shrinks to V, F = f·V, T = V-F, T+F = V
	const TributeResult r5 = tribute(100.0, 0.3, 50.0);
	check(near(r5.fee, 15.0) and near(r5.sent, 35.0), "too little stock: F=f·V, T=V-F");
	check(near(r5.sent + r5.fee, 50.0), "sender pays exactly the stock");
	const TributeResult r6 = tribute(100.0, 0.2, 30.0);
	check(near(r6.fee, 6.0) and near(r6.sent, 24.0), "too little stock with fee 0.2");
	const TributeResult r7 = tribute(100.0, 0.0, 50.0);
	check(near(r7.fee, 0.0) and near(r7.sent, 50.0), "fee 0 with too little stock sends V");

	// stock 0 and amount 0: nothing moves, never negative
	const TributeResult r8 = tribute(100.0, 0.3, 0.0);
	check(r8.sent == 0.0 and r8.fee == 0.0, "stock 0 sends nothing");
	const TributeResult r9 = tribute(0.0, 0.3, 100.0);
	check(r9.sent == 0.0 and r9.fee == 0.0, "amount 0 sends nothing");

	// sweep over amounts, fee rates and stocks
	for (double fee : {0.0, 0.2, 0.25, 0.3}) {
		for (int amount = 0; amount <= 200; amount += 7) {
			for (int stock : {0, 1, 27, 89, 500}) {
				const TributeResult r = tribute(amount, fee, stock);
				check(r.sent >= -1e-9 and r.fee >= -1e-9, "sweep: tribute never negative");
				check(r.sent + r.fee <= stock + 1e-9, "sweep: sender never pays more than the stock");
				if (stock >= amount * (1.0 + fee)) {
					check(near(r.sent, amount) and near(r.fee, fee * amount),
					      "sweep: enough stock sends T with F=f·T");
				}
				else {
					check(near(r.fee, fee * stock) and near(r.sent + r.fee, stock),
					      "sweep: too little stock shrinks to V");
				}
			}
		}
	}
}

void check_trade() {
	// preview and delivery (spec 02 §2.9): home (10,10) r=(2,2),
	// target (30,12) r=(3,1); preview takes 2·r of the TARGET and 1:
	// dx = 20-6-1 = 13, dy = 2-2-1 < 0 -> 0, d = 13; delivery takes both
	// extents without the 1: dx = 20-6-4 = 10, d = 10
	const Footprint home{10.0, 10.0, 2.0, 2.0};
	const Footprint target{30.0, 12.0, 3.0, 1.0};
	check(near(trade_distance_preview(home, target), 13.0), "preview distance 13 (target extent and 1)");
	check(near(trade_distance_delivery(home, target), 10.0), "delivery distance 10 (both extents)");
	// the preview prices the TARGET: swapping the buildings gives 15
	check(near(trade_distance_preview(target, home), 15.0), "preview follows the target building");

	// exact 3-4-5 triangle for the preview: dx = 3, dy = 4 -> d = 5
	const Footprint a{0.0, 0.0, 1.0, 1.0};
	const Footprint b{10.0, 7.0, 3.0, 1.0};
	check(near(trade_distance_preview(a, b), 5.0), "preview 3-4-5 triangle");

	// delivery with dx = 5, dy = 6: d² = 61
	const Footprint c{13.0, 12.0, 3.0, 2.0};
	const double d61 = trade_distance_delivery(a, c);
	check(near(d61 * d61, 61.0, 1e-9), "delivery squares to 61");

	// overlapping buildings: both distances clamp to 0.1
	const Footprint near_home{5.0, 5.0, 1.0, 1.0};
	const Footprint near_target{5.5, 5.0, 1.0, 1.0};
	check(trade_distance_preview(near_home, near_target) == 0.1, "preview 0 distance clamps to 0.1");
	check(trade_distance_delivery(near_home, near_target) == 0.1, "delivery 0 distance clamps to 0.1");

	// buildings with half extent 0.5: preview (2r+1) and delivery (2r+2r)
	// subtract the same 2 tiles, both distances match
	const Footprint small1{0.0, 0.0, 0.5, 0.5};
	const Footprint small2{20.0, 0.0, 0.5, 0.5};
	check(near(trade_distance_preview(small1, small2), trade_distance_delivery(small1, small2)),
	      "half-extent buildings: preview and delivery agree");

	// gold of one trip (spec 02 §2.9): G = 2·d·W·(d/S+0.3)/v
	check(near(trade_gold(100.0, TRADE_WORK_RATE, TRADE_SPEED_CART, 100.0), 59.8),
	      "cart gold at d=100, S=100 is 59.8");
	check(near(trade_gold(50.0, TRADE_WORK_RATE, TRADE_SPEED_CART, 100.0), 18.4),
	      "cart gold at d=50, S=100 is 18.4");
	check(near(trade_gold(10.0, TRADE_WORK_RATE, TRADE_SPEED_CART, 100.0), 1.84),
	      "cart gold at d=10, S=100 is 1.84");

	// G grows with d
	double last = trade_gold(0.1, TRADE_WORK_RATE, TRADE_SPEED_CART, 100.0);
	for (double d : {1.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0}) {
		const double gold = trade_gold(d, TRADE_WORK_RATE, TRADE_SPEED_CART, 100.0);
		check(gold > last, "gold grows with the distance");
		last = gold;
	}

	// caravan (spec 02 §2.9): speed AND work rate ×1.5, the gold per trip
	// stays the same
	for (double d : {10.0, 50.0, 100.0}) {
		for (double width : {50.0, 100.0, 200.0}) {
			const double plain = trade_gold(d, TRADE_WORK_RATE, TRADE_SPEED_CART, width);
			const double caravan = trade_gold(d,
			                                  TRADE_WORK_RATE * CARAVAN_FACTOR,
			                                  TRADE_SPEED_CART * CARAVAN_FACTOR,
			                                  width);
			check(near(caravan, plain), "caravan keeps the gold per trip");
		}
	}

	// cog (spec 02 §2.9): faster (1.32), so less gold per trip than the cart
	for (double d : {10.0, 50.0, 100.0}) {
		for (double width : {50.0, 100.0, 200.0}) {
			const double cart = trade_gold(d, TRADE_WORK_RATE, TRADE_SPEED_CART, width);
			const double cog = trade_gold(d, TRADE_WORK_RATE, TRADE_SPEED_COG, width);
			check(cog < cart, "cog earns less per trip than the cart");
			check(near(cog * TRADE_SPEED_COG, cart), "cog gold is the cart gold over 1.32");
		}
	}

	// symmetry: with equal buildings both orders give the same distance
	const Footprint sym1{10.0, 10.0, 2.0, 3.0};
	const Footprint sym2{40.0, 26.0, 2.0, 3.0};
	check(near(trade_distance_preview(sym1, sym2), trade_distance_preview(sym2, sym1)),
	      "preview symmetric for equal buildings");
	check(near(trade_distance_delivery(sym1, sym2), trade_distance_delivery(sym2, sym1)),
	      "delivery symmetric for equal buildings");

	// trade target: only a DIFFERENT building, own markets allowed
	check(may_trade_with(false), "another building is a trade target");
	check(not may_trade_with(true), "the own building is no trade target");
}

} // namespace

int main() {
	check_market();
	check_market_sweep();
	check_tribute();
	check_trade();

	std::cout << "trade check: " << checks << " checks, " << failures << " failures" << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
