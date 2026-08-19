#if defined(ESP32)

#pragma once

#include <string>
#include <vector>

void init_web();

struct web_search_result_t {
	bool valid = false;
	bool game_over = false;
	std::string game_state; // white_wins / black_wins / draw when game_over
	std::string best_move;
	int score = 0;
	int depth = 0;
	std::string fen; // position the search ran on (for /state during searches)
	std::vector<std::string> pv;
};

// Engine-bridge entry points (implemented in main.cpp). They run the SAME
// registered UCI handlers the serial path uses, so the web game shares the
// search, time management (incl. the ESP32 time-budget fix), book, TT and
// pondering bit-identically with serial UCI mode.
bool web_engine_set_position(const std::vector<std::string> & moves);
bool web_engine_go_movetime(int movetime_ms);
const web_search_result_t & web_engine_last_result();
std::string web_engine_fen();

#endif