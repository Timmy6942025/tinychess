#if defined(linux) || defined(_WIN32) || defined(__ANDROID__) || defined(__APPLE__)
#include <limits.h>
#include <sys/time.h>
#endif
#include <cinttypes>
#include <cmath>
#include <set>
#include <libchess/Position.h>
#include <libchess/UCIService.h>

#if defined(ESP32)
#include <esp_attr.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#if defined(ESP32)
static volatile uint32_t        es32_yield_gate = 0;
static volatile uint64_t        es32_last_yield = 0;
static volatile TaskHandle_t    es32_yield_waiter = NULL;
static volatile TaskHandle_t    es32_yield_peer = NULL;

void es32_set_yield_peer(TaskHandle_t th)
{
	es32_yield_peer = th;
}
#endif

constexpr bool ORDER_TABLE_READS = true;
#include "eval.h"
#include "corr_hist.h"
#include "exp_table.h"
#include "inbuf.h"
extern bool g_bench_active;
#include "lmr-red.h"
#include "main.h"
#include "max-ascii.h"
#include "search.h"
#include "str.h"
#if defined(linux) || defined(_WIN32) || defined(__APPLE__)
#include "syzygy.h"
#endif
#include "test.h"
#include "tt.h"
#include "tui.h"


std::optional<libchess::Move> str_to_move(const libchess::Position & p, const std::string & m)
{
	auto m_obj = libchess::Move::from(m);
	if (m_obj.has_value() == false)
		return { };

	if (m_obj.value().type() != libchess::Move::Type::NONE)
		return m_obj;

	for(auto m_compare: p.legal_move_list()) {
		if (m_compare == m_obj.value())  // compare is without type
			return m_compare;
	}

	return { };
}

inline int history_index(const libchess::Color & side, const libchess::PieceType & from_type, const libchess::Square & sq)
{
	return side * 6 * 64 + from_type * 64 + sq;
}

inline int capture_history_index(const libchess::Color & side, const libchess::PieceType & pt, const libchess::Square & to, const libchess::PieceType & captured)
{
	int piece = side * 6 + pt.value();
	return piece * 64 * 6 + to.value() * 6 + captured.value();
}

inline int butterfly_history_index(const libchess::Color & side, const libchess::Square & from, const libchess::Square & to)
{
	return side * 64 * 64 + from.value() * 64 + to.value();
}

inline int cont_history_index(const libchess::Square & prev_to, const libchess::Square & curr_to)
{
	return prev_to.value() * 64 + curr_to.value();
}

sort_movelist_compare::sort_movelist_compare(const search_pars_t & sp) : sp(sp)
{
}

void sort_movelist_compare::add_first_move(const libchess::Move move)
{
	assert(move.value());
	first_moves[n_first_moves++] = move;
}

// MVV-LVA + capture history + butterfly/cont for quiets
int IRAM_ATTR sort_movelist_compare::move_evaluater(const libchess::Move move) const
{
	for(int i=0; i<n_first_moves; i++) {
		if (move == first_moves[i])
			return INT_MAX - i;
	}

	int  score      = 0;
	auto piece_from = sp.pos.piece_on(move.from_square());
	if (!piece_from)
		return 0;
	auto from_type  = piece_from->type();
	auto to_type    = from_type;

	if (sp.pos.is_promotion_move(move)) {
		to_type = *move.promotion_piece_type();
		int piece_val = to_type;
		assert(piece_val < 2048);
		score  += piece_val << 19;
	}

	if (sp.pos.is_capture_move(move)) {
		if (move.type() == libchess::Move::Type::ENPASSANT) {
			score += libchess::constants::PAWN << 19;
		}
		else {
			auto piece_to = sp.pos.piece_on(move.to_square());
			if (piece_to) {
				int victim_val = piece_to->type();
				assert(victim_val < 2048);
				score += victim_val << 19;
			}
		}

		if (from_type != libchess::constants::KING) {
			int add = (libchess::constants::QUEEN - from_type) << 8;
			assert(abs(add) < (1 << 19));
			score += add;
		}

		// capture history: learned ordering for captures (never tried here
		// before). Scaled to reorder within a victim class and across the
		// attacker sub-ranks; victim class stays dominant so an unlearned
		// table degrades to plain MVV-LVA.
		if (ORDER_TABLE_READS) {
			libchess::PieceType capType = libchess::constants::PAWN;
			if (move.type() == libchess::Move::Type::ENPASSANT) {
				capType = libchess::constants::PAWN;
			} else {
				auto pt = sp.pos.piece_on(move.to_square());
				if (pt) capType = pt->type();
			}
			int idx = capture_history_index(sp.pos.side_to_move(), from_type, move.to_square(), capType);
			if (idx >=0 && idx < (int)capture_history_size && sp.capture_history)
				score += sp.capture_history[idx] * 16;
		}
	}
	else {
		int idx = history_index(sp.pos.side_to_move(), from_type, move.to_square());
		score += sp.history[idx];
		if (ORDER_TABLE_READS) {
			int bfIdx = butterfly_history_index(sp.pos.side_to_move(), move.from_square(), move.to_square());
			if (bfIdx >=0 && bfIdx < (int)butterfly_history_size && sp.butterfly_history)
				score += sp.butterfly_history[bfIdx];
			auto prev = sp.pos.previous_move();
			if (prev.has_value() && sp.cont_history) {
				libchess::Square prevTo = prev.value().to_square();
				int cIdx = cont_history_index(prevTo, move.to_square());
				if (cIdx >=0 && cIdx < (int)cont_history_size)
					score += sp.cont_history[cIdx];
			}
		}
	}

	return score;
}

bool is_check(libchess::Position & pos)
{
	return pos.attackers_to(pos.piece_type_bb(libchess::constants::KING, !pos.side_to_move()).forward_bitscan(), pos.side_to_move());
}

