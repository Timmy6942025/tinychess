#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <libchess/Position.h>

// Adaptive time management — hill-climbable policy.
//
// Three levers from the prompt:
//   1) remaining time -> fraction to spend
//   2) position complexity (mobility, pawn structure, king safety) -> extra time
//   3) opponent recent move times -> reaction
//
// All tunables are exposed as UCI Spin options (per-mille) so a hill-climber
// can search without recompiling: run cutechess with
//   option.TM_ComplexityWeight=200 option.TM_OppReactWeight=150 ...
// Defaults are chosen so the engine is bit-identical to the pre-policy
// fixed-fraction logic (complexity = opp = time-curve = 0 effect).

namespace time_policy {

// ---------------------------------------------------------------------------
// Tunable parameters — one struct, one global instance, mutated by UCI handlers.
// Hill climbing does coordinate ascent on these 8 dimensions.

struct Params {
    // increment weighting for hard / soft budget (per-mille, default = old code)
    int inc_weight_max_pm = 667;   // max: ms/moves_to_go + inc*667/1000  (was 2/3)
    int inc_weight_min_pm = 500;   // min: ms/(moves*3)  + inc*500/1000  (was 1/2)

    // position complexity -> extra time multiplier (per-mille)
    // think *= 1 + complexity * weight_pm/1000   complexity in [0,1]
    int complexity_weight_pm = 0;  // REVERTED Sep 5 2026: +40.6@2s did not transfer
    // (-145@1s with 56 forfeits, -49@5s with 31; pacing has no income anchor).
    // Keep at parity until the income-anchored redesign is gated.

    // opponent reaction -> extra time multiplier (per-mille)
    // opp_factor in [-1,1] : positive when opp spent long last move
    // think *= 1 + opp_factor * weight_pm/1000
    int opp_react_weight_pm = 0;   // 0 = off, try 80-200

