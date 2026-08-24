#pragma once

// Paired-fused NNUE kernels. See docs/paired-fused-nnue.md for the reasoning.
//
// Two jobs live here:
//
//   apply()      - apply a batch of signed feature-row deltas to BOTH
//                  accumulators in one sweep over the lanes.
//   output_dot() - one perspective of the squared-clipped-ReLU output layer,
//                  i.e. sum(clamp(acc,0,QA) * wrap16(clamp(acc,0,QA) * w)).
//
// Every arithmetic path is exact mod-2^16 integer math, identical to the old
// scalar evaluator. Addition mod 2^16 commutes and associates, so fusing,
// reordering or re-laning deltas cannot move a single bit. That property is
// what lets the unit suite pin literal eval values and lets fixed-depth
// searches reproduce the old binary's node counts digit for digit.
//
// The vector intrinsics used here (vaddq_s16, _mm_add_epi16, EE.VADDS.S32 on
// widened lanes) are two's-complement wraparound by definition, so none of
// this carries signed-overflow undefined behavior.
//
// Alignment contract: accumulator arrays and every weight row passed here are
// 16-byte aligned. Accumulators carry alignas(64); paired rows sit on a
// 64-byte-aligned base with a 512-byte stride. On ESP32 the EE.VLD/VST
// instructions silently round addresses DOWN to 16 bytes rather than faulting,
// so a lost alignment there would corrupt data quietly instead of crashing.

#include <cstdint>
#include <cstddef>

#include "weights.h"

constexpr int SCALE = 400;
constexpr std::int16_t QA = 255;
constexpr std::int16_t QB = 64;

#if !defined(ESP32)
#if defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif
#endif