// https://www.reddit.com/r/chess/comments/se89db/a_writeup_on_definitions_of_insufficient_material/
bool IRAM_ATTR is_insufficient_material_draw(const libchess::Position & pos)
{
	using namespace libchess::constants;

        // A king + any(pawn, rook, queen) is sufficient.
	if (pos.piece_type_bb(PAWN) || pos.piece_type_bb(QUEEN) || pos.piece_type_bb(ROOK))
		return false;

        // A king and more than one other type of piece is sufficient (e.g. knight + bishop).
	if ((pos.piece_type_bb(KNIGHT, WHITE) && pos.piece_type_bb(BISHOP, WHITE)) ||
	    (pos.piece_type_bb(KNIGHT, BLACK) && pos.piece_type_bb(BISHOP, BLACK)))
		return false;

        // A king and two (or more) knights is sufficient
	if (pos.piece_type_bb(KNIGHT, WHITE).popcount() >= 2 ||
	    pos.piece_type_bb(KNIGHT, BLACK).popcount() >= 2)
		return false;

        // King + bishop against king + any(knight, pawn) is sufficient.
        if ((pos.piece_type_bb(BISHOP, WHITE) && (pos.piece_type_bb(KNIGHT, BLACK) || pos.piece_type_bb(PAWN, BLACK))) ||
            (pos.piece_type_bb(BISHOP, BLACK) && (pos.piece_type_bb(KNIGHT, WHITE) || pos.piece_type_bb(PAWN, WHITE)))) {
                return false;
        }

        // King + knight against king + any(rook, bishop, knight, pawn) is sufficient.
        if (((pos.piece_type_bb(ROOK, WHITE) || pos.piece_type_bb(BISHOP, WHITE) || pos.piece_type_bb(KNIGHT, WHITE) || pos.piece_type_bb(PAWN, WHITE))
				&& pos.piece_type_bb(KNIGHT, BLACK)) ||
            ((pos.piece_type_bb(ROOK, BLACK) || pos.piece_type_bb(BISHOP, BLACK) || pos.piece_type_bb(KNIGHT, BLACK) || pos.piece_type_bb(PAWN, BLACK))
	    			&& pos.piece_type_bb(KNIGHT, WHITE)))
		return false;

        // King + bishop(s) is also sufficient if there's bishops on opposite colours (even king + bishop against king + bishop).
        constexpr uint64_t white_squares = 0x55aa55aa55aa55aall;
        constexpr uint64_t black_squares = 0xaa55aa55aa55aa55ll;
        const libchess::Bitboard piece_bb = pos.piece_type_bb(BISHOP);
        if ((piece_bb & black_squares) && (piece_bb & white_squares)) {
                return false;
        }

        return true;
}

void IRAM_ATTR gen_qs_moves_into(libchess::Position & pos, libchess::MoveList & ml)
{
	libchess::Color side = pos.side_to_move();

	if (pos.checkers_to(side)) {
		pos.pseudo_legal_move_list_into(ml);
		return;
	}

	ml.clear();
	pos.generate_promotions(ml, side);
	pos.generate_capture_moves(ml, side);
}

// Static Exchange Evaluation: net material balance of the capture sequence
// starting with `move`, from the perspective of the side to move, assuming
// optimal play (each side may stand pat after its own capture). Greedy
// least-valuable-attacker recursion with x-ray discovery; matches the
// classic swap-list algorithm and Stockfish's see_ge() semantics.
int IRAM_ATTR see_rec(const libchess::Position & pos, const libchess::Bitboard occ, const libchess::Square to, const libchess::Color stm, const int captured, const int depth)
{
	using namespace libchess;
	using namespace libchess::constants;

	if (depth > 31)
		return 0;

	const Bitboard attackers = pos.attackers_to(to, occ) & occ;
	const Bitboard mine      = attackers & pos.color_bb(stm);
	if (!mine)
		return 0;

	// least valuable attacker of the side to move
	int    pt   = KING.value();
	Square from = Square{0};
	for (int p = PAWN.value(); p <= KING.value(); p++) {
		Bitboard cand = mine & pos.piece_type_bb(PieceType{p});
		if (cand) {
			pt   = p;
			from = cand.forward_bitscan();
			break;
		}
	}

	if (pt == KING.value()) {
		// a king capture is only legal if nothing attacks `to` afterwards
		const Bitboard after = occ ^ Bitboard(from);
		if (pos.attackers_to(to, after) & pos.color_bb(!stm) & after)
			return 0;
		return captured;
	}

	const int continuation = see_rec(pos, occ ^ Bitboard(from), to, !stm, piece_values[pt], depth + 1);
	return std::max(0, captured - continuation);
}

int IRAM_ATTR see(const libchess::Position & pos, const libchess::Move & move)
{
	using namespace libchess;
	using namespace libchess::constants;

	// quiet move: only promotions carry material value in qsearch
	// (is_capture_move relies on move.type(), which Move::from() does not
	// set, so also treat any piece on the target square as a capture)
	if (pos.is_capture_move(move) == false && pos.piece_type_on(move.to_square()).has_value() == false) {
		if (move.promotion_piece_type().has_value() == false)
			return 0;
		return piece_values[move.promotion_piece_type().value().value()] - piece_values[0];
	}

	int captured_value = 100;  // en-passant: the captured pawn is not on `to`
	auto captured_pt   = pos.piece_type_on(move.to_square());
	if (captured_pt.has_value())
		captured_value = piece_values[captured_pt.value().value()];

	int moving_value = piece_values[pos.piece_type_on(move.from_square()).value().value()];
	if (move.promotion_piece_type().has_value())
		moving_value = piece_values[move.promotion_piece_type().value().value()];

	// the captured piece leaves the board; for en-passant the captured
	// pawn sits on the rank behind the target square and must be removed
	// too, or it would block x-ray attacks through its square
	Bitboard occ = pos.occupancy_bb() ^ Bitboard(move.from_square());
	if (move.type() == libchess::Move::Type::ENPASSANT) {
		const int ep_sq = move.to_square().value() + (pos.side_to_move() == libchess::constants::WHITE ? -8 : 8);
		occ ^= Bitboard(libchess::Square{ ep_sq });
	}

	return captured_value - see_rec(pos, occ, move.to_square(), !pos.side_to_move(), moving_value, 0);
}

