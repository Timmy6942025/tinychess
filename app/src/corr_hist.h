#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <algorithm>
#include <libchess/Position.h>

#define CORR_HIST_ENABLED 1

constexpr size_t CORR_SIZE = 524288; // 1 MB /2 int16 entries total
constexpr size_t CORR_HALF = CORR_SIZE / 2; // per color
constexpr size_t CORR_MASK = CORR_HALF - 1; // AND mask per side, power of two
constexpr int CORR_LIMIT = 1024;
constexpr int CORR_SCALE_NUM = 66;
constexpr int CORR_SCALE_DEN = 512;

#if defined(ESP32)
#include <esp_attr.h>
#else
#define IRAM_ATTR
#endif

namespace corr_hist {

extern int16_t *g_table;
extern bool g_enabled;
extern bool g_allocated_psram;
extern bool g_inited;

bool init();
void free_table();
void clear();
bool load();
bool save();

// fast pawn hash from position's pawn bitboards using Zobrist piece-square keys
inline uint64_t pawn_hash(const libchess::Position &pos) {
    uint64_t h = 0;
    auto wp = pos.piece_type_bb(libchess::constants::PAWN, libchess::constants::WHITE);
    while (wp) {
        auto sq = wp.forward_bitscan();
        wp.forward_popbit();
        h ^= libchess::zobrist::piece_square_key(sq, libchess::constants::PAWN, libchess::constants::WHITE);
    }
    auto bp = pos.piece_type_bb(libchess::constants::PAWN, libchess::constants::BLACK);
    while (bp) {
        auto sq = bp.forward_bitscan();
        bp.forward_popbit();
        h ^= libchess::zobrist::piece_square_key(sq, libchess::constants::PAWN, libchess::constants::BLACK);
    }
    return h;
}

inline int IRAM_ATTR get_corr_raw(int side, uint64_t pawnHash) {
    if (!g_table) return 0;
    size_t idx = pawnHash & CORR_MASK;
    size_t off = side ? (idx + CORR_HALF) : idx;
    return g_table[off];
}

inline int IRAM_ATTR get_correction(int side, uint64_t pawnHash) {
    if (!g_table || !g_enabled) return 0;
    int corr = get_corr_raw(side, pawnHash);
    return (CORR_SCALE_NUM * corr) / CORR_SCALE_DEN;
}

inline int IRAM_ATTR get_correction_for_pos(const libchess::Position &pos) {
    if (!g_table || !g_enabled) return 0;
    int side = pos.side_to_move() == libchess::constants::WHITE ? 0 : 1;
    uint64_t ph = pawn_hash(pos);
    return get_correction(side, ph);
}

// apply to raw eval, clamp to [-max_non_mate, max_non_mate]
inline int IRAM_ATTR apply(int rawEval, const libchess::Position &pos) {
    if (!g_table || !g_enabled) return rawEval;
    int side = pos.side_to_move() == libchess::constants::WHITE ? 0 : 1;
    uint64_t ph = pawn_hash(pos);
    int corr = get_corr_raw(side, ph);
    int adj = (CORR_SCALE_NUM * corr) / CORR_SCALE_DEN;
    int out = rawEval + adj;
    if (out > 29500) out = 29500;
    if (out < -29500) out = -29500;
    return out;
}

// bounded update, gravity 1024
inline void IRAM_ATTR update(const libchess::Position &pos, int error, int depth) {
    if (!g_table || !g_enabled) return;
    if (depth < 2) return;
    int side = pos.side_to_move() == libchess::constants::WHITE ? 0 : 1;
    uint64_t h = pawn_hash(pos);
    size_t idx = h & CORR_MASK;
    size_t off = side ? (idx + CORR_HALF) : idx;
    int bonus = error * depth / 8;
    if (bonus > CORR_LIMIT/4) bonus = CORR_LIMIT/4;
    if (bonus < -CORR_LIMIT/4) bonus = -CORR_LIMIT/4;
    if (bonus == 0) return;
    int cur = g_table[off];
    int delta = bonus - cur * abs(bonus) / CORR_LIMIT;
    int nxt = cur + delta;
    if (nxt > CORR_LIMIT) nxt = CORR_LIMIT;
    if (nxt < -CORR_LIMIT) nxt = -CORR_LIMIT;
    g_table[off] = int16_t(nxt);
}

// overload with explicit pawnHash and side
inline void IRAM_ATTR update_with_hash(uint64_t pawnHash, int side, int error, int depth) {
    if (!g_table || !g_enabled) return;
    if (depth < 2) return;
    size_t idx = pawnHash & CORR_MASK;
    size_t off = side ? (idx + CORR_HALF) : idx;
    int bonus = error * depth / 8;
    if (bonus > CORR_LIMIT/4) bonus = CORR_LIMIT/4;
    if (bonus < -CORR_LIMIT/4) bonus = -CORR_LIMIT/4;
    if (bonus == 0) return;
    int cur = g_table[off];
    int delta = bonus - cur * abs(bonus) / CORR_LIMIT;
    int nxt = cur + delta;
    if (nxt > CORR_LIMIT) nxt = CORR_LIMIT;
    if (nxt < -CORR_LIMIT) nxt = -CORR_LIMIT;
    g_table[off] = int16_t(nxt);
}

// verification helper: AND indexing must equal modulo for power of two
bool verify_and_indexing();

} // namespace corr_hist
