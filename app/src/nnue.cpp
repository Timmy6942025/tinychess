#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "weights.cpp"
#include "weights.h"

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

#include "main.h"
#include "nnue.h"
#include "nnue_kernels.h"

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

	int IRAM_ATTR evaluate(const Accumulator& us, const Accumulator& them) const {
		static_assert(sizeof(Network) == weights_size);

		int output;

#if defined(ESP32)
		output  = accx_dot16(us.vals.data(),   this->output_weights[0].vals.data());
		output += accx_dot16(them.vals.data(), this->output_weights[1].vals.data());
#else
		output  = nnue_k::output_dot(us.vals.data(),   this->output_weights[0].vals.data());
		output += nnue_k::output_dot(them.vals.data(), this->output_weights[1].vals.data());
#endif

		output /= int{QA};
		output += this->output_bias;
		output *= SCALE;
		output /= int{QA} * int{QB};

		return std::clamp(output, -max_non_mate, max_non_mate);
	}
};

const Network *NNUE = reinterpret_cast<const Network *>(weights_data);

// ---------------------------------------------------------------------------
// Paired weight table
//
// The blob stores own-perspective feature rows at index f (0..383) and other-
// perspective rows at f+384. Every piece event reads exactly one of each, so
// the table permutes into 384 contiguous [own|other] pairs: one event touches
// a single 1KB region instead of two rows 192KB apart, and both perspectives
// update out of one sequential stream.
//
// Pairing is a fixed permutation of the trained weights - no value changes,
// only addresses - and accumulator updates commute mod 2^16, so evaluation
// stays bit-exact against the old flat layout.
//
// Desktop: permuted copy in static storage; the blob itself is .rodata and
// stays untouched (it still serves feature_bias / output weights).
// ESP32: the PSRAM copy of the blob permutes IN PLACE. Bias and output
// weights sit past the end of the feature region, outside the permuted span,
// so the Network view of them remains valid and PSRAM never holds two tables.
// ---------------------------------------------------------------------------

#if !defined(ESP32)
alignas(64) static std::int16_t g_paired_store[2 * 384 * HIDDEN_SIZE];
#endif

static std::int16_t *g_paired = nullptr;

namespace {

constexpr int PAIRED_ROWS = 384;

// Pair j = 64*piece + t stores own-view row (64*piece + t) in half 0 and the
// matching other-view row (384 + 64*piece + t^56) in half 1 - the square
// component flips between perspectives, so the partner of row j is not
// simply j + 384 but j shifted by the square-flip delta.
inline uint32_t other_half_source_row(uint32_t j)
{
	const uint32_t t = j % 64;
	return uint32_t(PAIRED_ROWS) + j + (t ^ 56u) - t;
}

// destination element index -> source element index, within the feature region
inline uint32_t paired_source(uint32_t d)
{
	const uint32_t j    = d / (2u * HIDDEN_SIZE);
	const uint32_t r    = d % (2u * HIDDEN_SIZE);
	const uint32_t half = r / HIDDEN_SIZE;
	const uint32_t l    = r % HIDDEN_SIZE;
	return (half != 0 ? other_half_source_row(j) : j) * HIDDEN_SIZE + l;
}

[[maybe_unused]] void permute_rows(std::int16_t *dst, const std::int16_t *src)
{
	for (uint32_t j = 0; j < uint32_t(PAIRED_ROWS); j++) {
		memcpy(dst + (2u * j) * HIDDEN_SIZE,         src +  j * HIDDEN_SIZE,                 HIDDEN_SIZE * sizeof(std::int16_t));
		memcpy(dst + (2u * j + 1) * HIDDEN_SIZE,     src + other_half_source_row(j) * HIDDEN_SIZE, HIDDEN_SIZE * sizeof(std::int16_t));
	}
}

void permute_rows_in_place(std::int16_t *a)
{
	constexpr uint32_t n_elems = uint32_t(PAIRED_ROWS) * 2 * HIDDEN_SIZE;

	std::vector<bool> done(n_elems, false);

	for (uint32_t i = 0; i < n_elems; i++) {
		if (done[i])
			continue;
		uint32_t     j   = i;
		std::int16_t tmp = a[j];
		while (true) {
			uint32_t s = paired_source(j);
			done[j] = true;
			if (s == i)
				break;
			a[j] = a[s];
			j    = s;
		}
		a[j] = tmp;
	}
}

} // namespace