int IRAM_ATTR qs(int alpha, const int beta, const int qsdepth, search_pars_t & sp)
{
#if defined(ESP32)
	if (qsdepth > sp.md) {
		sp.md = qsdepth;
		if (check_min_stack_size(sp)) {
			sp.md_limit = sp.md;
			sp.cs.data.large_stack++;
			return nnue_evaluate(sp.nnue_eval, sp.pos);
		}
	}
	else if (qsdepth >= sp.md_limit) {
		sp.cs.data.large_stack++;
		return nnue_evaluate(sp.nnue_eval, sp.pos);
	}
#endif
	if (qsdepth >= 127)
		return nnue_evaluate(sp.nnue_eval, sp.pos);

	sp.cs.data.qnodes++;
	sp.md = std::max(sp.md, uint16_t(qsdepth));

	if (sp.pos.halfmoves() >= 100 || sp.pos.is_repeat() || is_insufficient_material_draw(sp.pos))  {
		if (sp.pos.in_check()) {
			if (sp.pos.legal_move_list().empty()) {
				sp.cs.win[!sp.pos.side_to_move()]++;
				sp.cs.data.n_checkmate++;
				return -max_eval + qsdepth;
			}
		}
		sp.cs.draw++;
		return 0;
	}

	int            start_alpha = alpha;

	// TT // (probe disabled: 2.6% hit / 2.4% cutoff at ~292 cycles/qnode
	// is net-negative; the qs still stores results for the main search)
	uint64_t hash = sp.pos.hash();
	////////

	int  best_score = -32767;

	bool in_check   = sp.pos.in_check();
	if (!in_check) {
		// standing pat
		best_score = nnue_evaluate(sp.nnue_eval, sp.pos);
		if (best_score > alpha && best_score >= beta) {
			sp.cs.data.n_standing_pat++;
			return best_score;
		}

		alpha = std::max(alpha, best_score);
	}

	int  n_played  = 0;
	libchess::MoveList local_ml;
	std::vector<int>   local_scores;
	node_scratch_t   * qscr = nullptr;
	libchess::MoveList * move_list_ptr = nullptr;
	if (qsdepth < n_qs_scratch_levels) {
		qscr = &sp.scratch[n_search_scratch_levels + qsdepth];
		gen_qs_moves_into(sp.pos, qscr->ml);
		move_list_ptr = &qscr->ml;
	}
	else {
		gen_qs_moves_into(sp.pos, local_ml);
		move_list_ptr = &local_ml;
	}
	auto & move_list = *move_list_ptr;
	std::optional<libchess::Move> m;

	// generate list of scores: SEE order (winning captures first),
	// with the TT move pinned to the front
	size_t           n_moves = move_list.size();
	libchess::Bitboard pinned = sp.pos.pinned_pieces_of(sp.pos.side_to_move());
	std::vector<int> * scores_ptr;
	if (qscr) {
		qscr->scores.clear();
		qscr->scores.resize(n_moves);
		scores_ptr = &qscr->scores;
	}
	else {
		local_scores.resize(n_moves);
		scores_ptr = &local_scores;
	}
	std::vector<int> & move_scores = *scores_ptr;
	for(size_t i=0; i<n_moves; i++) {
		auto & move = *(move_list.begin() + i);
		// pure SEE: this array also drives the SEE<0 prune below, so it must
		// stay bit-exact with the accepted qsearch semantics - no history
		// blending here (a history-tainted score would turn ordering noise
		// into real pruning)
		move_scores[i] = see(sp.pos, move);
	}

	size_t m_idx  = 0;
	while(m_idx < n_moves) {
		size_t selected_idx = m_idx;
		for(size_t i=m_idx; i<n_moves; i++) {
			if (move_scores[i] > move_scores[selected_idx])
				selected_idx = i;
		}

		std::swap(move_scores[selected_idx], move_scores[m_idx]);
		std::swap(*(move_list.begin() + selected_idx), *(move_list.begin() + m_idx));

		auto & move = *(move_list.begin() + m_idx);
		m_idx++;

		// qsearch pruning: a capture with a negative SEE cannot improve
		// the score (never prune evasions)
		if (move_scores[m_idx - 1] < 0 && !sp.pos.in_check())
			continue;

		if (sp.pos.is_legal_generated_move(move, pinned) == false)
			continue;

		n_played++;

		auto undo_actions = make_move(sp.nnue_eval, sp.pos, move);
		int score = -qs(-beta, -alpha, qsdepth + 1, sp);
		unmake_move(sp.nnue_eval, sp.pos, undo_actions);

		if (score > best_score) {
			best_score = score;
			m          = move;

			if (score > alpha) {
				if (score >= beta) {
					sp.cs.data.n_qmoves_cutoff += n_played;
					sp.cs.data.nmc_qnodes++;
					break;
				}

				alpha = score;
			}
		}

		if (n_played >= 3 && best_score >= max_non_mate)
			break;
	}

	if (n_played == 0) {
		if (in_check) {
			sp.cs.data.n_checkmate++;
			best_score = -max_eval + qsdepth;
			sp.cs.win[!sp.pos.side_to_move()]++;
		}
		else if (best_score == -32767) {
			best_score = nnue_evaluate(sp.nnue_eval, sp.pos);
		}
	}

	assert(best_score >= -max_eval);
	assert(best_score <=  max_eval);

	if (sp.stop->flag == false) {
		sp.cs.data.qtt_store++;

		tt_entry_flag flag = EXACT;
		if (best_score <= start_alpha)
			flag = UPPERBOUND;
		else if (best_score >= beta)
			flag = LOWERBOUND;

		int work_score = eval_to_tt(best_score, qsdepth);

		if (best_score > start_alpha && m.has_value())
			tti.store(hash, flag, 0, work_score, m.value());
		else
			tti.store(hash, flag, 0, work_score);
	}

	return best_score;
}

void IRAM_ATTR update_history(const search_pars_t & sp, const int index, const int bonus)
{
	constexpr const int max_history = 1023;
	constexpr const int min_history = -max_history;
	int  clamped_bonus = std::clamp(bonus, min_history, max_history);
	int  final_value   = clamped_bonus - sp.history[index] * abs(clamped_bonus) / max_history;

	assert(sp.history[index] + final_value <=  32767);
	assert(sp.history[index] + final_value >= -32768);

	sp.history[index] += final_value;
}

void IRAM_ATTR update_capture_history(const search_pars_t & sp, const int index, const int bonus)
{
	constexpr const int max_hist = 1023;
	constexpr const int min_hist = -max_hist;
	int clamped = std::clamp(bonus, min_hist, max_hist);
	int delta = clamped - sp.capture_history[index] * abs(clamped) / max_hist;
	assert(sp.capture_history[index] + delta <= 32767);
	assert(sp.capture_history[index] + delta >= -32768);
	sp.capture_history[index] += delta;
}

void IRAM_ATTR update_butterfly_history(const search_pars_t & sp, const int index, const int bonus)
{
	constexpr const int max_hist = 1023;
	constexpr const int min_hist = -max_hist;
	int clamped = std::clamp(bonus, min_hist, max_hist);
	int delta = clamped - sp.butterfly_history[index] * abs(clamped) / max_hist;
	assert(sp.butterfly_history[index] + delta <= 32767);
	assert(sp.butterfly_history[index] + delta >= -32768);
	sp.butterfly_history[index] += delta;
}

