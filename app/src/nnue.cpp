#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

#include "weights.cpp"
#include "weights.h"

#include "main.h"
#include "nnue.h"

#if defined(ESP32)
// Dot product of two int16[256] vectors with the inputs clamped to [0, QA],
// computed with the PIE (EE.VMULAS.S16.ACCX) unit. Reproduces the scalar
// output layer bit-exactly: each clamped input is multiplied by its weight,
// the 32-bit product is truncated to int16 (EE.VMUL.S16 with SAR=0 writes
// the low 16 bits), then multiplied by the input again and accumulated into
// the 40-bit ACCX.
static int IRAM_ATTR accx_dot16(const std::int16_t *inputs, const std::int16_t *weights)
{
	alignas(16) static const std::int16_t zero8[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	alignas(16) static const std::int16_t qa8[8]   = { QA, QA, QA, QA, QA, QA, QA, QA };

	register const void *pi __asm__("a2") = inputs;
	register const void *pw __asm__("a3") = weights;
	register const void *pz __asm__("a4") = zero8;
	register const void *pq __asm__("a5") = qa8;
	register int cnt       __asm__("a6") = HIDDEN_SIZE / 8;
	register int result    __asm__("a10") = 0;

	asm volatile(
		"ee.zero.accx\n\t"
		"movi a9, 0\n\t"
		"wsr.sar a9\n\t"                 // SAR=0: EE.VMUL.S16 keeps the full product
		"ee.vld.128.ip q2, a4, 0\n\t"    // zero vector for the clamp
		"ee.vld.128.ip q3, a5, 0\n\t"    // QA vector for the clamp
		"ee.vld.128.ip q0, a2, 16\n\t"   // inputs[0..7]
		"ee.vld.128.ip q1, a3, 16\n\t"   // weights[0..7]
		"loopgtz a6, 1f\n\t"
		"ee.vmax.s16 q0, q0, q2\n\t"     // t = clamp(input, 0, QA)
		"ee.vmin.s16 q0, q0, q3\n\t"
		"ee.vmul.s16 q4, q0, q1\n\t"     // wrap = low16(t * w)
		"ee.vmulas.s16.accx q0, q4\n\t"  // ACCX += t * wrap
		"ee.vld.128.ip q0, a2, 16\n\t"
		"ee.vld.128.ip q1, a3, 16\n\t"
		"1:\n\t"
		"movi a9, 0\n\t"
		"ee.srs.accx a10, a9, 0\n\t"     // result = sat32(ACCX)
		: "+r"(result)
		: "r"(pi), "r"(pw), "r"(pz), "r"(pq), "r"(cnt)
		: "memory", "a9");
	return result;
}
#endif


struct Network {
	Accumulator feature_weights[2 * 6 * 64];
	Accumulator feature_bias;
	Accumulator output_weights[2];
	std::int16_t output_bias;

	int evaluate(const Accumulator& us, const Accumulator& them) const {
		static_assert(sizeof(Network) == weights_size);

		int output = 0;

#if defined(ESP32)
		// side to move
		output += accx_dot16(us.vals.data(), this->output_weights[0].vals.data());

		// not side to move
		output += accx_dot16(them.vals.data(), this->output_weights[1].vals.data());
#else
		// side to move
		for (int i = 0; i < HIDDEN_SIZE; i++) {
			std::int16_t input  = std::clamp(us.vals[i], std::int16_t{0}, QA);
			std::int16_t weight = input * this->output_weights[0].vals[i];
			output += int{input} * int{weight};
		}

		// not side to move
		for (int i = 0; i < HIDDEN_SIZE; i++) {
			std::int16_t input  = std::clamp(them.vals[i], std::int16_t{0}, QA);
			std::int16_t weight = input * this->output_weights[1].vals[i];
			output += int{input} * int{weight};
		}
#endif

		output /= int{QA};
		output += this->output_bias;
		output *= SCALE;
		output /= int{QA} * int{QB};

		return std::clamp(output, -max_non_mate, max_non_mate);
	}

	void add_feature(Accumulator& acc, const int feature_idx) const {
		for (int i = 0; i < HIDDEN_SIZE; i++) {
			acc.vals[i] += this->feature_weights[feature_idx].vals[i];
		}
	}

	void remove_feature(Accumulator& acc, const int feature_idx) const {
		for (int i = 0; i < HIDDEN_SIZE; i++) {
			acc.vals[i] -= this->feature_weights[feature_idx].vals[i];
		}
	}
};

const Network *const NNUE = reinterpret_cast<const Network *>(weights_data);

Eval::Eval()
{
	reset();
}

Eval::Eval(const libchess::Position & pos)
{
	set(pos);
}

void Eval::reset()
{
	this->white = NNUE->feature_bias;
	this->black = NNUE->feature_bias;
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
	if (white_to_move)
		return NNUE->evaluate(this->white, this->black);

	return NNUE->evaluate(this->black, this->white);
}

void IRAM_ATTR Eval::add_piece(const int piece, const int square, const bool is_white)
{
	assert(piece >= 0 && piece < 6);
	if (is_white) {
		NNUE->add_feature(this->white, 64 * piece + square);
		NNUE->add_feature(this->black, 64 * (6 + piece) + (square ^ 56));
	}
	else {
		NNUE->add_feature(this->black, 64 * piece + (square ^ 56));
		NNUE->add_feature(this->white, 64 * (6 + piece) + square);
	}
}

void IRAM_ATTR Eval::remove_piece(const int piece, const int square, const bool is_white)
{
	assert(piece >= 0 && piece < 6);
	if (is_white) {
		NNUE->remove_feature(this->white, 64 * piece + square);
		NNUE->remove_feature(this->black, 64 * (6 + piece) + (square ^ 56));
	}
	else {
		NNUE->remove_feature(this->black, 64 * piece + (square ^ 56));
		NNUE->remove_feature(this->white, 64 * (6 + piece) + square);
	}
}