#if defined(ESP32)

namespace nnue_k {

bool pie_ok = true;

// Exact mod-2^16 int16 accumulator updates with the PIE unit. The EE unit
// has no wrapping int16 vector add (VADDS saturates by spec), so each 128-bit
// chunk is zero-widened to s32 via EE.VZIP.16 against a fresh EE.ZERO.Q
// register, combined with EE.VADDS/VSUBS.S32 (a handful of widened rows can
// never reach 2^31, so saturation cannot fire), and narrowed back with
// EE.VUNZIP.16 - whose even elements are precisely the low 16 bits of each
// widened lane, i.e. the wrapped int16 result. Verified against TRM v1.8
// section 1.8.207/210 pseudocode and simulated exhaustively on host.
//
// Both accumulators ride one loop: four cursors (two read, two write),
// counter in a6 via loopgtz. 17 EE/general ops per 8 lanes per delta pair.

// One delta pair applied to both widened accumulator chunks. Every operand
// is its own post-incrementing cursor: accumulator read (ar/br), accumulator
// write (awr/bwr) and one weight-row cursor per perspective all march
// through their 512-byte spans together, so each is a "+r" in/out.
#define PIE_DUAL_HEAD                                                            \
	"loopgtz %[cnt], 9f\n\t"                                                     \
	"ee.zero.q q6\n\t"                                                           \
	"ee.zero.q q7\n\t"                                                           \
	"ee.vld.128.ip q0, %[ar], 16\n\t"                                            \
	"ee.vld.128.ip q1, %[br], 16\n\t"                                            \
	"ee.vzip.16 q0, q6\n\t"                                                      \
	"ee.vzip.16 q1, q7\n\t"

#define PIE_DUAL_TAIL                                                            \
	"ee.vunzip.16 q0, q6\n\t"                                                    \
	"ee.vunzip.16 q1, q7\n\t"                                                    \
	"ee.vst.128.ip q0, %[aw], 16\n\t"                                            \
	"ee.vst.128.ip q1, %[bw], 16\n\t"                                            \
	"9:\n\t"

#define PIE_DUAL_STEP(OP, WA, WB)                                                \
	"ee.zero.q q3\n\t"                                                           \
	"ee.vld.128.ip q2, %[" #WA "], 16\n\t"                                       \
	"ee.vzip.16 q2, q3\n\t"                                                      \
	"ee.zero.q q5\n\t"                                                           \
	"ee.vld.128.ip q4, %[" #WB "], 16\n\t"                                       \
	"ee.vzip.16 q4, q5\n\t"                                                      \
	"ee." #OP ".s32 q0, q0, q2\n\t"                                              \
	"ee." #OP ".s32 q6, q6, q3\n\t"                                              \
	"ee." #OP ".s32 q1, q1, q4\n\t"                                              \
	"ee." #OP ".s32 q7, q7, q5\n\t"

void IRAM_ATTR pie_dual_add(std::int16_t *aw, std::int16_t *ab, const std::int16_t *wa, const std::int16_t *wb)
{
	std::int16_t       *awc = aw;
	std::int16_t       *bwc = ab;
	const std::int16_t *arc = aw;
	const std::int16_t *brc = ab;
	const std::int16_t *wac = wa;
	const std::int16_t *wbc = wb;
	register int cnt   __asm__("a6") = HIDDEN_SIZE / 8;

	__asm__ volatile(
		PIE_DUAL_HEAD
		PIE_DUAL_STEP(vadds, wac, wbc)
		PIE_DUAL_TAIL
		: [aw] "+r"(awc), [bw] "+r"(bwc), [ar] "+r"(arc), [br] "+r"(brc),
		  [wac] "+r"(wac), [wbc] "+r"(wbc)
		: [cnt] "r"(cnt)
		: "memory");
}

void IRAM_ATTR pie_dual_sub(std::int16_t *aw, std::int16_t *ab, const std::int16_t *wa, const std::int16_t *wb)
{
	std::int16_t       *awc = aw;
	std::int16_t       *bwc = ab;
	const std::int16_t *arc = aw;
	const std::int16_t *brc = ab;
	const std::int16_t *wac = wa;
	const std::int16_t *wbc = wb;
	register int cnt   __asm__("a6") = HIDDEN_SIZE / 8;

	__asm__ volatile(
		PIE_DUAL_HEAD
		PIE_DUAL_STEP(vsubs, wac, wbc)
		PIE_DUAL_TAIL
		: [aw] "+r"(awc), [bw] "+r"(bwc), [ar] "+r"(arc), [br] "+r"(brc),
		  [wac] "+r"(wac), [wbc] "+r"(wbc)
		: [cnt] "r"(cnt)
		: "memory");
}

void IRAM_ATTR pie_dual_1s1a(std::int16_t *aw, std::int16_t *ab,
                             const std::int16_t *sw, const std::int16_t *sb,
                             const std::int16_t *aa, const std::int16_t *ab2)
{
	std::int16_t       *awc = aw;
	std::int16_t       *bwc = ab;
	const std::int16_t *arc = aw;
	const std::int16_t *brc = ab;
	const std::int16_t *s0  = sw;
	const std::int16_t *s1  = sb;
	const std::int16_t *a0  = aa;
	const std::int16_t *a1  = ab2;
	register int cnt   __asm__("a6") = HIDDEN_SIZE / 8;

	__asm__ volatile(
		PIE_DUAL_HEAD
		PIE_DUAL_STEP(vsubs, s0, s1)
		PIE_DUAL_STEP(vadds, a0, a1)
		PIE_DUAL_TAIL
		: [aw] "+r"(awc), [bw] "+r"(bwc), [ar] "+r"(arc), [br] "+r"(brc),
		  [s0] "+r"(s0), [s1] "+r"(s1), [a0] "+r"(a0), [a1] "+r"(a1)
		: [cnt] "r"(cnt)
		: "memory");
}

void IRAM_ATTR pie_dual_2s1a(std::int16_t *aw, std::int16_t *ab,
                             const std::int16_t *sw1, const std::int16_t *sb1,
                             const std::int16_t *sw2, const std::int16_t *sb2,
                             const std::int16_t *aa, const std::int16_t *ab2)
{
	std::int16_t       *awc = aw;
	std::int16_t       *bwc = ab;
	const std::int16_t *arc = aw;
	const std::int16_t *brc = ab;
	const std::int16_t *s0  = sw1;
	const std::int16_t *s1  = sb1;
	const std::int16_t *t0  = sw2;
	const std::int16_t *t1  = sb2;
	const std::int16_t *a0  = aa;
	const std::int16_t *a1  = ab2;
	register int cnt   __asm__("a6") = HIDDEN_SIZE / 8;

	__asm__ volatile(
		PIE_DUAL_HEAD
		PIE_DUAL_STEP(vsubs, s0, s1)
		PIE_DUAL_STEP(vsubs, t0, t1)
		PIE_DUAL_STEP(vadds, a0, a1)
		PIE_DUAL_TAIL
		: [aw] "+r"(awc), [bw] "+r"(bwc), [ar] "+r"(arc), [br] "+r"(brc),
		  [s0] "+r"(s0), [s1] "+r"(s1), [t0] "+r"(t0), [t1] "+r"(t1),
		  [a0] "+r"(a0), [a1] "+r"(a1)
		: [cnt] "r"(cnt)
		: "memory");
}

// Boot-time insurance: run every kernel against the scalar reference over
// wraparound-heavy pseudo-random data. Any disagreement flips pie_ok and the
// engine quietly falls back to the C loops for the rest of the session.
//
// The staged probes exist because the first hardware run disagreed somewhere
// in the widen-add-narrow pipeline: one boot with per-stage diagnostics beats
// five blind flash-and-pray cycles.
void pie_selftest()
{
	alignas(16) std::int16_t aw[2][HIDDEN_SIZE];
	alignas(16) std::int16_t ab[2][HIDDEN_SIZE];
	alignas(16) std::int16_t wa[HIDDEN_SIZE];
	alignas(16) std::int16_t wb[HIDDEN_SIZE];

	uint32_t rng = 0x1234abcd;
	auto next = [&rng]() {
		rng = rng * 1664525u + 1013904223u;
		return static_cast<std::int16_t>((rng >> 8) & 0xffffu);
	};

	for (int i = 0; i < HIDDEN_SIZE; i++) {
		aw[0][i] = next();
		ab[0][i] = next();
		wa[i]    = next();
		wb[i]    = next();
	}
	aw[0][0] = INT16_MAX;   aw[0][1] = INT16_MIN;   wa[0] = INT16_MIN;   wa[1] = INT16_MAX;

	auto fail = [](const char *stage, const std::int16_t *got, const std::int16_t *want) {
		printf("# PIE %s FAILED:", stage);
		for (int i = 0; i < 8 && i < HIDDEN_SIZE; i++)
			printf(" [%d] %d!=%d", i, int(got[i]), int(want[i]));
		printf("\n");
		pie_ok = false;
	};

	{
		// Stage 1: bare load/store roundtrip through one q register pair.
		memcpy(aw[1], aw[0], sizeof(aw[0]));
		std::int16_t       *dst = aw[1];
		const std::int16_t *src = aw[0];
		register int cnt __asm__("a6") = HIDDEN_SIZE / 8;
		asm volatile(
			"loopgtz %[cnt], 9f\n\t"
			"ee.vld.128.ip q0, %[src], 16\n\t"
			"ee.vst.128.ip q0, %[dst], 16\n\t"
			"9:\n\t"
			: [src] "+r"(src), [dst] "+r"(dst)
			: [cnt] "r"(cnt)
			: "memory");
		if (memcmp(aw[1], aw[0], sizeof(aw[0])) != 0)
			return fail("ld/st roundtrip", aw[1], aw[0]);
	}

	{
		// Stage 2: zip/unzip roundtrip, no arithmetic - pins the lane
		// permutation convention without any add/sub involved.
		memcpy(aw[1], aw[0], sizeof(aw[0]));
		std::int16_t       *dst = aw[1];
		const std::int16_t *src = aw[0];
		register int cnt __asm__("a6") = HIDDEN_SIZE / 8;
		asm volatile(
			"loopgtz %[cnt], 9f\n\t"
			"ee.zero.q q6\n\t"
			"ee.zero.q q7\n\t"
			"ee.vld.128.ip q0, %[src], 16\n\t"
			"ee.vzip.16 q0, q6\n\t"
			"ee.vunzip.16 q0, q6\n\t"
			"ee.vst.128.ip q0, %[dst], 16\n\t"
			"9:\n\t"
			: [src] "+r"(src), [dst] "+r"(dst)
			: [cnt] "r"(cnt)
			: "memory");
		if (memcmp(aw[1], aw[0], sizeof(aw[0])) != 0)
			return fail("zip/unzip roundtrip", aw[1], aw[0]);
	}

	{
		// Stage 3: widen-add-narrow on one accumulator, one row.
		alignas(16) std::int16_t exp[HIDDEN_SIZE];
		for (int i = 0; i < HIDDEN_SIZE; i++)
			exp[i] = static_cast<std::int16_t>(aw[0][i] + wa[i]);
		std::int16_t       *acc = aw[0];
		const std::int16_t *w   = wa;
		register int cnt __asm__("a6") = HIDDEN_SIZE / 8;
		asm volatile(
			"loopgtz %[cnt], 9f\n\t"
			"ee.zero.q q6\n\t"
			"ee.zero.q q7\n\t"
			"ee.vld.128.ip q0, %[arc], 16\n\t"
			"ee.vzip.16 q0, q6\n\t"
			"ee.zero.q q3\n\t"
			"ee.vld.128.ip q2, %[wc], 16\n\t"
			"ee.vzip.16 q2, q3\n\t"
			"ee.vadds.s32 q0, q0, q2\n\t"
			"ee.vadds.s32 q6, q6, q3\n\t"
			"ee.vunzip.16 q0, q6\n\t"
			"ee.vst.128.ip q0, %[acw], 16\n\t"
			"9:\n\t"
			: [arc] "+r"(acc), [wc] "+r"(w)
			: [acw] "r"(aw[1]), [cnt] "r"(cnt)
			: "memory");
		if (memcmp(aw[1], exp, sizeof(exp)) != 0)
			return fail("single widen-add", aw[1], exp);
	}

	alignas(16) std::int16_t expw[HIDDEN_SIZE];
	alignas(16) std::int16_t expb[HIDDEN_SIZE];

	for (int t = 0; t < 32; t++) {
		for (int i = 0; i < HIDDEN_SIZE; i++) {
			aw[0][i] = next();
			ab[0][i] = next();
			wa[i]    = next();
			wb[i]    = next();
		}
		if (t % 8 == 0) {           // poke the corners
			aw[0][t] = INT16_MAX;   aw[0][t + 1] = INT16_MIN;
			wa[t]    = INT16_MIN;   wa[t + 1]    = INT16_MAX;
		}

		memcpy(aw[1], aw[0], sizeof(aw[0]));
		memcpy(ab[1], ab[0], sizeof(ab[0]));

		for (int i = 0; i < HIDDEN_SIZE; i++) {
			expw[i] = static_cast<std::int16_t>(aw[1][i] + wa[i]);
			expb[i] = static_cast<std::int16_t>(ab[1][i] + wb[i]);
		}
		pie_dual_add(aw[0], ab[0], wa, wb);
		if (memcmp(aw[0], expw, sizeof(expw)) != 0 || memcmp(ab[0], expb, sizeof(expb)) != 0)
			return fail("pie_dual_add", aw[0], expw);

		for (int i = 0; i < HIDDEN_SIZE; i++) {
			expw[i] = static_cast<std::int16_t>(aw[1][i] - wa[i]);
			expb[i] = static_cast<std::int16_t>(ab[1][i] - wb[i]);
		}
		pie_dual_sub(aw[0], ab[0], wa, wb);
		pie_dual_sub(aw[0], ab[0], wa, wb);
		if (memcmp(aw[0], expw, sizeof(expw)) != 0 || memcmp(ab[0], expb, sizeof(expb)) != 0)
			return fail("pie_dual_sub", aw[0], expw);

		memcpy(aw[1], aw[0], sizeof(aw[0]));
		memcpy(ab[1], ab[0], sizeof(ab[0]));

		for (int i = 0; i < HIDDEN_SIZE; i++) {
			expw[i] = static_cast<std::int16_t>(aw[1][i] - wa[i]);
			expb[i] = static_cast<std::int16_t>(ab[1][i] - wb[i]);
		}
		pie_dual_1s1a(aw[0], ab[0], wa, wb, wa, wb);
		pie_dual_2s1a(aw[0], ab[0], wa, wb, wa, wb, wa, wb);
		if (memcmp(aw[0], expw, sizeof(expw)) != 0 || memcmp(ab[0], expb, sizeof(expb)) != 0)
			return fail("pie_dual_1s1a/2s1a", aw[0], expw);
	}
}

} // namespace nnue_k