void IRAM_ATTR update_cont_history(const search_pars_t & sp, const int index, const int bonus)
{
	constexpr const int max_hist = 1023;
	constexpr const int min_hist = -max_hist;
	int clamped = std::clamp(bonus, min_hist, max_hist);
	int delta = clamped - sp.cont_history[index] * abs(clamped) / max_hist;
	assert(sp.cont_history[index] + delta <= 32767);
	assert(sp.cont_history[index] + delta >= -32768);
	sp.cont_history[index] += delta;
}

int IRAM_ATTR search(int depth, int alpha, const int beta, const int null_move_depth, const int16_t max_depth, const int level, libchess::Move *const m, search_pars_t & sp)
{
	if (sp.stop->flag)
		return 0;

	const int scr_level    = level < n_search_scratch_levels ? level : n_search_scratch_levels - 1;
	const int child_scr_lv = level + 1 < n_search_scratch_levels ? level + 1 : n_search_scratch_levels - 1;
	node_scratch_t & scr   = sp.scratch[scr_level];
	scr.pv_len             = 0;

#if defined(ESP32)
	// Keep the idle tasks alive so they can feed the task watchdog. The
	// engine runs an unbounded auto-ponder search after each bestmove and
	// both searcher threads are same-priority, so one of them is always
	// ready and IDLE never gets a slot: vTaskDelay(1) merely passes the
	// CPU to the peer searcher. Instead, the two searchers block in
	// vTaskDelay(1) at the same time via a shared gate, letting IDLE run.
	// Triggered by time (not the node counter: that can be folded away by
	// the compiler, making the gate unreachable). The pre-check is a plain
	// load in the common case (gate closed, window not due). The first
	// searcher to pass it stamps the window and waits for the peer; the
	// peer's pre-check sees the open gate (==1) and follows it in, so both
	// searchers delay together and BOTH idle tasks get CPU time to feed
	// the task watchdog (one yielding searcher would leave the other
	// core's IDLE starved and trip a PANIC-enabled WDT on long searches).
	const uint32_t gate_val = __atomic_load_n(&es32_yield_gate, __ATOMIC_SEQ_CST);
	if (gate_val == 1 || esp_timer_get_time() - es32_last_yield >= 1500000) {
		if (__atomic_add_fetch(&es32_yield_gate, 1, __ATOMIC_SEQ_CST) == 1) {
			// First searcher to arrive: stamp the window, publish its
			// handle, then block until the peer follows (guaranteed:
			// its pre-check sees the open gate), then both delay
			// together.
			es32_last_yield = esp_timer_get_time();
			es32_yield_waiter = xTaskGetCurrentTaskHandle();
			ulTaskNotifyTake(pdTRUE, 2);
			vTaskDelay(1);
		}
		else {
			// Peer: close the gate and wake the first searcher.
			__atomic_store_n(&es32_yield_gate, 0, __ATOMIC_SEQ_CST);
			TaskHandle_t waiter = es32_yield_waiter;
			es32_yield_waiter = NULL;
			if (waiter != NULL)
				xTaskNotifyGive(waiter);
			else {
				TaskHandle_t peer = es32_yield_peer;
				if (peer != NULL)
					xTaskNotifyGive(peer);
			}
			vTaskDelay(1);
		}
	}
#endif

	if (depth == 0) {
		int score = qs(alpha, beta, max_depth, sp);
		return score;
	}

	sp.cs.data.nodes++;

	const int  csd              = max_depth -  depth + 1;
	bool       is_root_position = max_depth == depth;

	if (!is_root_position && (sp.pos.is_repeat() || sp.pos.halfmoves() > 100 || is_insufficient_material_draw(sp.pos))) {
		if (sp.pos.in_check()) {
			if (sp.pos.legal_move_list().empty()) {
				sp.cs.win[!sp.pos.side_to_move()]++;
				sp.cs.data.n_checkmate++;
				return -max_eval + csd;
			}
		}
		sp.cs.draw++;
		sp.cs.data.n_draws++;
		return 0;
	}

	const int  start_alpha = alpha;
	const bool is_pv       = alpha != beta -1;

	// TT //
	std::optional<libchess::Move> tt_move { };
	std::optional<libchess::Move> exp_move { };
	std::optional<exp_entry> exp_e { };
	uint64_t       hash        = sp.pos.hash();
#if EXP_TABLE_ENABLED
	if (exp_table::g_enabled && exp_table::g_entries) {
		auto ee = exp_table::lookup(hash);
		if (ee.has_value()) {
			exp_e = ee;
			if (ee->move) {
				auto em = uint_to_libchessmove(ee->move);
				if (sp.pos.is_legal_move(em)) exp_move = em;
			}
			if (ee->depth >= depth && !is_pv) {
				int wscore = eval_from_tt(ee->score, csd);
				auto flag = tt_entry_flag(ee->flags);
				bool use = flag == EXACT ||
					(flag == LOWERBOUND && wscore >= beta) ||
					(flag == UPPERBOUND && wscore <= alpha);
				if (use) {
					sp.cs.data.tt_cutoff++;
					if (exp_move.has_value()) {
						*m = exp_move.value();
						return wscore;
					}
					if (!is_root_position) return wscore;
				}
			}
		}
	}
#endif
	std::optional<tt_entry> te = tti.lookup(hash);
	sp.cs.data.tt_query++;

        if (te.has_value()) {  // TT hit?
		sp.cs.data.tt_hit++;
		if (te.value().M) {  // move stored in TT?
			tt_move = uint_to_libchessmove(te.value().M);
			if (sp.pos.is_legal_move(tt_move.value()) == false) {
				sp.cs.data.tt_invalid++; // move stored in TT is not valid - TT-collision
				tt_move.reset();
			}
		}

		if (te.value().depth >= depth && !is_pv) {
			int score      = te.value().score;
			int work_score = eval_from_tt(score, csd);
			auto flag      = te.value().flags;
                        bool use       = flag == EXACT ||
                                        (flag == LOWERBOUND && work_score >= beta) ||
                                        (flag == UPPERBOUND && work_score <= alpha);

			if (use) {
				sp.cs.data.tt_cutoff++;
				if (tt_move.has_value()) {
					*m = tt_move.value();  // move in TT is valid
					return work_score;
				}
				if (!is_root_position) {
					return work_score;
				}
			}
		}
	}
	else if (is_pv && depth >= 4) {  // IIR, Internal Iterative Reductions
		depth--;
	}
	////////

	// Mate distance pruning (Stockfish)
	// Since the path to a checkmate is unique, a mate score can only be
	// improved at the node below the one that finds it. So a mate score
	// is a hard bound: we can never do better than mate in (ply+1) from
	// here, and never worse than being mated at the current ply. With
	// these bounds we can fail high/low early, which is especially
	// useful for mate searching.
	// https://github.com/official-stockfish/Stockfish/blob/master/src/search.cpp
	if ((is_pv && te.has_value()) || (!is_pv && tt_move.has_value())) {
		int mdp_alpha = std::max(-max_eval + csd, alpha);
		int mdp_beta  = std::min(max_eval - csd - 1, beta);
		if (mdp_alpha >= mdp_beta)
			return mdp_alpha;
	}

#if defined(linux) || defined(_WIN32) || defined(__ANDROID__) || defined(__APPLE__)
	if (with_syzygy && !is_root_position) {
		// check piece count
		unsigned counts = sp.pos.occupancy_bb().popcount();

		// syzygy count?
		if (counts <= TB_LARGEST) {
			sp.cs.data.syzygy_queries++;
			std::optional<int> syzygy_score = probe_fathom_nonroot(sp.pos);

			if (syzygy_score.has_value()) {
				sp.cs.data.syzygy_query_hits++;
				int score = syzygy_score.value();
				if (score < 0)
					score = -max_non_mate - 1;
				else if (score > 0)
					score =  max_non_mate + 1;
				return score;
			}
		}
	}
#endif

	////////
	bool in_check = sp.pos.in_check();

	int  staticeval   = 0;
	int  staticeval_raw = 0;
	bool staticeval_ok = false;
	bool futility_ok  = false;

	if (!is_root_position && !in_check && depth <= 7 && beta <= max_non_mate) {
		sp.cs.data.n_static_eval++;
		staticeval_raw  = nnue_evaluate(sp.nnue_eval, sp.pos);
		staticeval = staticeval_raw;
#if CORR_HIST_ENABLED
		if (corr_hist::g_enabled && corr_hist::g_table) {
			int corr = corr_hist::get_correction_for_pos(sp.pos);
			staticeval = staticeval_raw + corr;
			if (staticeval > max_non_mate) staticeval = max_non_mate;
			if (staticeval < -max_non_mate) staticeval = -max_non_mate;
		}
#endif
		staticeval_ok = true;
		futility_ok = depth <= 2;

		// static null pruning (reverse futility pruning)
		if (staticeval - depth * 121 > beta) {
			sp.cs.data.n_static_eval_hit++;
			return (beta + staticeval) / 2;
		}

		// razoring: at shallow depth a position whose static eval is far
		// below alpha is hopeless - skip move generation, qsearch only
		// (captures can still recover the material deficit)
		if (!is_pv && !tt_move.has_value() && depth <= 3 && beta <= max_non_mate
			&& staticeval + 350 + depth * 150 < alpha) {
			sp.cs.data.n_razor++;
			return qs(alpha, beta, max_depth, sp);
		}
	}

	///// null move
	int nm_reduce_depth = depth > 6 ? 4 : 3;
	if (depth >= 2 && !in_check && !is_root_position && null_move_depth < 2) {
		sp.cs.data.n_null_move++;

		sp.pos.make_null_move();
		libchess::Move     ignore_move { };
		int nmscore = -search(std::max(0, depth - nm_reduce_depth), -beta, -beta + 1, null_move_depth + 1, max_depth, level + 1, &ignore_move, sp);
		sp.pos.unmake_move();

                if (nmscore >= beta) {
			libchess::Move     ignore2 { };
			int verification = search(std::max(0, depth - nm_reduce_depth), beta - 1, beta, null_move_depth, max_depth, level + 1, &ignore2, sp);
			if (verification >= beta) {
				sp.cs.data.n_null_move_hit++;
				return abs(nmscore) >= max_non_mate ? beta : nmscore;
			}
                }
	}
	///////////////

	int                    best_score = -32767;
	sp.pos.pseudo_legal_move_list_into(scr.ml);
	libchess::MoveList &  move_list  = scr.ml;

	sort_movelist_compare smc(sp);

	if (tt_move.has_value())
		smc.add_first_move(tt_move.value());
#if EXP_TABLE_ENABLED
	if (exp_move.has_value() && (!tt_move.has_value() || exp_move.value() != tt_move.value()))
		smc.add_first_move(exp_move.value());
#endif
	if (m->value() && sp.pos.is_capture_move(*m))
		smc.add_first_move(*m);

	int     n_played   = 0;
	int     lmr_start  = !in_check && depth >= 2 ? 4 : 999;

	// generate list of scores
	size_t           n_moves = move_list.size();
	libchess::Bitboard pinned = sp.pos.pinned_pieces_of(sp.pos.side_to_move());
	scr.scores.clear();
	scr.scores.resize(n_moves);
	std::vector<int> & move_scores = scr.scores;
	for(size_t i=0; i<n_moves; i++)
		move_scores[i] = smc.move_evaluater(*(move_list.begin() + i));

	std::optional<libchess::Move> beta_cutoff_move;
	libchess::Move new_move;

	int new_depth_basic = depth - 1 + (sp.pos.in_check() || n_moves == 1 ? 1 : 0);

	// Recapture extension: if the opponent's last move was a capture on
	// square S, our move capturing back on S gets +1 ply (the classic
	// companion to the check extension - the position's material balance
	// is in flux on the recapture square and cheap errors there ripple).
	std::optional<libchess::Square> recapture_square { };
	if (auto prev = sp.pos.previous_move(); prev.has_value() && sp.pos.is_capture_move(prev.value()))
		recapture_square = prev.value().to_square();

	size_t             m_idx    = 0;
	while(m_idx < n_moves) {
		size_t selected_idx = m_idx;
		for(size_t i=m_idx; i<n_moves; i++) {
			if (move_scores[i] > move_scores[selected_idx])
				selected_idx = i;
		}

		std::swap(move_scores[selected_idx], move_scores[m_idx]);
		std::swap(*(move_list.begin() + selected_idx), *(move_list.begin() + m_idx));

		auto & move = *(move_list.begin() + m_idx);
		m_idx++;

		if (sp.pos.is_legal_generated_move(move, pinned) == false)
			continue;

		// per-move recapture extension: +1 ply when we capture back on the
		// square the opponent's last move captured on
		int recapture_ext = 0;
		if (recapture_square.has_value() && move.to_square() == recapture_square.value() && sp.pos.is_capture_move(move))
			recapture_ext = 1;

		// futility pruning: a quiet move cannot improve the score if the
		// static eval plus a depth-scaled margin still falls below alpha
		// (only at shallow depths, outside PV, and never for the TT move
		// or the best-ranked first move of the node)
		if (futility_ok && !is_pv && n_played > 0 && n_moves > 1
			&& !sp.pos.is_capture_move(move) && !sp.pos.is_promotion_move(move)
			&& !(tt_move.has_value() && move == tt_move.value())
			&& staticeval + 180 + depth * 150 < alpha) {
			sp.cs.data.n_futility_prune++;
			continue;
		}

		sp.cur_move = move.value();

                bool is_lmr = false;
                int  score  = -max_eval;

		auto undo_actions = make_move(sp.nnue_eval, sp.pos, move);
		if (n_played == 0)
			score = -search(new_depth_basic + recapture_ext, -beta, -alpha, null_move_depth, max_depth, level + 1, &new_move, sp);
		else {
			int new_depth = new_depth_basic + recapture_ext;

			if (n_played >= lmr_start && !sp.pos.is_capture_move(move) && !sp.pos.is_promotion_move(move)) {
				is_lmr = true;
				sp.cs.data.n_lmr++;

				if (alpha == beta -1) {
					int reduction = lmr_reductions[std::min(N_LMR_DEPTH - 1, int(depth))][std::min(N_LMR_MOVES - 1, n_played)];
					new_depth = std::max(new_depth_basic - reduction, 0);
				}
				else if (n_played >= lmr_start + 2)
					new_depth = new_depth_basic * 2 / 3;
				else {
					new_depth = new_depth_basic - 1;
				}
			}

			score = -search(new_depth, -alpha - 1, -alpha, null_move_depth, max_depth, level + 1, &new_move, sp);

			if (is_lmr && score > alpha)
				score = -search(depth -1, -alpha - 1, -alpha, null_move_depth, max_depth, level + 1, &new_move, sp);

			if (score > alpha && score < beta)
				score = -search(depth - 1, -beta, -alpha, null_move_depth, max_depth, level + 1, &new_move, sp);
		}
		unmake_move(sp.nnue_eval, sp.pos, undo_actions);

		n_played++;

		if (score > best_score) {
			best_score         = score;
			*m                 = move;

			node_scratch_t & child_scr = sp.scratch[child_scr_lv];
			scr.pv_len = 0;
			if (scr.pv_len < 64)
				scr.pv[scr.pv_len++] = move;
			for(size_t i=0; i < child_scr.pv_len && scr.pv_len < 64; i++)
				scr.pv[scr.pv_len++] = child_scr.pv[i];

			if (score > alpha) {
				if (score >= beta) {
					beta_cutoff_move = move;
					sp.cs.data.n_lmr_hit += is_lmr;
					break;
				}

				alpha = score;
			}
		}
	}

	// History updates: quiet histories (history, butterfly, cont) and capture history.
	// Uses same gravity as the original history (depth*30-25) for all tables to keep
	// the learning rate uniform; tables are orthogonal so they do not dilute each other
	// like the failed hist3d single-table expansion did.
	if (beta_cutoff_move.has_value()) {
		int bonus = depth * 30 - 25;
		bool is_cap = sp.pos.is_capture_move(beta_cutoff_move.value());
		if (is_cap) {
			// capture history: reward the cutting capture, penalize earlier captures
			for(auto move : move_list) {
				if (!sp.pos.is_capture_move(move))
					continue;
				auto pf = sp.pos.piece_on(move.from_square());
				if (!pf) continue;
				auto cap = sp.pos.piece_on(move.to_square());
				libchess::PieceType capType = libchess::constants::PAWN;
				if (move.type() == libchess::Move::Type::ENPASSANT) capType = libchess::constants::PAWN;
				else if (cap) capType = cap->type();
				int idx = capture_history_index(sp.pos.side_to_move(), pf->type(), move.to_square(), capType);
				if (idx <0 || idx >= (int)capture_history_size) continue;
				if (move == beta_cutoff_move.value()) {
					update_capture_history(sp, idx, bonus);
					break;
				}
				update_capture_history(sp, idx, -bonus);
			}
		} else {
			for(auto move : move_list) {
				if (sp.pos.is_capture_move(move))
					continue;
				auto piece_type_from = sp.pos.piece_type_on(move.from_square());
				if (!piece_type_from) continue;
				int idx = history_index(sp.pos.side_to_move(), piece_type_from.value(), move.to_square());
				int bfIdx = butterfly_history_index(sp.pos.side_to_move(), move.from_square(), move.to_square());
				int cIdx = -1;
				auto prev = sp.pos.previous_move();
				if (prev.has_value() && sp.cont_history)
					cIdx = cont_history_index(prev.value().to_square(), move.to_square());
				if (move == beta_cutoff_move.value()) {
					update_history(sp, idx, bonus);
					if (bfIdx>=0 && bfIdx < (int)butterfly_history_size) update_butterfly_history(sp, bfIdx, bonus);
					if (cIdx>=0 && cIdx < (int)cont_history_size) update_cont_history(sp, cIdx, bonus);
					break;
				}
				update_history(sp, idx, -bonus);
				if (bfIdx>=0 && bfIdx < (int)butterfly_history_size) update_butterfly_history(sp, bfIdx, -bonus);
				if (cIdx>=0 && cIdx < (int)cont_history_size) update_cont_history(sp, cIdx, -bonus);
			}
		}
		sp.cs.data.n_moves_cutoff += n_played;
		sp.cs.data.nmc_nodes++;
	}

#if CORR_HIST_ENABLED
	// persistent correction history update: bounded quiet results teach the pawn-structure tables
	if (!g_bench_active && !sp.stop->flag && staticeval_ok && n_played > 0 && depth >= 2 && abs(best_score) < max_non_mate) {
		const bool quiet_cutoff = beta_cutoff_move.has_value() &&
			!sp.pos.is_promotion_move(beta_cutoff_move.value());
		const bool failed_low = best_score <= start_alpha;
		int error = 0;
		bool informative = false;
		if (quiet_cutoff) {
			error = best_score - staticeval_raw;
			informative = error > 0;
		} else if (failed_low) {
			error = best_score - staticeval_raw;
			informative = error < 0;
		}
		// also require not in check and quiet-like (no capture promotion that changes pawn hash for quiet_cutoff path)
		// pawn captures change pawn hash, but current position's pawn hash is independent of cutoff move, so we keep capture cutoffs for pawn correction
		// we already excluded promotions above; additionally skip if cutoff is a pawn capture that would be noisy? keep inclusive for now.
		if (informative && !in_check) {
			// gravity update via shared PSRAM table, racy is fine
			uint64_t ph = corr_hist::pawn_hash(sp.pos);
			int side = sp.pos.side_to_move() == libchess::constants::WHITE ? 0 : 1;
			corr_hist::update_with_hash(ph, side, error, depth);
		}
	}
#endif

	if (n_played == 0) {
		if (in_check) {
			sp.cs.data.n_checkmate++;
			sp.cs.win[!sp.pos.side_to_move()]++;
			best_score = -max_eval + csd;
		}
		else {
			sp.cs.draw++;
			sp.cs.data.n_stalemate++;
			best_score = 0;
		}
	}

	if (sp.stop->flag == false) {
		sp.cs.data.tt_store++;

		tt_entry_flag flag = EXACT;
		if (best_score <= start_alpha)
			flag = UPPERBOUND;
		else if (best_score >= beta)
			flag = LOWERBOUND;

		int work_score = eval_to_tt(best_score, csd);

		if (best_score > start_alpha && m->value())
			tti.store(hash, flag, depth, work_score, *m);
		else
			tti.store(hash, flag, depth, work_score);

#if EXP_TABLE_ENABLED
		if (!g_bench_active && exp_table::g_enabled && exp_table::g_entries && !in_check && m->value() && depth >= 4) {
			exp_table::store(hash, depth, work_score, *m, flag);
		}
#endif
	}

	return best_score;
}

