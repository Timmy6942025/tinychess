#pragma once

#include <cstdint>
#include <optional>

#include <libchess/Position.h>


#define __PRAGMA_PACKED__ __attribute__ ((__packed__))

typedef enum { NOTVALID = 0, EXACT = 1, LOWERBOUND = 2, UPPERBOUND = 3 } tt_entry_flag;

typedef struct __PRAGMA_PACKED__
{
	uint16_t hash;
	int16_t  score;
	uint8_t  depth  : 8;
	uint32_t M      : 18;
	uint8_t  flags  : 2;
	uint8_t  age    : 2;
	uint8_t  filler : 2;
} tt_entry;

class tt
{
private:
	tt_entry *entries { nullptr };
	// C11 L0: small SRAM front cache (128 KB, 2-way) in front of the PSRAM TT
	tt_entry *l0 { nullptr };
	static constexpr uint64_t L0_ENTRIES = 16384;
	uint8_t   generation { 1 };  // incremented per ID iteration; ages TT entries
#if defined(ESP32)
#define ESP32_TT_RAM_SIZE 98304
#define ESP32_DEFAULT_TT_SIZE (4 * 1024l * 1024l)
	uint64_t n_entries { ESP32_DEFAULT_TT_SIZE / sizeof(tt_entry) };
#elif defined(__ANDROID__)
	uint64_t n_entries { 16 * 1024 * 1024  / sizeof(tt_entry) };
#elif defined(linux) || defined(_WIN32) || defined(__APPLE__)
	uint64_t n_entries { 16 * 1024 * 1024  / sizeof(tt_entry) };  // as requested, because of OpenBench testing
#endif
	void allocate();

public:
	tt();
	~tt();

	void     debug_helper();
	void     reset();
	void     set_size(const uint64_t s);
	void     new_search();
	int      get_size() const;  // in MB
	uint64_t get_n   () const;
	int      get_per_mille_filled() const;

	std::optional<tt_entry> lookup(const uint64_t board_hash);
	void store(const uint64_t hash, const tt_entry_flag f, const int d, const int score, const libchess::Move & m);
	void store(const uint64_t hash, const tt_entry_flag f, const int d, const int score);
};

int eval_to_tt  (const int eval, const int ply);
int eval_from_tt(const int eval, const int ply);
uint32_t       libchessmove_to_uint(const libchess::Move & m);
libchess::Move uint_to_libchessmove(const uint32_t v);

extern tt tti;