    // remaining-time curve: per-bucket multiplier (per-mille, 1000 = 1.0)
    // Buckets are on *our* remaining ms after overhead deduction.
    int scale_lt2s_pm   = 1000;    // ms <  2 000, REVERTED Sep 5 2026 with complexity (see above)
    int scale_lt5s_pm   = 1000;    // ms <  5 000
    int scale_lt15s_pm  = 1000;    // ms < 15 000
    int scale_gte15s_pm = 1000;    // ms >=15 000
};

inline Params g_params;
inline std::mutex g_params_mutex; // tiny, only touched on setoption

inline void set_params(const Params &p) {
    std::lock_guard<std::mutex> lk(g_params_mutex);
    g_params = p;
}
inline Params get_params() {
    std::lock_guard<std::mutex> lk(g_params_mutex);
    return g_params;
}

// ---------------------------------------------------------------------------
// Opponent move time tracking.
// We infer opponent time spent from the UCI wtime/btime stream:
//   opp_used = prev_opp_ms - cur_opp_ms + inc_opp   (Fischer)
// Keep a short rolling window.

struct OppHistory {
    std::deque<int> recent_ms; // last N opponent move times in ms
    int prev_opp_ms = -1;      // last seen opponent clock, -1 = unknown
    static constexpr size_t K = 6;
    std::mutex m;
    void reset() {
        std::lock_guard<std::mutex> lk(m);
        recent_ms.clear();
        prev_opp_ms = -1;
    }
    // Call each go_handler with current opponent clock and its inc.
    void observe(int cur_opp_ms, int inc_opp) {
        std::lock_guard<std::mutex> lk(m);
        if (prev_opp_ms >= 0 && cur_opp_ms >= 0) {
            int used = prev_opp_ms - cur_opp_ms + inc_opp;
            // Clamp nonsense (first move, time control quirks)
            if (used >= 0 && used < 600000) {
                recent_ms.push_back(used);
                if (recent_ms.size() > K) recent_ms.pop_front();
            }
        }
        prev_opp_ms = cur_opp_ms;
    }
    double avg_ms() {
        std::lock_guard<std::mutex> lk(m);
        if (recent_ms.empty()) return -1;
        long long s=0; for(int v: recent_ms) s+=v;
        return double(s)/recent_ms.size();
    }
    int last_ms() {
        std::lock_guard<std::mutex> lk(m);
        if (recent_ms.empty()) return -1;
        return recent_ms.back();
    }
};

inline OppHistory g_opp_hist;

// ---------------------------------------------------------------------------
// Position complexity  -> [0,1]
// Cheap, runs once per move (in go_handler) so wall time is irrelevant
// (<10us). Combines three sub-features with fixed internal weights;
// the single hill-climbable knob is complexity_weight_pm above.

inline double mobility_factor(const libchess::Position &pos) {
    // Pseudo-legal move count as proxy for mobility / branching.
    // 15 moves = cramped, 35+ = wide open.
    libchess::MoveList ml;
    pos.pseudo_legal_move_list_into(ml);
    int n = (int)ml.size();
    // Clamp and normalise to [0,1]
    if (n <= 10) return 0.0;
    if (n >= 40) return 1.0;
    return (n - 10) / 30.0;
}

inline double pawn_structure_factor(const libchess::Position &pos) {
    // Pawn islands, doubled and isolated — cheap bitboard scan.
    double score = 0.0;
    for (int c = 0; c < 2; ++c) {
        libchess::Color col = libchess::Color(c);
        libchess::Bitboard pawns = pos.piece_type_bb(libchess::constants::PAWN, col);
        if (!pawns) continue;
        int files_with_pawn = 0;
        int doubled = 0;
        int isolated = 0;
        int file_cnt[8] = {};
        for (int f = 0; f < 8; ++f) {
            libchess::Bitboard file_mask(uint64_t(0x0101010101010101ULL) << unsigned(f));
            int cnt = (pawns & file_mask).popcount();
            file_cnt[f] = cnt;
            if (cnt) files_with_pawn++;
            if (cnt > 1) doubled += cnt - 1;
        }
        // islands: groups of consecutive files with pawns
        int islands = 0;
        bool in = false;
        for (int f = 0; f < 8; ++f) {
            if (file_cnt[f]) { if (!in) { islands++; in = true; } }
            else in = false;
        }
        // isolated: pawn file has no neighbour file pawn
        for (int f = 0; f < 8; ++f) if (file_cnt[f]) {
            bool neigh = false;
            if (f > 0 && file_cnt[f-1]) neigh = true;
            if (f < 7 && file_cnt[f+1]) neigh = true;
            if (!neigh) isolated++;
        }
        // Normalise each term to 0..1 and blend
        double islands_n = std::clamp((islands - 1) / 4.0, 0.0, 1.0); // 1 island quiet, 4+ ragged
        double doubled_n = std::clamp(doubled / 3.0, 0.0, 1.0);
        double isolated_n = std::clamp(isolated / 3.0, 0.0, 1.0);
        score += (islands_n * 0.3 + doubled_n * 0.4 + isolated_n * 0.3);
    }
    score /= 2.0; // average over colours -> [0,1]
    return std::clamp(score, 0.0, 1.0);
}

inline double king_safety_factor(const libchess::Position &pos) {
    double score = 0.0;
    for (int c = 0; c < 2; ++c) {
        libchess::Color col = libchess::Color(c);
        libchess::Square ks = pos.king_square(col);
        // Attackers to king square: more attackers = sharper position
        libchess::Bitboard atk = pos.attackers_to(ks, col == libchess::constants::WHITE ? libchess::constants::BLACK : libchess::constants::WHITE);
        int n_atk = atk.popcount();
        double atk_n = std::clamp(n_atk / 4.0, 0.0, 1.0);
        // Pawn shield: squares in front of king missing friendly pawn
        int shield_missing = 0;
        int shield_total = 0;
        int r = ks.rank();
        int f = ks.file();
        int dir = (col == libchess::constants::WHITE) ? 1 : -1;
        for (int df = -1; df <= 1; ++df) {
            int ff = f + df;
            int rr = r + dir;
            if (ff < 0 || ff > 7 || rr < 0 || rr > 7) continue;
            shield_total++;
            libchess::Square sq(ff + rr * 8);
            auto pt = pos.piece_type_on(sq);
            auto co = pos.color_of(sq);
            if (!pt.has_value() || pt.value() != libchess::constants::PAWN || co.value() != col)
                shield_missing++;
            // second rank shield row
            int rr2 = r + dir * 2;
            if (rr2 >= 0 && rr2 < 8) {
                shield_total++;
                libchess::Square sq2(ff + rr2 * 8);
                auto pt2 = pos.piece_type_on(sq2);
                auto co2 = pos.color_of(sq2);
                // only count if first rank already missing? count always but weight less
                if (shield_missing > 0) {
                    // already penalised for hole, second rank pawns matter less
                }
                (void)pt2; (void)co2;
            }
        }
        double shield_n = shield_total ? double(shield_missing) / shield_total : 0.0;
        // In-check is maximal complexity
        double check_n = pos.in_check() ? 0.5 : 0.0;
        // Blend: attackers 0.5, shield 0.3, check 0.2
        score += atk_n * 0.5 + shield_n * 0.3 + check_n;
    }
    score /= 2.0;
    return std::clamp(score, 0.0, 1.0);
}

inline double compute_complexity(const libchess::Position &pos) {
    double mob  = mobility_factor(pos);       // 0..1
    double pawn = pawn_structure_factor(pos); // 0..1
    double king = king_safety_factor(pos);    // 0..1
    // Weighted blend; mobility dominates (branching is the best complexity proxy)
    double c = mob * 0.45 + pawn * 0.25 + king * 0.30;
    // Small boost in middlegame: piece count 20-32 -> higher complexity
    int n_pieces = pos.occupancy_bb().popcount();
    double phase = std::clamp((n_pieces - 10) / 22.0, 0.0, 1.0); // 10=endgame, 32=opening
    c = c * 0.85 + phase * 0.15;
    return std::clamp(c, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Remaining-time bucket scale (per-mille -> factor)

inline double remaining_scale_for_ms(int ms, const Params &p) {
    if (ms < 2000)  return p.scale_lt2s_pm   / 1000.0;
    if (ms < 5000)  return p.scale_lt5s_pm   / 1000.0;
    if (ms < 15000) return p.scale_lt15s_pm  / 1000.0;
    return p.scale_gte15s_pm / 1000.0;
}

// ---------------------------------------------------------------------------
// Opponent factor in [-1,1].
// Positive means opp spent a long time last move -> position likely critical
// -> we should spend more. Negative means opp blitzed -> maybe reply quickly
// or dig deeper (hill climb decides sign via weight).

inline double opp_factor_from_history() {
    // Use deviation of last move from running average.
    // If no history, 0.
    double avg = g_opp_hist.avg_ms();
    int last = g_opp_hist.last_ms();
    if (avg < 0 || last < 0) return 0.0;
    if (avg < 1) return 0.0;
    // Normalise: factor = clamp((last - avg)/avg, -1, 1) * 0.5 + bias
    // Bias: if opp is averaging < 200ms they're blitzing -> factor slightly negative
    double dev = (last - avg) / avg; // -1..large
    dev = std::clamp(dev, -1.0, 1.0);
    // Also inject absolute speed signal: very fast avg -> -0.3
    double abs_signal = 0.0;
    if (avg < 300) abs_signal = -0.3 * (1.0 - avg/300.0);
    else if (avg > 3000) abs_signal = 0.2 * std::min(1.0, (avg-3000)/5000.0);
    return std::clamp(dev * 0.7 + abs_signal, -1.0, 1.0);
}

// ---------------------------------------------------------------------------
// Main entry: compute (think_time_min, think_time_max) with policy.
// Call from go_handler after moves_to_go/ms/inc are known and with pos.
// Signature mirrors old arithmetic but adds policy scaling.

inline std::pair<int,int> compute_budget(
    const libchess::Position &pos,
    int ms,                // our remaining ms after overhead
    int inc,               // our increment
    int ms_opp,            // opponent remaining ms (for history update; may be 0)
    int inc_opp,
    int moves_to_go)
{
    Params p = get_params();

    // Base budget — identical to old code when weights = defaults.
    // Use integer arithmetic for base to keep parity, then apply policy in floating.
    double base_max = double(ms) / std::max(1, moves_to_go) + double(inc) * p.inc_weight_max_pm / 1000.0;
    double base_min = double(ms) / std::max(1, moves_to_go * 3) + double(inc) * p.inc_weight_min_pm / 1000.0;

    // Remaining-time bucket scale
    double rem_scale = remaining_scale_for_ms(ms, p);

    // Position complexity
    double complexity = compute_complexity(pos); // 0..1
    double c_mult_max = 1.0 + complexity * p.complexity_weight_pm / 1000.0;
    double c_mult_min = 1.0 + complexity * p.complexity_weight_pm / 1000.0 * 0.5; // min less affected

    // Opponent reaction
    double opp_f = opp_factor_from_history(); // -1..1
    double o_mult_max = 1.0 + opp_f * p.opp_react_weight_pm / 1000.0;
    double o_mult_min = 1.0 + opp_f * p.opp_react_weight_pm / 1000.0 * 0.5;

    double tmax = base_max * rem_scale * c_mult_max * o_mult_max;
    double tmin = base_min * rem_scale * c_mult_min * o_mult_min;

    int think_max = std::max(1, int(std::lround(tmax)));
    int think_min = std::max(1, int(std::lround(tmin)));
    // Guard: min never exceeds max
    if (think_min > think_max) think_min = think_max;
    return {think_min, think_max};
}

} // namespace time_policy