void timer(const int think_time, end_t *const ei)
{
	if (think_time > 0) {
		auto end_time = std::chrono::high_resolution_clock::now() += std::chrono::milliseconds{think_time};

		std::unique_lock<std::mutex> lk(ei->cv_lock);
		while(!ei->flag) {
			if (ei->cv.wait_until(lk, end_time) == std::cv_status::timeout)
				break;
		}
	}

	set_flag(ei);

	my_trace("# time is up; set stop flag\n");
}

double calculate_EBF(const std::vector<uint64_t> & node_counts)
{
        size_t n = node_counts.size();
        return n >= 3 ? sqrt(double(node_counts.at(n - 1)) / double(node_counts.at(n - 3))) : -1;
}

std::string gen_pv_str(const libchess::MoveList & pv)
{
	std::string pv_str;
	for(int i=0; i<pv.size(); i++) {
		if (i)
			pv_str += " ";
		pv_str += (pv.cbegin() + i)->to_str();
	}
	return pv_str;
}

std::string emit_result(const int best_score, const uint64_t thought_ms, const std::vector<uint64_t> & node_counts, const int max_depth, const std::pair<uint64_t, uint64_t> & nodes, const libchess::MoveList & pv, const bool is_tui, const std::optional<uint32_t> & time_left)
{
	std::string pv_str     = gen_pv_str(pv);
	double      ebf        = calculate_EBF(node_counts);
	std::string ebf_str    = ebf >= 0 ? std::to_string(ebf) : "";
	if (ebf_str.empty() == false)
		ebf_str = "ebf " + ebf_str + " ";

	uint64_t use_thought_ms = std::max(uint64_t(1), thought_ms);  // prevent div. by 0
	std::string score_str;
	std::string score_str_human;
	if (abs(best_score) > max_non_mate) {
		int mate_moves = (max_eval - abs(best_score) + 1) / 2 * (best_score < 0 ? -1 : 1);
		auto mate_str = std::to_string(mate_moves);
		score_str = "score mate " + mate_str;
		score_str_human = "mate in " + mate_str;
	}
	else {
		score_str = "score cp " + std::to_string(best_score);
		score_str_human = myformat("score: %d", best_score);
	}

	uint64_t nps = uint64_t(nodes.first * 1000 / use_thought_ms);

	if (is_tui) {
		extern bool verbose;
		std::string time_left_str = time_left.has_value() ? myformat(", time left: %02d:%02d.%03d", time_left.value() / (60 * 1000), (time_left.value() / 1000) % 60, time_left.value() % 1000) : "";
		std::string msg1;
		if (verbose)
			msg1 = myformat("depth: %d, duration: %.3f, NPS: %" PRIu64, max_depth, thought_ms / 1000., nps) + ", " + score_str_human + time_left_str + "\r\n";
		else
			msg1 = myformat("depth: %d (%.3fs), ", max_depth, thought_ms / 1000.) + score_str_human + time_left_str + "\r\n";
		std::string msg2 = "pv: " + pv_str + "\r\n";
		return msg1 + msg2;
	}

	return myformat("info depth %d %s nodes %" PRIu64 " %stime %" PRIu64 " nps %" PRIu64 " tbhits %" PRIu64 " hashfull %d pv %s\n",
			max_depth, score_str.c_str(),
			nodes.first, ebf_str.c_str(), thought_ms, nps,
			nodes.second, tti.get_per_mille_filled(), pv_str.c_str());
}

