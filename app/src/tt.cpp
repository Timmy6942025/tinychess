#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#if !defined(NDEBUG)
#if !defined(_WIN32) && !defined(ESP32) && !defined(__ANDROID__) && !defined(__APPLE__)
#include <valgrind/helgrind.h>
#endif
#endif
#if defined(linux)
#include <sys/mman.h>
#endif

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

#include "libchess/Position.h"
#include "main.h"
#include "tt.h"


static_assert(sizeof(tt_entry) == 8, "tt_entry must be 8 bytes in size");

tt tti;

tt::tt()
{
	allocate();
	reset();
}

tt::~tt()
{
	free(entries);
	free(l0);
}

void tt::debug_helper()
{
#if !defined(NDEBUG)
#if !defined(_WIN32) && !defined(ESP32) && !defined(__ANDROID__) && !defined(__APPLE__)
	VALGRIND_HG_DISABLE_CHECKING(entries, n_entries * sizeof(tt_entry));
#endif
#endif
}

void tt::allocate()
{
	// C11 L0: 128 KB SRAM front cache. Plain malloc = internal RAM on ESP32.
	// Optional: a null l0 just disables the front cache, the PSRAM TT works alone.
	free(l0);
	l0 = nullptr;
	l0 = reinterpret_cast<tt_entry *>(malloc(L0_ENTRIES * sizeof(tt_entry)));
	if (l0)
		printf("Using %zu bytes of SRAM for L0 TT\n", size_t(L0_ENTRIES * sizeof(tt_entry)));
	else
		printf("# L0 TT malloc failed, PSRAM TT only\n");
#if defined(ESP32)
	size_t requested = n_entries * sizeof(tt_entry);
	size_t psram_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	if (psram_size > ESP32_TT_RAM_SIZE) {
		constexpr const size_t max_sp_size = 6 * 1024l * 1024l;
		size_t take = std::min(requested, std::min(psram_size, max_sp_size));
		n_entries = take / sizeof(tt_entry);
		printf("Using %zu bytes of PSRAM\n", take);
		entries = reinterpret_cast<tt_entry *>(heap_caps_malloc(take, MALLOC_CAP_SPIRAM));
		if (entries)
			return;
		printf("# PSRAM malloc failed, falling back to SRAM\n");
		n_entries = ESP32_TT_RAM_SIZE / sizeof(tt_entry);
	}
	if (!entries) {
		printf("No PSRAM\n");
		n_entries = std::min<uint64_t>(n_entries, ESP32_TT_RAM_SIZE / sizeof(tt_entry));
		for (;;) {
			auto n_bytes = n_entries * sizeof(tt_entry);
			printf("Using %zu bytes of RAM\n", size_t(n_bytes));
			entries = reinterpret_cast<tt_entry *>(malloc(n_bytes));
			if (entries)
				break;
			n_entries = std::max(uint64_t(0), n_entries - 1024 / sizeof(tt_entry));
			if (n_entries == 0)
				break;
		}
	}
#endif
#if !defined(ESP32)
	size_t s = n_entries * sizeof(tt_entry);
#if defined(linux)
	if (posix_memalign(reinterpret_cast<void **>(&entries), 1024 * 1024 * 2, s)) {
		printf("# posix_memalign failed: %s\n", strerror(errno));
		entries = reinterpret_cast<tt_entry *>(malloc(s));
	}
	else {
		if (madvise(entries, s, MADV_HUGEPAGE) == -1)
			printf("# madvise failed: %s\n", strerror(errno));
	}
#else
	entries = reinterpret_cast<tt_entry *>(malloc(s));
#endif
#endif
}

void tt::set_size(const uint64_t s)
{
	n_entries = std::max(uint64_t(2), s / sizeof(tt_entry));
	free(entries);
	entries = nullptr;  // so allocate()'s no-PSRAM fallback is not defeated by a dangling (freed) pointer
	allocate();
	reset();
	printf("# Newly allocated node count: %" PRIu64 "\n", n_entries);
}

int tt::get_size() const
{
	return n_entries * sizeof(tt_entry);
}

uint64_t tt::get_n() const
{
	return n_entries;
}

void tt::reset()
{
	if (entries == nullptr)
		return;

	memset(entries, 0x00, sizeof(tt_entry) * n_entries);
	if (l0)
		memset(l0, 0x00, sizeof(tt_entry) * L0_ENTRIES);
}

// see https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
#if defined(ESP32)
static inline uint32_t fastrange32(uint32_t word, uint32_t p)
{
	return (uint64_t(word) * uint64_t(p)) >> 32;
}
#define fastrange fastrange32
#else
typedef unsigned __int128 uint128_t;
inline uint64_t fastrange64(uint64_t word, uint64_t p)
{
	return (uint128_t(word) * uint128_t(p)) >> 64;
}
#define fastrange fastrange64
#endif

static inline tt_entry * replace_slot(tt_entry *const slots, const uint16_t hash16, const uint8_t generation);

