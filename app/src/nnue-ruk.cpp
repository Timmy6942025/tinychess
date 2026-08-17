#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#ifdef USE_RUK_NET_256
#include "weights-ruk256.cpp"
#else
#include "weights-ruk.cpp"
#endif
#include "weights.h"

#include "main.h"
#include "nnue.h"


// RukChess 768->512->1 loader (see tools/net_convert.py for the blob layout).
//
// Blob: 40-byte header, then feature_weights [768][HIDDEN_SIZE] int16,
// feature_bias [HIDDEN_SIZE] int16, output_weights [2*HIDDEN_SIZE] int16,
// output_bias int32 (quantised x qin*qout). The loader validates the header
// (magic, source net hash, dims, quantisation, length) before using the net.

struct RukHdr
{
	char     magic[4];
	uint32_t src_magic;
	uint64_t src_hash;
	uint32_t hidden;
	uint32_t input;
	uint32_t output;
	uint32_t qin;
	uint32_t qout;
	uint32_t reserved;
};

static_assert(sizeof(RukHdr) == 40, "RukHdr layout");
static_assert(HIDDEN_SIZE == 512 || HIDDEN_SIZE == 256, "Ruk net hidden size");
static_assert(ruk_weights_size == int(sizeof(RukHdr) + 768 * HIDDEN_SIZE * 2 + HIDDEN_SIZE * 2 + 2 * HIDDEN_SIZE * 2 + 4), "Ruk blob length");

// RukChess nets evaluate on a ~4x smaller scale than Dog's 400-scale net;
// rescale so the search's margins (aspiration, futility, razor, resign)
// behave as tuned. Verified against the float nets on calibrating positions.
#ifdef USE_RUK_NET_256
static constexpr int RUK_SCALE = 4;
#else
static constexpr int RUK_SCALE = 1;
#endif

static const int16_t *ruk_feature_weights = reinterpret_cast<const int16_t *>(ruk_weights_data + sizeof(RukHdr));
static const int16_t *ruk_feature_bias    = ruk_feature_weights + 768 * HIDDEN_SIZE;
static const int16_t *ruk_output_weights  = ruk_feature_bias + HIDDEN_SIZE;
static const int32_t *ruk_output_bias     = reinterpret_cast<const int32_t *>(ruk_output_weights + 2 * HIDDEN_SIZE);

static void add_feature(Accumulator & acc, const int feature_idx);
static void remove_feature(Accumulator & acc, const int feature_idx);

static bool ruk_net_valid(void)
{
	const RukHdr *h = reinterpret_cast<const RukHdr *>(ruk_weights_data);
	return memcmp(h->magic, "MDRK", 4) == 0
		&& h->src_magic == 0x524b5242u
		&& h->src_hash  == ruk_src_hash
		&& h->hidden    == HIDDEN_SIZE
		&& h->input     == 768
		&& h->output    == 1
		&& h->qin       == 64
		&& h->qout      == 512;
}

void Eval::reset()
{
	assert(ruk_net_valid());

	std::copy(ruk_feature_bias, ruk_feature_bias + HIDDEN_SIZE, this->white.vals.begin());
	std::copy(ruk_feature_bias, ruk_feature_bias + HIDDEN_SIZE, this->black.vals.begin());
}

Eval::Eval()
{
	reset();
}

Eval::Eval(const libchess::Position & pos)
{
	set(pos);
}

void Eval::set(const libchess::Position & pos)
{
	reset();

	for(libchess::PieceType type : libchess::constants::PIECE_TYPES) {
		libchess::Bitboard piece_bb_w = pos.piece_type_bb(type, libchess::constants::WHITE);
		while (piece_bb_w) {
			libchess::Square sq = piece_bb_w.forward_bitscan();
			piece_bb_w.forward_popbit();
			add_piece(type, sq, true);
		}

		libchess::Bitboard piece_bb_b = pos.piece_type_bb(type, libchess::constants::BLACK);
		while (piece_bb_b) {
			libchess::Square sq = piece_bb_b.forward_bitscan();
			piece_bb_b.forward_popbit();
			add_piece(type, sq, false);
		}
	}
}

int IRAM_ATTR Eval::evaluate(const bool white_to_move) const
{
	const Accumulator & us   = white_to_move ? this->white : this->black;
	const Accumulator & them = white_to_move ? this->black : this->white;

	int64_t output = *ruk_output_bias;

	for (int i = 0; i < HIDDEN_SIZE; i++) {
		output += int64_t{std::max(0, int(us.vals[i]))}   * ruk_output_weights[i];
		output += int64_t{std::max(0, int(them.vals[i]))} * ruk_output_weights[HIDDEN_SIZE + i];
	}

	output /= 32768;  // qin * qout
	output *= RUK_SCALE;

	return std::clamp(int(output), -max_non_mate, max_non_mate);
}

void IRAM_ATTR Eval::add_piece(const int piece, const int square, const bool is_white)
{
	assert(piece >= 0 && piece < 6);
	// RukChess numbers squares a1=56..h8=7 (rank-flipped vs libchess a1=0),
	// so its square maps to our (square ^ 56). The two accumulators are fixed
	// perspectives, not per-piece-color slots: acc[white] always holds the
	// white-view rows (piece + 6*color), acc[black] the black-view rows
	// (piece + 6*(1-color), square unflipped). Verified bit-exact against the
	// RukChess trainer's NNPredict on the published net (net-7342fb032855).
	if (is_white) {
		add_feature(this->white, 64 * piece + (square ^ 56));
		add_feature(this->black, 64 * (6 + piece) + square);
	}
	else {
		add_feature(this->white, 64 * (6 + piece) + (square ^ 56));
		add_feature(this->black, 64 * piece + square);
	}
}

void IRAM_ATTR Eval::remove_piece(const int piece, const int square, const bool is_white)
{
	assert(piece >= 0 && piece < 6);
	if (is_white) {
		remove_feature(this->white, 64 * piece + (square ^ 56));
		remove_feature(this->black, 64 * (6 + piece) + square);
	}
	else {
		remove_feature(this->white, 64 * (6 + piece) + (square ^ 56));
		remove_feature(this->black, 64 * piece + square);
	}
}

static void add_feature(Accumulator & acc, const int feature_idx)
{
	const int16_t *row = ruk_feature_weights + feature_idx * HIDDEN_SIZE;
	for (int i = 0; i < HIDDEN_SIZE; i++)
		acc.vals[i] += row[i];
}

static void remove_feature(Accumulator & acc, const int feature_idx)
{
	const int16_t *row = ruk_feature_weights + feature_idx * HIDDEN_SIZE;
	for (int i = 0; i < HIDDEN_SIZE; i++)
		acc.vals[i] -= row[i];
}