void emit(const std::string & text, const bool is_tui)
{
#if defined(ESP32)
	if (is_tui)
		to_uart(text.c_str(), text.size());
	else {
		std::lock_guard<std::recursive_mutex> lock(libchess::uci_console_mutex);
		printf("%s", text.c_str());
	}
#else
	printf("%s", text.c_str());
#endif
}

std::tuple<libchess::Move, int, int> IRAM_ATTR search_it(const int search_time_min, const int search_time_max, const bool is_absolute_time, search_pars_t *const sp, const int ultimate_max_depth, std::optional<uint64_t> max_n_nodes, const output_type_t output, const bool is_tui)
{
	uint64_t t_offset = esp_timer_get_time();

#if defined(linux) || defined(_WIN32) || defined(__ANDROID__) || defined(__APPLE__)
	std::thread *think_timeout_timer { nullptr };
#endif

	if (sp->thread_nr == 0) {
		if (search_time_max > 0) {
#if defined(linux) || defined(_WIN32) || defined(__ANDROID__) || defined(__APPLE__)
			think_timeout_timer = new std::thread([search_time_min, search_time_max, sp] {
					set_thread_name("searchtotimer");
					timer(search_time_max, sp->stop);
				});
#else
			esp_timer_start_once(think_timeout_timer, search_time_max * 1000ll);
#endif
		}
	}

	int best_score = 0;
	int max_depth  = 1;
	auto move_list = sp->pos.legal_move_list();
	libchess::Move best_move { *move_list.begin() };

	std::string should_output;

	if (move_list.size() > 1) {
		int alpha     = -32767;
		int beta      =  32767;

		int add_alpha = 75;
		int add_beta  = 75;

		libchess::Move cur_move;

		int alpha_repeat = 0;
		int beta_repeat  = 0;

		std::vector<uint64_t> node_counts;
		uint64_t              previous_node_count = 0;

		std::set<std::string> itd_moves;  // iterative deepening moves

		while(ultimate_max_depth == -1 || max_depth <= ultimate_max_depth) {
			sp->md = 0;
			tti.new_search();
			if (max_depth >= 4)
				cur_move = sp->best_moves[max_depth - 3];
			int                score = search(max_depth, alpha, beta, 0, max_depth, 0, &cur_move, *sp);
			libchess::MoveList pv;
			node_scratch_t &   root_scr = sp->scratch[0];
			for(size_t i=0; i < root_scr.pv_len && i < 64; i++)
				pv.add(root_scr.pv[i]);
			assert(score >= -max_eval && score <= max_eval);

			auto counts = simple_search_statistics();
			if (sp->stop->flag) {
				if (sp->thread_nr == 0 && output >= O_MINIMAL) {
					my_trace("info string stop flag set\n");
					uint64_t thought_ms = (esp_timer_get_time() - t_offset) / 1000;
					libchess::MoveList l_pv;
					l_pv.add(best_move);
					auto temp = emit_result(best_score, thought_ms, node_counts, max_depth, counts, l_pv, is_tui, search_time_max - thought_ms);
					if (output == O_FULL)
						emit(temp, is_tui);
					else
						should_output = temp;
				}
				break;
			}

			uint64_t cur_n_nodes = counts.first;
			node_counts.push_back(cur_n_nodes - previous_node_count);
			previous_node_count  = cur_n_nodes;

			if (score <= alpha) {
				sp->cs.data.asp_win_resizes++;
				my_trace("# alpha %d <= %d, resizes: %d, md: %d\n", score, alpha, sp->cs.data.asp_win_resizes, max_depth);
				if (alpha_repeat >= 3)
					alpha = -max_eval;
				else {
					beta = (alpha + beta) / 2;
					alpha = score - add_alpha;
					if (alpha < -max_eval)
						alpha = -max_eval;
					add_alpha += add_alpha / 15 + 1;

					alpha_repeat++;
				}
			}
			else if (score >= beta) {
				sp->cs.data.asp_win_resizes++;
				my_trace("# beta %d >= %d, resizes: %d, md: %d\n", score, alpha, sp->cs.data.asp_win_resizes, max_depth);
				if (beta_repeat >= 3)
					beta = max_eval;
				else {
					alpha = (alpha + beta) / 2;
					beta = score + add_beta;
					if (beta > max_eval)
						beta = max_eval;
					add_beta += add_beta / 15 + 1;

					beta_repeat++;
				}
			}
			else {
				if (alpha != -32767) {
					sp->cs.data.alpha_distance += abs(score - alpha);
					sp->cs.data.n_alpha_distances++;
				}
				if (beta != 32767) {
					sp->cs.data.beta_distance  += abs(beta - score);
					sp->cs.data.n_beta_distances++;
				}

				alpha_repeat = 0;
				beta_repeat  = 0;

				add_alpha = 75;
				add_beta  = 75;

				alpha = std::max(-max_eval, score - add_alpha);
				beta  = std::min( max_eval, score + add_beta );

				best_move  = cur_move;
				best_score = score;

				uint64_t thought_ms = (esp_timer_get_time() - t_offset) / 1000;

				if (sp->thread_nr == 0 && output >= O_MINIMAL) {
					auto temp = emit_result(best_score, thought_ms, node_counts, max_depth, counts, pv, is_tui, search_time_max - thought_ms);
					if (output == O_FULL)
						emit(temp, is_tui);
					else
						should_output = temp;
				}

				itd_moves.insert(best_move.to_str());

				if (sp->thread_nr == 0) {
					int search_time = itd_moves.size() / double(max_depth) * (search_time_max - search_time_min) + search_time_min;

					// ESP32: stop only once the full adaptive budget is spent.
					// The desktop's `search_time / 2` threshold halves an
					// already-collapsed budget (itd_moves.size()=1 with a
					// stable root move -> search_time ~= min = max/3), so the
					// board spent ~200-400 ms of its ~1 s budget at 30+0.1
					// (measured via Trace: "My time: 30000 ... moves_to_go: 39"
					// with bestmove at depth 5 in 0.34 s). Desktop keeps the
					// /2: full spend at 2+0.02 drains the clock and forfeits
					// (gated: TimeFix 24-68-87, 0.379, forfeits at move 40-49;
					// same finding as the floor-30 desktop regression).
#if defined(ESP32)
					if (search_time > 0 && int(thought_ms) >= search_time) {
#else
					if ((int(thought_ms) > search_time / 2 && search_time > 0 && is_absolute_time == false) ||
					    (int(thought_ms) >= search_time && is_absolute_time == true)) {
#endif
						my_trace("info string %d time %u is up %" PRIu64 " (%.2f %% | %.2f %%)\n", sp->pos.fullmoves(), search_time, thought_ms, thought_ms * 100. / search_time, thought_ms * 100. / search_time_max);
						break;
					}
				}

				if (max_depth == 127)
					break;

				sp->best_moves[max_depth] = best_move;

				max_depth++;
			}

			if (max_n_nodes.has_value() && cur_n_nodes >= max_n_nodes.value()) {
				my_trace("info string node limit reached with %zu nodes\n", size_t(cur_n_nodes));
				break;
			}
		}
	}
	else {
		my_trace("info string only 1 move possible (%s for %s)\n", best_move.to_str().c_str(), sp->pos.fen().c_str());
		libchess::MoveList pv;
		pv.add(best_move);
		best_score = nnue_evaluate(sp->nnue_eval, sp->pos);

		auto temp = emit_result(best_score, 0, { }, 0, { 0, 0 }, pv, is_tui, search_time_max);
		if (output == O_FULL)
			emit(temp, is_tui);
		else
			should_output = temp;
	}

	if (sp->thread_nr == 0) {
#if defined(linux) || defined(_WIN32) || defined(__ANDROID__) || defined(__APPLE__)
		set_flag(sp->stop);

		if (think_timeout_timer) {
			think_timeout_timer->join();
			delete think_timeout_timer;
		}
#else
		esp_timer_stop(think_timeout_timer);

		my_trace("# heap free: %u, max block size: %u\n", esp_get_free_heap_size(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

		vTaskGetRunTimeStats();
#endif
	}

	if (output == O_MINIMAL && should_output.empty() == false)
		emit(should_output, is_tui);

	return { best_move, best_score, max_depth };
}