namespace {

// Exercise apply()'s shape decomposition against the scalar reference,
// including the scrambled orders real move types produce (en-passant is
// sub,add,sub before canonicalization; castle is sub,add,sub,add). The
// kernel selftests above validate the asm in isolation - this one validates
// how batches get mapped onto it. Any failure flips pie_ok and the engine
// runs the order-agnostic C path.
void pie_decomposition_selftest()
{
	alignas(16) std::int16_t aw[2][HIDDEN_SIZE];
	alignas(16) std::int16_t ab[2][HIDDEN_SIZE];
	alignas(16) std::int16_t rw[5][HIDDEN_SIZE];
	alignas(16) std::int16_t rb[5][HIDDEN_SIZE];

	uint32_t rng = 0x0ddba11;
	auto next = [&rng]() {
		rng = rng * 1664525u + 1013904223u;
		return static_cast<std::int16_t>((rng >> 8) & 0xffffu);
	};

	// The shapes real callers produce, including scrambled orders: ep
	// (S,A,S), castle (S,A,S,A), plain pair, set()-style add batch,
	// scrambled + canonical captures, scrambled castle.
	const int n_ops_list[]    = { 3, 4, 2, 5, 3, 3, 4 };
	const bool pattern[][5]   = { { false, true,  false, true,  false },
		                            { false, true,  false, true,  false },
		                            { true,  false, true,  false, false },
		                            { true,  true,  true,  true,  true },
		                            { true,  false, false, false, false },
		                            { false, false, true,  false, false },
		                            { true,  false, false, true,  false } };

	for (int c = 0; c < 7; c++) {
		const int n = n_ops_list[c];

		for (int i = 0; i < HIDDEN_SIZE; i++) {
			aw[0][i] = next();
			ab[0][i] = next();
			for (int j = 0; j < n; j++) {
				rw[j][i] = next();
				rb[j][i] = next();
			}
		}
		memcpy(aw[1], aw[0], sizeof(aw[0]));
		memcpy(ab[1], ab[0], sizeof(ab[0]));

		nnue_k::Delta deltas[5];
		for (int j = 0; j < n; j++) {
			deltas[j].a   = rw[j];
			deltas[j].b   = rb[j];
			deltas[j].add = pattern[c][j];
		}
		nnue_k::apply(aw[0], ab[0], deltas, n);

		for (int i = 0; i < HIDDEN_SIZE; i++) {
			std::int16_t ew = aw[1][i];
			std::int16_t eb = ab[1][i];
			for (int j = 0; j < n; j++) {
				ew = pattern[c][j] ? static_cast<std::int16_t>(ew + rw[j][i])
			                           : static_cast<std::int16_t>(ew - rw[j][i]);
				eb = pattern[c][j] ? static_cast<std::int16_t>(eb + rb[j][i])
			                           : static_cast<std::int16_t>(eb - rb[j][i]);
			}
			if (aw[0][i] != ew || ab[0][i] != eb) {
				printf("# PIE apply() decomposition mismatch (shape %d)\n", c);
				nnue_k::pie_ok = false;
				return;
			}
		}
	}
}

} // namespace

