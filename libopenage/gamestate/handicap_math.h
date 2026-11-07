// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once


namespace openage::gamestate::handicap {

/**
 * AI difficulty and game speed (XR fork W9, spec 09 §9.5, spec 01 §1.2).
 * No engine dependencies (host test gamestate/handicap_check.cpp).
 */

/// difficulty of the AI, hardest first (spec 09 §9.5)
enum class ai_level_t { HARDEST = 0, HARD = 1, MODERATE = 2, EASY = 3, EASIEST = 4 };

/// factor for the training and research of the AI: EASY 0.75 and EASIEST 0.5,
/// with optional_factors also HARD 0.95 and MODERATE 0.875, else 1.0 (spec 09 §9.5)
inline double production_rate(ai_level_t level, bool optional_factors) {
	switch (level) {
	case ai_level_t::EASY: return 0.75;
	case ai_level_t::EASIEST: return 0.5;
	case ai_level_t::HARD: return optional_factors ? 0.95 : 1.0;
	case ai_level_t::MODERATE: return optional_factors ? 0.875 : 1.0;
	default: return 1.0;
	}
}

/// humans have no handicap (spec 09 §9.5)
inline double human_rate() {
	return 1.0;
}

/// game speed setting (spec 01 §1.2)
enum class game_speed_t { SLOW, NORMAL, FAST };

/// game seconds per real second (spec 01 §1.2); the factors are assumed
/// from public knowledge, the spec names only the three speeds
inline double game_seconds_per_real_second(game_speed_t speed) {
	switch (speed) {
	case game_speed_t::SLOW: return 1.0;
	case game_speed_t::FAST: return 2.0;
	default: return 1.5;
	}
}

/// one game year is five game seconds (spec 01 §1.2)
constexpr double GAME_SECONDS_PER_YEAR = 5.0;

/// game years of the given game seconds
inline double years_of_game_seconds(double seconds) {
	return seconds / GAME_SECONDS_PER_YEAR;
}

} // namespace openage::gamestate::handicap
