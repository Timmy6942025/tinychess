#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <libchess/Position.h>
#include "tt.h"

#define EXP_TABLE_ENABLED 1

#if defined(ESP32)
#include <esp_attr.h>
#else
#define IRAM_ATTR
#endif

// 12-byte entry: hash32 + score + depth + flags + move (18-bit effective)
// Fits ~87381 entries per MB (spec target 131072 for 8-byte, we trade move storage for depth-preferred)
// Keep packed to 12 bytes to store full move.
typedef struct __attribute__((packed)) {
    uint32_t hash;
    int16_t  score;
    uint8_t  depth;
    uint8_t  flags; // tt_entry_flag
    uint32_t move; // libchessmove_to_uint, 0 if none
} exp_entry;

static_assert(sizeof(exp_entry) == 12, "exp_entry must be 12 bytes");

namespace exp_table {

extern exp_entry *g_entries;
extern size_t g_n_entries;
extern bool g_enabled;
extern bool g_allocated_psram;
extern bool g_inited;

bool init();
void free_table();
void clear();
bool load();
bool save();

inline size_t n_entries() { return g_n_entries; }

// fastrange32 like tt.cpp
inline uint32_t fastrange32(uint32_t word, uint32_t p) {
    return (uint64_t(word) * uint64_t(p)) >> 32;
}

// lookup: direct mapped, check hash equality
inline std::optional<exp_entry> IRAM_ATTR lookup(uint64_t hash) {
    if (!g_entries || !g_enabled) return {};
    uint32_t h32 = uint32_t(hash & 0xFFFFFFFFull);
    if (g_n_entries == 0) return {};
    uint32_t idx = fastrange32(h32, uint32_t(g_n_entries));
    exp_entry &e = g_entries[idx];
    if (e.hash == h32) return e;
    return {};
}

// for move ordering: returns move if hit
inline std::optional<libchess::Move> IRAM_ATTR probe_move(uint64_t hash) {
    auto e = lookup(hash);
    if (e.has_value() && e->move) {
        return uint_to_libchessmove(e->move);
    }
    return {};
}

// store: depth-preferred, keep deeper entry
inline void IRAM_ATTR store(uint64_t hash, int depth, int score, const libchess::Move &m, tt_entry_flag flags) {
    if (!g_entries || !g_enabled) return;
    if (g_n_entries == 0) return;
    uint32_t h32 = uint32_t(hash & 0xFFFFFFFFull);
    uint32_t idx = fastrange32(h32, uint32_t(g_n_entries));
    exp_entry &e = g_entries[idx];
    // depth-preferred: keep deeper entries, do not overwrite deep with shallow
    if (e.hash != 0 && e.hash != h32 && e.depth > depth) return;
    if (e.hash == h32 && e.depth > depth) return;
    e.hash = h32;
    e.score = int16_t(score);
    e.depth = uint8_t(depth);
    e.flags = uint8_t(flags);
    e.move = libchessmove_to_uint(m);
}

inline void IRAM_ATTR store(uint64_t hash, int depth, int score, tt_entry_flag flags) {
    if (!g_entries || !g_enabled) return;
    if (g_n_entries == 0) return;
    uint32_t h32 = uint32_t(hash & 0xFFFFFFFFull);
    uint32_t idx = fastrange32(h32, uint32_t(g_n_entries));
    exp_entry &e = g_entries[idx];
    if (e.hash != 0 && e.hash != h32 && e.depth > depth) return;
    if (e.hash == h32 && e.depth > depth) return;
    // keep existing move if any?
    uint32_t keepMove = e.hash == h32 ? e.move : 0;
    e.hash = h32;
    e.score = int16_t(score);
    e.depth = uint8_t(depth);
    e.flags = uint8_t(flags);
    e.move = keepMove;
}

} // namespace exp_table