#endif // ESP32

#if defined(ESP32)
// Copy the weight table into PSRAM at boot and pair-permute it there. Rows
// are 512B each; a 16-byte-aligned base keeps every row aligned for the PIE
// loads. The permutation touches only the feature region; bias and output
// weights live past its end at fixed offsets and stay valid for the Network
// view without any stashing.
void IRAM_ATTR nnue_load_weights_to_psram()
{
	const size_t n = sizeof(weights_data);
	void *mem = heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (mem == nullptr) {
		printf("# PSRAM weight copy failed (%u bytes) - using flash XIP\n", unsigned(n));
		return;
	}
	memcpy(mem, weights_data, n);

	g_paired = static_cast<std::int16_t *>(mem);
	permute_rows_in_place(g_paired);

	printf("# weights in PSRAM, paired layout: %u bytes\n", unsigned(n));
}

void ensure_init()
{
	static std::once_flag once;
	std::call_once(once, [] {
		// nnue_load_weights_to_psram() runs before anything else can get
		// here and leaves g_paired pointing at the permuted PSRAM copy. If
		// that allocation failed, g_paired stays null and rows resolve
		// against the flat blob instead - slower addressing, same values.
		if (g_paired != nullptr) {
			nnue_k::pie_selftest();
			pie_decomposition_selftest();
			if (!nnue_k::pie_ok)
				printf("# PIE kernel selftest failed - using scalar eval updates\n");
		}
	});
}
#else
void ensure_init()
{
	static std::once_flag once;
	std::call_once(once, [] {
		alignas(64) static std::int16_t paired_store[2 * 384 * HIDDEN_SIZE];
		g_paired = paired_store;
		permute_rows(g_paired, reinterpret_cast<const std::int16_t *>(weights_data));
	});
}
#endif

