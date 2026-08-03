/*
 * ESP32-S3 PIE (128-bit SIMD) kernels for the NNUE incremental
 * accumulator updates. Both functions process HIDDEN_SIZE (128)
 * int16 elements = 16 x 128-bit vectors.
 *
 * The saturating add/sub is bit-identical to the scalar wrapping
 * add as long as |accumulator| < 32768, which holds in practice
 * (max observed magnitude ~19360, see acc_scan).
 *
 * dst and src must be 16-byte aligned (Accumulator is alignas(64),
 * feature rows are alignas(64)).
 */

	.section .iram1,"ax",@progbits
	.align 4
	.global nnue_vec_add
	.type   nnue_vec_add,@function
// void nnue_vec_add(int16_t *dst, const int16_t *src)
// a2 = dst, a3 = src
nnue_vec_add:
	entry	a1, 16
	mov.n	a4, a2			// store pointer, starts at dst
	movi	a5, 16			// 1 preloaded + 16 loop iterations = 17 loads, 16 computes
	ee.vld.128.ip	q0, a2, 16	// preload dst vector 0
	loopnez	a5, .add_done
		ee.vld.128.ip	q1, a3, 16
		ee.vadds.s16.ld.incp	q0, a2, q2, q0, q1
		ee.vst.128.ip	q2, a4, 16
.add_done:
	retw.n

	.align 4
	.global nnue_vec_sub
	.type   nnue_vec_sub,@function
// void nnue_vec_sub(int16_t *dst, const int16_t *src)
// a2 = dst, a3 = src
nnue_vec_sub:
	entry	a1, 16
	mov.n	a4, a2
	movi	a5, 16
	ee.vld.128.ip	q0, a2, 16
	loopnez	a5, .sub_done
		ee.vld.128.ip	q1, a3, 16
		ee.vsubs.s16.ld.incp	q0, a2, q2, q0, q1
		ee.vst.128.ip	q2, a4, 16
.sub_done:
	retw.n