namespace nnue_k {

static_assert(HIDDEN_SIZE % 16 == 0, "kernels sweep in 16-lane tiles");

// One signed delta touching both perspectives. Row 'a' feeds the white
// accumulator, row 'b' feeds the black accumulator; callers resolve which
// paired half belongs to which side from the moving piece's color.
struct Delta {
	const std::int16_t *a;
	const std::int16_t *b;
	bool                add;
};

#if defined(ESP32)
// Board build: hand-written PIE kernels for the common shapes, scalar
// fallback selected at boot if the selftest ever disagrees. Subs before
// adds, always - the order is arithmetically free but fixing it keeps the
// asm variants few. Anything beyond these shapes drains through them.
void pie_selftest();
extern bool pie_ok;

void pie_dual_add (std::int16_t *aw, std::int16_t *ab, const std::int16_t *wa, const std::int16_t *wb);
void pie_dual_sub (std::int16_t *aw, std::int16_t *ab, const std::int16_t *wa, const std::int16_t *wb);
void pie_dual_1s1a(std::int16_t *aw, std::int16_t *ab,
                   const std::int16_t *sw, const std::int16_t *sb,
                   const std::int16_t *aa, const std::int16_t *ab2);
void pie_dual_2s1a(std::int16_t *aw, std::int16_t *ab,
                   const std::int16_t *sw1, const std::int16_t *sb1,
                   const std::int16_t *sw2, const std::int16_t *sb2,
                   const std::int16_t *aa, const std::int16_t *ab2);
#endif

inline void apply(std::int16_t *acc_w, std::int16_t *acc_b, const Delta *ops, int n_ops)
{
	if (n_ops <= 0)
		return;

#if defined(ESP32)
	// Decompose into at most two PIE passes. Every search-time shape is
	// (subs..., adds...) with k+m <= 4; castle (2 subs + 2 adds per side)
	// gets its own kernel, rarer tails ride repeated single-op passes.
	if (!pie_ok) {
		for (int i = 0; i < HIDDEN_SIZE; i++) {
			std::int16_t wa = acc_w[i];
			std::int16_t ba = acc_b[i];
			for (int j = 0; j < n_ops; j++) {
				wa = ops[j].add ? static_cast<std::int16_t>(wa + ops[j].a[i])
				                : static_cast<std::int16_t>(wa - ops[j].a[i]);
				ba = ops[j].add ? static_cast<std::int16_t>(ba + ops[j].b[i])
				                : static_cast<std::int16_t>(ba - ops[j].b[i]);
			}
			acc_w[i] = wa;
			acc_b[i] = ba;
		}
		return;
	}

#define K_ROW(idx)     (ops[idx].a)
#define K_COL(idx)     (ops[idx].b)

	// Eval::set() batches up to 32 same-sign deltas; real moves carry at
	// most four. Big batches drain through single-op kernels directly.
	if (n_ops > 4) {
		for (int i = 0; i < n_ops; i++) {
			if (ops[i].add)
				pie_dual_add(acc_w, acc_b, ops[i].a, ops[i].b);
			else
				pie_dual_sub(acc_w, acc_b, ops[i].a, ops[i].b);
		}
		return;
	}

	int sub_idx[4];
	int add_idx[4];
	int n_si = 0;
	int n_ai = 0;
	for (int i = 0; i < n_ops; i++) {
		if (!ops[i].add)
			sub_idx[n_si++] = i;
		else
			add_idx[n_ai++] = i;
	}

	// Order-independent mapping onto the biggest available kernel. Real
	// shapes: normal/promo [S,A] -> 1s1a; capture/capture-promo [S,S,A]
	// and en-passant [S,A,S]-style batches -> 2s1a; castle [S,S,A,A] ->
	// 2s1a + tail add.
	if (n_si >= 2 && n_ai >= 1) {
		pie_dual_2s1a(acc_w, acc_b,
		              K_ROW(sub_idx[0]), K_COL(sub_idx[0]),
		              K_ROW(sub_idx[1]), K_COL(sub_idx[1]),
		              K_ROW(add_idx[0]), K_COL(add_idx[0]));
		if (n_si == 3) {
			pie_dual_sub(acc_w, acc_b, K_ROW(sub_idx[2]), K_COL(sub_idx[2]));
		}
		if (n_ai >= 2) {
			pie_dual_add(acc_w, acc_b, K_ROW(add_idx[1]), K_COL(add_idx[1]));
		}
	}
	else if (n_si == 1 && n_ai >= 1) {
		pie_dual_1s1a(acc_w, acc_b, K_ROW(sub_idx[0]), K_COL(sub_idx[0]),
		              K_ROW(add_idx[0]), K_COL(add_idx[0]));
		for (int k = 1; k < n_ai; k++)
			pie_dual_add(acc_w, acc_b, K_ROW(add_idx[k]), K_COL(add_idx[k]));
	}
	else if (n_si == 0) {
		for (int k = 0; k < n_ai; k++)
			pie_dual_add(acc_w, acc_b, K_ROW(add_idx[k]), K_COL(add_idx[k]));
	}
	else {
		for (int k = 0; k < n_si; k++)
			pie_dual_sub(acc_w, acc_b, K_ROW(sub_idx[k]), K_COL(sub_idx[k]));
	}

#undef K_ROW
#undef K_COL

#elif defined(__ARM_NEON)
	for (int i = 0; i < HIDDEN_SIZE; i += 16) {
		int16x8_t va0 = vld1q_s16(acc_w + i);
		int16x8_t va1 = vld1q_s16(acc_w + i + 8);
		int16x8_t vb0 = vld1q_s16(acc_b + i);
		int16x8_t vb1 = vld1q_s16(acc_b + i + 8);
		for (int j = 0; j < n_ops; j++) {
			int16x8_t wa0 = vld1q_s16(ops[j].a + i);
			int16x8_t wa1 = vld1q_s16(ops[j].a + i + 8);
			int16x8_t wb0 = vld1q_s16(ops[j].b + i);
			int16x8_t wb1 = vld1q_s16(ops[j].b + i + 8);
			if (ops[j].add) {
				va0 = vaddq_s16(va0, wa0);
				va1 = vaddq_s16(va1, wa1);
				vb0 = vaddq_s16(vb0, wb0);
				vb1 = vaddq_s16(vb1, wb1);
			}
			else {
				va0 = vsubq_s16(va0, wa0);
				va1 = vsubq_s16(va1, wa1);
				vb0 = vsubq_s16(vb0, wb0);
				vb1 = vsubq_s16(vb1, wb1);
			}
		}
		vst1q_s16(acc_w + i, va0);
		vst1q_s16(acc_w + i + 8, va1);
		vst1q_s16(acc_b + i, vb0);
		vst1q_s16(acc_b + i + 8, vb1);
	}
#elif defined(__SSE2__)
	const __m128i *aw = reinterpret_cast<const __m128i *>(acc_w);
	for (int i = 0; i < HIDDEN_SIZE; i += 16) {
		__m128i va0 = _mm_load_si128(aw + (i >> 3));
		__m128i va1 = _mm_load_si128(aw + (i >> 3) + 1);
		__m128i vb0 = _mm_load_si128(reinterpret_cast<const __m128i *>(acc_b + i));
		__m128i vb1 = _mm_load_si128(reinterpret_cast<const __m128i *>(acc_b + i + 8));
		for (int j = 0; j < n_ops; j++) {
			__m128i wa0 = _mm_load_si128(reinterpret_cast<const __m128i *>(ops[j].a + i));
			__m128i wa1 = _mm_load_si128(reinterpret_cast<const __m128i *>(ops[j].a + i + 8));
			__m128i wb0 = _mm_load_si128(reinterpret_cast<const __m128i *>(ops[j].b + i));
			__m128i wb1 = _mm_load_si128(reinterpret_cast<const __m128i *>(ops[j].b + i + 8));
			if (ops[j].add) {
				va0 = _mm_add_epi16(va0, wa0);
				va1 = _mm_add_epi16(va1, wa1);
				vb0 = _mm_add_epi16(vb0, wb0);
				vb1 = _mm_add_epi16(vb1, wb1);
			}
			else {
				va0 = _mm_sub_epi16(va0, wa0);
				va1 = _mm_sub_epi16(va1, wa1);
				vb0 = _mm_sub_epi16(vb0, wb0);
				vb1 = _mm_sub_epi16(vb1, wb1);
			}
		}
		_mm_store_si128(reinterpret_cast<__m128i *>(acc_w + i), va0);
		_mm_store_si128(reinterpret_cast<__m128i *>(acc_w + i + 8), va1);
		_mm_store_si128(reinterpret_cast<__m128i *>(acc_b + i), vb0);
		_mm_store_si128(reinterpret_cast<__m128i *>(acc_b + i + 8), vb1);
	}
#else
	// Reference path. Sanitizer builds run this too - keep it literally
	// identical in spirit to the old evaluator's loops.
	for (int i = 0; i < HIDDEN_SIZE; i++) {
		std::int16_t wa = acc_w[i];
		std::int16_t ba = acc_b[i];
		for (int j = 0; j < n_ops; j++) {
			wa = ops[j].add ? static_cast<std::int16_t>(wa + ops[j].a[i])
			                : static_cast<std::int16_t>(wa - ops[j].a[i]);
			ba = ops[j].add ? static_cast<std::int16_t>(ba + ops[j].b[i])
			                : static_cast<std::int16_t>(ba - ops[j].b[i]);
		}
		acc_w[i] = wa;
		acc_b[i] = ba;
	}
#endif
}

inline int output_dot(const std::int16_t *acc, const std::int16_t *w)
{
#if defined(__ARM_NEON)
	const int16x8_t zero = vdupq_n_s16(0);
	const int16x8_t qa   = vdupq_n_s16(static_cast<std::int16_t>(QA));
	int32x4_t sum = vdupq_n_s32(0);
	for (int i = 0; i < HIDDEN_SIZE; i += 8) {
		int16x8_t a = vmaxq_s16(vld1q_s16(acc + i), zero);
		a = vminq_s16(a, qa);
		// Low 16 bits of the product, exactly like the scalar int16 multiply.
		const int16x8_t m = vmulq_s16(a, vld1q_s16(w + i));
		sum = vmlal_s16(sum, vget_low_s16(a), vget_low_s16(m));
		sum = vmlal_s16(sum, vget_high_s16(a), vget_high_s16(m));
	}
	return vgetq_lane_s32(sum, 0) + vgetq_lane_s32(sum, 1) + vgetq_lane_s32(sum, 2) + vgetq_lane_s32(sum, 3);
#elif defined(__SSE2__)
	const __m128i zero = _mm_setzero_si128();
	const __m128i qa   = _mm_set1_epi16(static_cast<short>(QA));
	__m128i sum = _mm_setzero_si128();
	for (int i = 0; i < HIDDEN_SIZE; i += 8) {
		__m128i a = _mm_load_si128(reinterpret_cast<const __m128i *>(acc + i));
		a = _mm_max_epi16(a, zero);
		a = _mm_min_epi16(a, qa);
		const __m128i m = _mm_mullo_epi16(a, _mm_load_si128(reinterpret_cast<const __m128i *>(w + i)));
		sum = _mm_add_epi32(sum, _mm_madd_epi16(a, m));
	}
	sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 8));
	sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 4));
	return _mm_cvtsi128_si32(sum);
#else
	int output = 0;
	for (int i = 0; i < HIDDEN_SIZE; i++) {
		const std::int16_t input  = static_cast<std::int16_t>(acc[i] < 0 ? 0 : (acc[i] > QA ? QA : acc[i]));
		const std::int16_t weight = static_cast<std::int16_t>(input * w[i]);
		output += int { input } * int { weight };
	}
	return output;
#endif
}

} // namespace nnue_k