Eval::Eval()
{
	ensure_init();
	reset();
}

Eval::Eval(const libchess::Position & pos)
{
	ensure_init();
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

	nnue_k::Delta deltas[32];
	int           n = 0;

	for (libchess::PieceType type : libchess::constants::PIECE_TYPES) {
		libchess::Bitboard bb = pos.piece_type_bb(type, libchess::constants::WHITE);
		while (bb) {
			libchess::Square sq = bb.forward_bitscan();
			bb.forward_popbit();
			push_delta(deltas, n, type, sq.value(), true, true);
		}

		bb = pos.piece_type_bb(type, libchess::constants::BLACK);
		while (bb) {
			libchess::Square sq = bb.forward_bitscan();
			bb.forward_popbit();
			push_delta(deltas, n, type, sq.value(), false, true);
		}
	}

	nnue_k::apply(this->white.vals.data(), this->black.vals.data(), deltas, n);
}

int IRAM_ATTR Eval::evaluate(const bool white_to_move) const
{
	if (white_to_move)
		return NNUE->evaluate(this->white, this->black);

	return NNUE->evaluate(this->black, this->white);
}

// Resolve the paired rows touched by a piece event. A white event reads pair
// k = 64*piece + square with the own-view half feeding the white accumulator;
// a black event reads pair k = 64*piece + square^56 with the own-view half
// feeding the black accumulator. Identical rows to the old flat indexing -
// see the pairing diagram above. When no paired table exists (PSRAM
// allocation failed on the board), rows fall back to the flat blob; slower
// addressing, bit-identical values.
void IRAM_ATTR Eval::push_delta(nnue_k::Delta *deltas, int &n, const int piece, const int square, const bool is_white, const bool add) const
{
	assert(piece >= 0 && piece < 6);
	assert(square >= 0 && square < 64);

	const int flipped   = square ^ 56;
	const int k         = 64 * piece + (is_white ? square : flipped);
	const int half_own  = is_white ? 0 : 1;

	const size_t off_w = (size_t)(k * 2 + half_own) * HIDDEN_SIZE;
	const size_t off_b = (size_t)(k * 2 + (1 - half_own)) * HIDDEN_SIZE;

	auto & d = deltas[n++];
	if (g_paired != nullptr) {
		d.a = g_paired + off_w;
		d.b = g_paired + off_b;
	}
	else {
		// Flat-blob fallback (board boot without PSRAM). Pair j stores
		// own-view row j in half 0 and other-view row
		// PAIRED_ROWS+j+(j%64 ^ 56)-(j%64) in half 1 - the square flips
		// between perspectives, so neither half resolves to a plain k+384.
		const uint32_t t   = uint32_t(k) % 64;
		const int      own = k;
		const int      oth = int(PAIRED_ROWS + k + (t ^ 56u) - t);
		d.a = NNUE->feature_weights[(half_own ? oth : own)].vals.data();
		d.b = NNUE->feature_weights[(half_own ? own : oth)].vals.data();
	}
	d.add = add;
}

void IRAM_ATTR Eval::add_piece(const int piece, const int square, const bool is_white)
{
	nnue_k::Delta d[1];
	int           n = 0;
	push_delta(d, n, piece, square, is_white, true);
	nnue_k::apply(this->white.vals.data(), this->black.vals.data(), d, n);
}

void IRAM_ATTR Eval::remove_piece(const int piece, const int square, const bool is_white)
{
	nnue_k::Delta d[1];
	int           n = 0;
	push_delta(d, n, piece, square, is_white, false);
	nnue_k::apply(this->white.vals.data(), this->black.vals.data(), d, n);
}
