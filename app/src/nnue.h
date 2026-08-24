#pragma once

#include <libchess/Position.h>

#include "nnue_kernels.h"
#include "weights.h"


struct Accumulator
{
    alignas(64) std::array<std::int16_t, HIDDEN_SIZE> vals;
};

class Eval
{
private:
	Accumulator white;
	Accumulator black;

	Eval();

public:
	Eval(const libchess::Position & pos);

	void reset();
	void set(const libchess::Position & pos);

	int  evaluate    (const bool white_to_move) const;
	void add_piece   (const int piece, const int square, const bool is_white);
	void remove_piece(const int piece, const int square, const bool is_white);

	// Collect the paired rows a piece event touches instead of applying it
	// right away, so callers can batch a whole move's deltas into one sweep.
	void push_delta(nnue_k::Delta *deltas, int &n, const int piece, const int square, const bool is_white, const bool add) const;

	std::array<std::int16_t, HIDDEN_SIZE> & acc_white() { return white.vals; }
	std::array<std::int16_t, HIDDEN_SIZE> & acc_black() { return black.vals; }
};

#if defined(ESP32)
void nnue_load_weights_to_psram();
#endif