std::optional<tt_entry> IRAM_ATTR tt::lookup(const uint64_t hash)
{
	if (entries == nullptr)
		return { };

	// C11 L0 first: SRAM hit avoids the PSRAM round trip entirely
	if (l0) {
		uint64_t   l0index = fastrange(hash, L0_ENTRIES / 2);
		tt_entry & l0e0    = l0[l0index * 2];
		tt_entry & l0e1    = l0[l0index * 2 + 1];

		if (l0e0.hash == uint16_t(hash))
			return l0e0;
		if (l0e1.hash == uint16_t(hash))
			return l0e1;
	}

	uint64_t   index = fastrange(hash, n_entries / 2);
	tt_entry & e0    = entries[index * 2];
	tt_entry & e1    = entries[index * 2 + 1];

	if (e0.hash == uint16_t(hash)) {
		if (l0) {
			uint64_t   l0index = fastrange(hash, L0_ENTRIES / 2);
			tt_entry * slots = &l0[l0index * 2];
			*replace_slot(slots, uint16_t(hash), generation) = e0;
		}
		return e0;
	}
	if (e1.hash == uint16_t(hash)) {
		if (l0) {
			uint64_t   l0index = fastrange(hash, L0_ENTRIES / 2);
			tt_entry * slots = &l0[l0index * 2];
			*replace_slot(slots, uint16_t(hash), generation) = e1;
		}
		return e1;
	}

	return { };
}

uint32_t libchessmove_to_uint(const libchess::Move & m)
{
	uint32_t v = m.from_square().file() | (m.from_square().rank() << 3) |
		(m.to_square().file() << 6) | (m.to_square().rank() << 9) |
		(int(m.type()) << 15);

	if (m.promotion_piece_type().has_value())
		v |= m.promotion_piece_type().value() << 12;

	return v;
}

libchess::Move uint_to_libchessmove(const uint32_t v)
{
	auto promo = libchess::PieceType((v >> 12) & 7);
	auto from  = libchess::Square::from(libchess::File(v & 7), libchess::Rank((v >> 3) & 7)).value();
	auto to    = libchess::Square::from(libchess::File((v >> 6) & 7), libchess::Rank((v >> 9) & 7)).value();
	auto type  = libchess::Move::Type(v >> 15);

	if (promo)
		return libchess::Move{ from, to, promo, type };

	return libchess::Move{ from, to, type };
}

// pick the slot to evict: a matching hash is always reused, otherwise the
// slot whose (depth - 4 * age) is lowest - i.e. the shallowest entry from
// the oldest search - loses, so deep entries of the previous iterations
// survive the churn of the current one
static inline tt_entry * replace_slot(tt_entry *const slots, const uint16_t hash16, const uint8_t generation)
{
	for (uint32_t i = 0; i < 2; i++) {
		if (slots[i].hash == hash16)
			return &slots[i];
	}

	int      best_val = 1 << 30;
	uint32_t best     = 0;
	for (uint32_t i = 0; i < 2; i++) {
		int rel_age = (generation - slots[i].age) & 3;
		int val     = int(slots[i].depth) - 4 * rel_age;
		if (val < best_val) {
			best_val = val;
			best     = i;
		}
	}
	return &slots[best];
}

void IRAM_ATTR tt::store(const uint64_t hash, const tt_entry_flag f, const int d, const int score, const libchess::Move & m)
{
	if (entries == nullptr)
		return;

	uint64_t   index = fastrange(hash, n_entries / 2);
	tt_entry * slots = &entries[index * 2];

	tt_entry *cur = replace_slot(slots, uint16_t(hash), generation);

	cur->score = int16_t(score);
	cur->depth = uint8_t(d);
	cur->flags = f;
	cur->M     = libchessmove_to_uint(m);
	cur->hash  = uint16_t(hash);
	cur->age   = generation;

	// C11 L0 mirror: every store lands in SRAM too
	if (l0) {
		uint64_t   l0index = fastrange(hash, L0_ENTRIES / 2);
		tt_entry * l0slots = &l0[l0index * 2];
		*replace_slot(l0slots, uint16_t(hash), generation) = *cur;
	}
}

void IRAM_ATTR tt::store(const uint64_t hash, const tt_entry_flag f, const int d, const int score)
{
	if (entries == nullptr)
		return;

	uint64_t   index = fastrange(hash, n_entries / 2);
	tt_entry * slots = &entries[index * 2];

	tt_entry *cur = replace_slot(slots, uint16_t(hash), generation);

	if (cur->hash != uint16_t(hash))
		cur->M = 0;  // keep the move of an existing entry, clear it otherwise

	cur->score = int16_t(score);
	cur->depth = uint8_t(d);
	cur->flags = f;
	cur->hash  = uint16_t(hash);
	cur->age   = generation;

	// C11 L0 mirror
	if (l0) {
		uint64_t   l0index = fastrange(hash, L0_ENTRIES / 2);
		tt_entry * l0slots = &l0[l0index * 2];
		*replace_slot(l0slots, uint16_t(hash), generation) = *cur;
	}
}

void tt::new_search()
{
	generation = (generation + 1) & 3;
	if (generation == 0)
		generation = 1;
}

int tt::get_per_mille_filled() const
{
	int count = 0;
	const int n = std::min(1000, int(n_entries));
	for(int i=0; i<n; i++)
		count += entries[i].hash != 0;
	return count;
}

int eval_to_tt(const int eval, const int ply)
{
	if (eval > max_non_mate)
		return eval + ply;
	if (eval < -max_non_mate)
		return eval - ply;
	return eval;
}

int eval_from_tt(const int eval, const int ply)
{
	if (eval > max_non_mate)
		return eval - ply;
	if (eval < -max_non_mate)
		return eval + ply;
	return eval;
}
