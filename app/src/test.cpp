#include <cinttypes>
#include <thread>

#include <libchess/Position.h>

#include "eval.h"
#include "main.h"
#include "nnue.h"
#include "san.h"
#include "search.h"
#include "str.h"


#define my_assert(x) \
	if (!(x)) { \
		fprintf(stderr, "assert fail at line %d (%s) in %s\n", __LINE__, __func__, __FILE__); \
		delete_threads(); \
		exit(1); \
	}

int get_nnue_score(libchess::Position &pos)
{
	Eval e(pos);
	return nnue_evaluate(&e, pos);
}

uint64_t do_nnue_verify_perft(Eval *const nnue_eval, libchess::Position &pos, int depth, const int max_depth)
{
        libchess::MoveList move_list = pos.legal_move_list();
        if (depth == 1)
                return move_list.size();

        uint64_t count = 0;
        for(const libchess::Move & move: move_list) {
		auto undo_actions = make_move(nnue_eval, pos, move);
                count += do_nnue_verify_perft(nnue_eval, pos, depth - 1, max_depth);
		unmake_move(nnue_eval, pos, undo_actions);

		{
			int a = 0, b = 0;
			if ((a = get_nnue_score(pos)) != (b = nnue_evaluate(nnue_eval, pos))) {
				printf("fail @ %d: %s %s (%d != %d)\n", depth, pos.fen().c_str(), move.to_str().c_str(), a, b);
				for(int i=0; i<undo_actions.first; i++) {
					auto & action = undo_actions.second[i];
					printf("%s: %s %c %s\n", action.is_put ? "ADD":"REM", action.location.to_str().c_str(), action.type.to_char(), action.is_white ? "white":"black");
				}
				my_assert(false);
			}
		}
        }

        return count;
}

void nnue_verify_perft(Eval *const nnue_eval, libchess::Position &pos, const std::vector<unsigned> & depths)
{
	nnue_eval->set(pos);
	for(size_t i=0; i<depths.size(); i++) {
		uint64_t result = do_nnue_verify_perft(nnue_eval, pos, i + 1, i + 1);
		if (result != depths.at(i)) {
			printf("Count mismatch, got %" PRIu64 ", expected %u\n", result, depths.at(i));
			my_assert(false);
		}
	}
}

// ---------------------------------------------------------------------------
// Deterministic PRNG (xorshift64*) so the fuzz tests are reproducible.
static uint64_t fuzz_rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t fuzz_rand()
{
	fuzz_rng_state ^= fuzz_rng_state >> 12;
	fuzz_rng_state ^= fuzz_rng_state << 25;
	fuzz_rng_state ^= fuzz_rng_state >> 27;
	return fuzz_rng_state * 0x2545F4914F6CDD1Dull;
}

static uint64_t fuzz_rand_below(const uint64_t n)
{
	return n ? fuzz_rand() % n : 0;
}

// Generate an arbitrary (not necessarily legal) move that round-trips through
// the tt move encoding: from/to on the board, a valid 3-bit Move::Type and an
// optional promotion piece (KNIGHT..QUEEN) so the PROMOTION_TYPE bits and the
// 18-bit tt M field are exercised.
static libchess::Move fuzz_move()
{
	using namespace libchess;
	const Square from = Square::from(File(fuzz_rand_below(8)), Rank(fuzz_rand_below(8))).value();
	const Square to   = Square::from(File(fuzz_rand_below(8)), Rank(fuzz_rand_below(8))).value();
	const Move::Type type = Move::Type(fuzz_rand_below(8));  // NONE..CAPTURE_PROMOTION

	if (fuzz_rand() & 1) {
		static const PieceType promos[] = { constants::KNIGHT, constants::BISHOP, constants::ROOK, constants::QUEEN };
		return Move{ from, to, promos[fuzz_rand_below(4)], type };
	}
	return Move{ from, to, type };
}

// Plain perft (position-only, no NNUE book-keeping) for deep move-generation
// verification on standard + combinatorial positions.
static uint64_t count_perft(libchess::Position &pos, const int depth)
{
	auto move_list = pos.legal_move_list();
	if (depth == 1)
		return move_list.size();

	uint64_t count = 0;
	for(const auto & move: move_list) {
		pos.make_move(move);
		count += count_perft(pos, depth - 1);
		pos.unmake_move();
	}
	return count;
}

void tests()
{
	using namespace libchess;

	set_thread_name("TESTS");

	printf("Size of int must be 32 bit\n");
	my_assert(sizeof(int) == 4);
	printf("OK\n");

	allocate_threads(1);

	{
		printf("tt move conversion\n");
		libchess::Move m1 { *libchess::Move::from("e2e4") };
		uint32_t v = libchessmove_to_uint(m1);
		libchess::Move m2 = uint_to_libchessmove(v);
		my_assert(m1 == m2);
		my_assert(m1.type() == m2.type());
		printf("OK\n");
	}

	{
		printf("NNUE perft\n");

		const std::vector<std::pair<std::string, std::vector<unsigned> > > perfts {
			{ constants::STARTPOS_FEN, { 20, 400, 8902, 197281, 4865609 } },
			{ "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", { 48, 2039, 97862, 4085603 } },
			{ "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -", { 14, 191, 2812, 43238, 674624, 11030083 } },
			{ "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", { 6, 264, 9467, 422333, 15833292 } },
			{ "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", { 44, 1486, 62379, 2103487 } },
			{ "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", { 46, 2079, 89890, 3894594 } },
			{ "3k4/8/8/2PBb3/4p3/2K1N3/8/8 w - -", { 5, 90, 1950, 27716, 585553 } },
			{ "8/8/8/8/8/8/6k1/4K2R b K - 0 1", { 3, 32, 134, 2073, 10485, 179869 } },
		};

		for(auto & record: perfts) {
			printf("Testing %s\n", record.first.c_str());
			Position pos { record.first };
			Eval *e = new Eval(pos);
			nnue_verify_perft(e, pos, record.second);
			delete e;
		}

		printf("OK\n");
	}

	// NNUE incremental
	{
		printf("NNUE incremental update test\n");

		int before = nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos);
		std::string before_str = sp.at(0)->pos.fen();

		// depth 1
		{
			for(auto & move: sp.at(0)->pos.legal_move_list()) {
				auto undo_actions = make_move(sp.at(0)->nnue_eval, sp.at(0)->pos, move);
				my_assert(sp.at(0)->pos.fen() != before_str);
				unmake_move(sp.at(0)->nnue_eval, sp.at(0)->pos, undo_actions);
				my_assert(before == nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos));
			}
		}

		// depth 2
		{
			auto undo_actions1 = make_move(sp.at(0)->nnue_eval, sp.at(0)->pos, { constants::D2, constants::D4, Move::Type::DOUBLE_PUSH });
			std::string before_str2 = sp.at(0)->pos.fen();
			for(auto & move: sp.at(0)->pos.legal_move_list()) {
				auto undo_actions2 = make_move(sp.at(0)->nnue_eval, sp.at(0)->pos, move);
				my_assert(sp.at(0)->pos.fen() != before_str2);
				unmake_move(sp.at(0)->nnue_eval, sp.at(0)->pos, undo_actions2);
			}
			unmake_move(sp.at(0)->nnue_eval, sp.at(0)->pos, undo_actions1);
			my_assert(before == nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos));
		}

		// generic position & promotion
		{
			sp.at(0)->pos = Position("8/5P1k/8/4B1K1/8/1B6/2N5/8 w - - 0 1");
			init_move(sp.at(0)->nnue_eval, sp.at(0)->pos);
			int before2 = nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos);
			auto undo_actions1 = make_move(sp.at(0)->nnue_eval, sp.at(0)->pos, { constants::E5, constants::B8, Move::Type::NORMAL });
			my_assert(sp.at(0)->pos.fen() != before_str);
			auto undo_actions2 = make_move(sp.at(0)->nnue_eval, sp.at(0)->pos, { constants::F7, constants::F8, constants::ROOK, Move::Type::PROMOTION });
			my_assert(sp.at(0)->pos.fen() != before_str);
			unmake_move(sp.at(0)->nnue_eval, sp.at(0)->pos, undo_actions2);
			unmake_move(sp.at(0)->nnue_eval, sp.at(0)->pos, undo_actions1);
			my_assert(before2 == nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos));
		}

		// promotion with capture
		{
			sp.at(0)->pos = Position("4b3/5P1k/8/6K1/8/1B6/2N5/8 w - - 0 1");
			init_move(sp.at(0)->nnue_eval, sp.at(0)->pos);
			int before2 = nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos);
			auto undo_actions1 = make_move(sp.at(0)->nnue_eval, sp.at(0)->pos, { constants::F7, constants::E8, constants::ROOK, Move::Type::CAPTURE_PROMOTION });
			my_assert(sp.at(0)->pos.fen() != before_str);
			unmake_move(sp.at(0)->nnue_eval, sp.at(0)->pos, undo_actions1);
			my_assert(before2 == nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos));
		}

		// castling
		{
			sp.at(0)->pos = Position("rnbqkbnr/p1p1p1pp/1p1p1p2/8/4P3/3B3N/PPPP1PPP/RNBQK2R w KQkq - 0 4");
			init_move(sp.at(0)->nnue_eval, sp.at(0)->pos);
			int before2 = nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos);
			auto undo_actions1 = make_move(sp.at(0)->nnue_eval, sp.at(0)->pos, { constants::E1, constants::G1, Move::Type::CASTLING });
			my_assert(sp.at(0)->pos.fen() != before_str);
			unmake_move(sp.at(0)->nnue_eval, sp.at(0)->pos, undo_actions1);
			my_assert(before2 == nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos));
		}

		// en-passant
		{
			sp.at(0)->pos = Position("rnbqkbnr/p1ppp1pp/1p3p2/4P3/8/3B3N/PPPP1PPP/RNBQK2R b KQkq - 0 1");
			init_move(sp.at(0)->nnue_eval, sp.at(0)->pos);
			int before2 = nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos);
			auto undo_actions1 = make_move(sp.at(0)->nnue_eval, sp.at(0)->pos, { constants::D7, constants::D5, Move::Type::NORMAL });
			my_assert(sp.at(0)->pos.fen() != before_str);
			auto undo_actions2 = make_move(sp.at(0)->nnue_eval, sp.at(0)->pos, { constants::E5, constants::D6, Move::Type::ENPASSANT });
			my_assert(sp.at(0)->pos.fen() != before_str);
			unmake_move(sp.at(0)->nnue_eval, sp.at(0)->pos, undo_actions2);
			unmake_move(sp.at(0)->nnue_eval, sp.at(0)->pos, undo_actions1);
			my_assert(before2 == nnue_evaluate(sp.at(0)->nnue_eval, sp.at(0)->pos));
		}

		printf("OK\n");
	}

	// these are from https://github.com/kz04px/rawr/blob/master/tests/search.rs#L14
	// - mate in 1
	const std::vector<std::pair<std::string, std::string> > mate_in_1 {
            {"6k1/R7/6K1/8/8/8/8/8 w - - 0 1", "a7a8"},
            {"8/8/8/8/8/6k1/r7/6K1 b - - 0 1", "a2a1"},
            {"6k1/4R3/6K1/q7/8/8/8/8 w - - 0 1", "e7e8"},
            {"8/8/8/8/Q7/6k1/4r3/6K1 b - - 0 1", "e2e1"},
            {"6k1/8/6K1/q3R3/8/8/8/8 w - - 0 1", "e5e8"},
            {"8/8/8/8/Q3r3/6k1/8/6K1 b - - 0 1", "e4e1"},
            {"k7/6R1/5R1P/8/8/8/8/K7 w - - 0 1", "f6f8"},
            {"k7/8/8/8/8/5r1p/6r1/K7 b - - 0 1", "f3f1"},
	};

	for(auto & entry: mate_in_1) {
		printf("Testing \"%s\" for mate-in-1\n", entry.first.c_str());
		Position p { entry.first };
		p.make_move(*Move::from(entry.second));
		my_assert(p.game_state() == Position::GameState::CHECKMATE);
	}

	printf("OK\n");

	// - underpromotions
	const std::vector<std::pair<std::string, std::string> > underpromotions {
            {"8/5P1k/8/4B1K1/8/1B6/2N5/8 w - - 0 1", "f7f8n"},
            {"8/2n5/1b6/8/4b1k1/8/5p1K/8 b - - 0 1", "f2f1n"},
	};

	for(auto & entry: underpromotions) {
		printf("Testing \"%s\" for underpromotions\n", entry.first.c_str());
		sp.at(0)->pos = Position { entry.first };
		my_assert(sp.at(0)->pos.fen() == entry.first);

		clear_flag(sp.at(0)->stop);
		memset(sp.at(0)->history, 0x00, history_malloc_size);
		Move best_move  { 0 };
		int  best_score { 0 };
		int  max_depth  { 0 };
		std::tie(best_move, best_score, max_depth) = search_it(100, 100, false, sp.at(0), -1, { }, O_NONE, false);
		
		my_assert(best_move == *Move::from(entry.second));

		printf("OK\n");
	}

	// - move sorting & generation
	{
		printf("move sorting & generation test\n");
		sp.at(0)->pos = Position { "rnbqkbnr/2p1p1pp/1p3p2/p2p4/Q1P1P3/8/PP1P1PPP/RNB1KBNR b KQkq - 0 1" };

		clear_flag(sp.at(0)->stop);
		memset(sp.at(0)->history, 0x00, history_malloc_size);

		MoveList move_list = sp.at(0)->pos.pseudo_legal_move_list();
		my_assert(move_list.size() == 7);
		sort_movelist_compare smc(*sp.at(0));
		move_list.sort([&smc](const Move move) { return smc.move_evaluater(move); });

		int prev_v = 32767;
		for(auto & m: move_list) {
			int cur_v = smc.move_evaluater(m);
			my_assert(cur_v <= prev_v);
			prev_v = cur_v;
		}

		printf("OK\n");
	}

	// tt
	{
		printf("tt test\n");

		tti.reset();
		// initial state
		my_assert(tti.lookup(1).has_value() == false);
		my_assert(tti.lookup(0).has_value() == true);

		// just set a record
		{
			tti.store(2, EXACT, 3, 4, *Move::from("e2e4"));
			my_assert(tti.lookup(0).has_value() == false);
			my_assert(tti.lookup(1).has_value() == false);
			my_assert(tti.lookup(2).has_value() == true);
			my_assert(tti.lookup(3).has_value() == false);
			auto record1 = tti.lookup(2);
			my_assert(record1.has_value());
			auto data1 = record1.value();
			my_assert(Move(uint_to_libchessmove(data1.M)) == *Move::from("e2e4"));
			my_assert(data1.depth == 3);
			my_assert(data1.score == 4);
			my_assert(data1.flags == EXACT);
		}

		printf("OK\n");
	}

	// bool is_insufficient_material_draw(const Position & pos)
	{
		printf("is_insufficient_material_draw test\n");

		// start position
		{
			Position p1 { constants::STARTPOS_FEN };
			my_assert(is_insufficient_material_draw(p1) == false);
		}

		// two kings
		{
			Position p1 { "8/8/8/2k5/8/5K2/8/8 w - - 0 1" };
			my_assert(is_insufficient_material_draw(p1) == true);
		}

		// A king + any(pawn, rook, queen) is sufficient.
		{
			const std::vector<std::string> tests { "8/8/5p2/2k5/8/5K2/8/8 w - - 0 1", "8/8/5R2/2k5/8/5K2/8/8 w - - 0 1", "8/8/5Q2/2k5/8/5K2/8/8 w - - 0 1" };
			for(auto & test: tests) {
				// printf(" %s\n", test.c_str());
				Position p1 { test };
				my_assert(is_insufficient_material_draw(p1) == false);
			}
		}

		// A king and more than one other type of piece is sufficient (e.g. knight + bishop).
		{
			Position p1 { "8/8/5nb1/2k5/8/5K2/8/8 w - - 0 1" };
			my_assert(is_insufficient_material_draw(p1) == false);
		}

		// A king and two (or more) knights is sufficient.
		{
			Position p1 { "8/8/5nn1/2k5/8/5K2/8/8 w - - 0 1" };
			my_assert(is_insufficient_material_draw(p1) == false);
		}

		// King + knight against king + any(rook, bishop, knight, pawn) is sufficient.
		{
			const std::vector<std::string> tests { "8/8/5nR1/2k5/8/5K2/8/8 w - - 0 1", "8/8/5nB1/2k5/8/5K2/8/8 w - - 0 1", "8/8/5nN1/2k5/8/5K2/8/8 w - - 0 1", "8/8/5nP1/2k5/8/5K2/8/8 w - - 0 1" };
			for(auto & test: tests) {
				// printf(" %s\n", test.c_str());
				Position p1 { test };
				my_assert(is_insufficient_material_draw(p1) == false);
			}
		}

		// King + bishop against king + any(knight, pawn) is sufficient.
		{
			const std::vector<std::string> tests { "8/8/2b5/2k5/5N2/5K2/8/8 w - - 0 1", "8/8/2b5/2k5/5P2/5K2/8/8 w - - 0 1" };
			for(auto & test: tests) {
				// printf(" %s\n", test.c_str());
				Position p1 { test };
				my_assert(is_insufficient_material_draw(p1) == false);
			}
		}

		// King + bishop(s) is also sufficient if there's bishops on opposite colours (even king + bishop against king + bishop).

		// tests from https://github.com/toanth/motors/blob/main/gears/src/games/chess.rs#L1564-L1610
		// insufficient
		{
			const std::vector<std::string> tests { "8/4k3/8/8/8/8/8/2K5 w - - 0 1",
				"8/4k3/8/8/8/8/5N2/2K5 w - - 0 1",
				"8/8/8/6k1/8/2K5/5b2/6b1 w - - 0 1",  // bishops on same color
				"8/8/3B4/7k/8/8/1K6/6b1 w - - 0 1",
				"8/6B1/8/6k1/8/2K5/8/6b1 w - - 0 1",
				"3b3B/2B5/1B1B4/B7/3b4/4b2k/5b2/1K6 w - - 0 1",  // bishops on same color
				"3B3B/2B5/1B1B4/B6k/3B4/4B3/1K3B2/2B5 w - - 0 1"  // bishops on same color
			};
			for(auto & test: tests) {
				Position p1 { test };
				my_assert(is_insufficient_material_draw(p1) == true);
			}
		}
		// sufficient
		{
			const std::vector<std::string> tests { "8/8/4k3/8/8/1K6/8/7R w - - 0 1",
				"5r2/3R4/4k3/8/8/1K6/8/8 w - - 0 1",
				"8/8/4k3/8/8/1K6/8/6BB w - - 0 1",
				"8/8/4B3/8/8/7K/8/6bk w - - 0 1",
				"3B3B/2B5/1B1B4/B6k/3B4/4B3/1K3B2/1B6 w - - 0 1",
				"8/3k4/8/8/8/8/NNN5/1K6 w - - 0 1"
			};
			for(auto & test: tests) {
				Position p1 { test };
				my_assert(is_insufficient_material_draw(p1) == false);
			}
		}
		// sufficient but unreasonable
		{
			const std::vector<std::string> tests { "6B1/8/8/6k1/8/2K5/8/6b1 w - - 0 1",
				"8/8/4B3/8/8/7K/8/6bk b - - 0 1",
				"8/8/4B3/7k/8/8/1K6/6b1 w - - 0 1",
				"8/3k4/8/8/8/8/1NN5/1K6 w - - 0 1",
				"8/2nk4/8/8/8/8/1NN5/1K6 w - - 0 1",
			};
			for(auto & test: tests) {
				Position p1 { test };
				my_assert(is_insufficient_material_draw(p1) == false);
			}
		}

		printf("OK\n");
	}

	// san
	const std::vector<std::tuple<const std::string, const std::string, const std::string, int> > san_parsing_tests {
		{ "7r/3r1p1p/6p1/1p6/2B5/5PP1/1Q5P/1K1k4 b - - 0 38", "bxc4", "7r/3r1p1p/6p1/8/2p5/5PP1/1Q5P/1K1k4 w - - 0 39", -827 },
		{ "2n1r1n1/1p1k1p2/6pp/R2pP3/3P4/8/5PPP/2R3K1 b - - 0 30", "Nge7", "2n1r3/1p1knp2/6pp/R2pP3/3P4/8/5PPP/2R3K1 w - - 1 31", 361 },
		{ "8/5p2/1kn1r1n1/1p1pP3/6K1/8/4R3/5R2 b - - 9 60", "Ngxe5+", "8/5p2/1kn1r3/1p1pn3/6K1/8/4R3/5R2 w - - 0 61", 1352 },
		{ "r3k2r/pp1bnpbp/1q3np1/3p4/3N1P2/1PP1Q2P/P1B3P1/RNB1K2R b KQkq - 5 15", "Ng8", "r3k1nr/pp1bnpbp/1q4p1/3p4/3N1P2/1PP1Q2P/P1B3P1/RNB1K2R w KQkq - 6 16", 325 },
		{ libchess::constants::STARTPOS_FEN, "e4", "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", 55 },
	};

	{
		printf("SAN parsing test\n");

		for(auto & test: san_parsing_tests) {
			libchess::Position pos(std::get<0>(test));
			pos.make_move(SAN_to_move(std::get<1>(test), pos).value());
			my_assert(std::get<2>(test) == pos.fen());
		}

		printf("OK\n");
	}

	// NNUE eval (using san parsing data)
	{
		printf("NNUE evaluation test\n");

		for(auto & test: san_parsing_tests) {
			libchess::Position pos(std::get<0>(test));
			init_move(sp.at(0)->nnue_eval, pos);
			my_assert(nnue_evaluate(sp.at(0)->nnue_eval, pos) == std::get<3>(test));
		}

		printf("OK\n");
	}

	// deeper plain perft (move generation only, no NNUE book-keeping) on the
	// standard positions plus the classic combinatorial ones. Values are the
	// well-known published perft numbers (also confirmed against libchess).
	{
		printf("deep perft (plain)\n");
		const std::vector<std::tuple<std::string, int, uint64_t> > cases {
			{ constants::STARTPOS_FEN, 6, 119060324ull },
			{ "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -", 7, 178633661ull },
			{ "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", 5, 193690690ull },
			{ "8/8/8/8/8/8/6k1/4K2R b K - 0 1", 7, 954475ull },
		};

		for(auto & c: cases) {
			Position pos(std::get<0>(c));
			uint64_t got = count_perft(pos, std::get<1>(c));
			if (got != std::get<2>(c)) {
				printf("deep perft mismatch for %s depth %d: got %" PRIu64 ", expected %" PRIu64 "\n",
					std::get<0>(c).c_str(), std::get<1>(c), got, std::get<2>(c));
				my_assert(false);
			}
		}

		printf("OK\n");
	}

	// NNUE incremental update fuzz: random make/unmake walks over positions that
	// exercise castling, en-passant, promotions and captures; the incrementally
	// updated accumulator must always agree with a fresh full re-evaluation.
	{
		printf("NNUE incremental fuzz\n");

		const std::vector<std::string> fuzz_fens {
			constants::STARTPOS_FEN,
			"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -",
			"rnbqkbnr/p1p1p1pp/1p1p1p2/8/4P3/3B3N/PPPP1PPP/RNBQK2R w KQkq - 0 4",
			"8/5P1k/8/4B1K1/8/1B6/2N5/8 w - - 0 1",
		};

		for(auto & fen: fuzz_fens) {
			sp.at(0)->pos = Position(fen);
			Eval *e = sp.at(0)->nnue_eval;
			init_move(e, sp.at(0)->pos);
			int root_score = nnue_evaluate(e, sp.at(0)->pos);

			for(int walk = 0; walk < 8; walk++) {
				std::vector<std::pair<int, std::array<undo_t, 4> > > undo_stack;

				for(int ply = 0; ply < 24; ply++) {
					auto move_list = sp.at(0)->pos.legal_move_list();
					if (move_list.size() == 0)
						break;
					auto it = move_list.begin();
					std::advance(it, size_t(fuzz_rand_below(move_list.size())));
					auto & move = *it;
					undo_stack.push_back(make_move(e, sp.at(0)->pos, move));

					int inc  = nnue_evaluate(e, sp.at(0)->pos);
					Eval fresh(sp.at(0)->pos);
					int full = nnue_evaluate(&fresh, sp.at(0)->pos);
					if (inc != full) {
						printf("NNUE fuzz mismatch walk %d ply %d fen %s move %s (%d != %d)\n",
							walk, ply, sp.at(0)->pos.fen().c_str(), move.to_str().c_str(), inc, full);
						my_assert(false);
					}
				}

				// unwind the whole walk in reverse; each unmake must also agree
				// with a fresh evaluation.
				while(!undo_stack.empty()) {
					unmake_move(e, sp.at(0)->pos, undo_stack.back());
					undo_stack.pop_back();

					int inc  = nnue_evaluate(e, sp.at(0)->pos);
					Eval fresh(sp.at(0)->pos);
					int full = nnue_evaluate(&fresh, sp.at(0)->pos);
					if (inc != full) {
						printf("NNUE fuzz unmake mismatch fen %s (%d != %d)\n",
							sp.at(0)->pos.fen().c_str(), inc, full);
						my_assert(false);
					}
				}

				// back at the root: must equal the value recorded before the walk
				if (nnue_evaluate(e, sp.at(0)->pos) != root_score) {
					printf("NNUE fuzz root mismatch fen %s\n", sp.at(0)->pos.fen().c_str());
					my_assert(false);
				}
			}
		}

		printf("OK\n");
	}

	// TT probe/insert/overwrite/age-generation round-trips, plus the
	// move <-> uint conversion fuzz that underpins the tt move field.
	{
		printf("tt round-trip fuzz\n");
		tti.reset();

		// move <-> uint conversion: must round-trip exactly for every type and
		// both promotion and plain moves (the "M" encoding the tt stores).
		for(int i = 0; i < 5000; i++) {
			libchess::Move m1 = fuzz_move();
			uint32_t       v  = libchessmove_to_uint(m1);
			libchess::Move m2 = uint_to_libchessmove(v);
			if (!(m1.from_square() == m2.from_square() &&
			      m1.to_square()   == m2.to_square() &&
			      m1.type()        == m2.type() &&
			      m1.promotion_piece_type() == m2.promotion_piece_type())) {
				printf("move<->uint round-trip fail @%d: %s -> %s\n", i,
					m1.to_str().c_str(), m2.to_str().c_str());
				my_assert(false);
			}
		}

		// insert + probe across many distinct hashes
		for(int i = 0; i < 1000; i++) {
			uint64_t      h = fuzz_rand();
			tt_entry_flag f = tt_entry_flag(fuzz_rand_below(3) + 1);  // EXACT/LOWERBOUND/UPPERBOUND
			int           d  = int(fuzz_rand_below(255));
			int           sc = int(fuzz_rand_below(60001)) - 30000;
			libchess::Move m = fuzz_move();

			tti.store(h, f, d, sc, m);
			auto rec = tti.lookup(h);
			my_assert(rec.has_value());
			tt_entry e = rec.value();
			my_assert(e.depth == uint8_t(d));
			my_assert(e.score == int16_t(sc));
			my_assert(e.flags == f);
			libchess::Move rm = uint_to_libchessmove(e.M);
			my_assert(rm == m && rm.type() == m.type());
		}

		// same-hash overwrite, including the move-preserving store overload
		{
			tti.reset();
			uint64_t      h = fuzz_rand();
			libchess::Move m = fuzz_move();
			tti.store(h, EXACT, 5, 100, m);
			auto r1 = tti.lookup(h);
			my_assert(r1.has_value() && uint_to_libchessmove(r1->M) == m);

			tti.store(h, LOWERBOUND, 9, -22);  // no move supplied: must keep M
			auto r2 = tti.lookup(h);
			my_assert(r2.has_value());
			my_assert(r2->depth == 9 && r2->score == -22 && r2->flags == LOWERBOUND);
			my_assert(uint_to_libchessmove(r2->M) == m);
		}

		// age-generation / reset cycle: a stored record is gone after reset()
		{
			tti.reset();
			uint64_t h = fuzz_rand();
			tti.store(h, UPPERBOUND, 3, 7, fuzz_move());
			my_assert(tti.lookup(h).has_value());
			tti.reset();
			my_assert(tti.lookup(h).has_value() == false);
		}

		printf("OK\n");
	}

	// mate-in-N depth sweeps: bounded searches must find known mates
	{
		printf("mate-in-N depth sweep\n");
		// { fen, depth at which the mate must already be found }
		const std::vector<std::pair<std::string, int> > mates {
			{ "6k1/R7/6K1/8/8/8/8/8 w - - 0 1", 1 },  // a7a8 mate in 1
			{ "6k1/8/6K1/5R2/8/8/8/8 w - - 0 1", 3 },  // K+R, mate in 2 by depth 3
			{ "6k1/8/8/8/8/8/8/R3R1K1 w - - 0 1", 5 },  // two-rook ladder, mate in 3 by depth 5
		};

		for(auto & m: mates) {
			sp.at(0)->pos = Position(m.first);
			init_move(sp.at(0)->nnue_eval, sp.at(0)->pos);
			clear_flag(sp.at(0)->stop);
			memset(sp.at(0)->history, 0x00, history_malloc_size);
			auto rc = search_it(0, 0, false, sp.at(0), m.second, 2000000, O_NONE, false);
			int score = std::get<1>(rc);
			if (abs(score) < max_non_mate) {
				printf("mate-in-N: %s at depth %d scored %d (no mate), best %s\n",
					m.first.c_str(), m.second, score, std::get<0>(rc).to_str().c_str());
				my_assert(false);
			}
		}

		printf("OK\n");
	}

	// aspiration-window / fail-low / fail-high sanity on quiet positions: a
	// bounded search must return a legal move and an in-range score without
	// crashing, and must complete at least one iteration.
	{
		printf("aspiration-window sanity\n");
		const std::vector<std::string> quiet {
			constants::STARTPOS_FEN,
			"r1bq1rk1/pppp1ppp/5n2/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQ1RK1 w - - 0 7",
			"r3k2r/pp1n1ppp/2p2n2/8/2B1P3/2N2N2/PP3PPP/R1BQ1RK1 w kq - 0 9",
			"r1b1k2r/pppp1ppp/2n5/8/4P3/2N2N2/PP1P1PPP/R1BQK2R w KQkq - 0 5",
		};

		for(auto & fen: quiet) {
			sp.at(0)->pos = Position(fen);
			init_move(sp.at(0)->nnue_eval, sp.at(0)->pos);
			clear_flag(sp.at(0)->stop);
			memset(sp.at(0)->history, 0x00, history_malloc_size);
			auto   legal = sp.at(0)->pos.legal_move_list();
			auto   rc    = search_it(0, 0, false, sp.at(0), 6, 120000, O_NONE, false);
			Move   best  = std::get<0>(rc);
			int    score = std::get<1>(rc);
			int    md    = std::get<2>(rc);

			bool legal_found = false;
			for(auto & m: legal) {
				if (m == best) {
					legal_found = true;
					break;
				}
			}
			my_assert(legal_found);
			my_assert(score >= -max_eval && score <= max_eval);
			my_assert(md >= 1);
		}

		printf("OK\n");
	}

	delete_threads();
}

void run_tests()
{
	// because of ESP32 stack
	auto th = new std::thread{tests};
	th->join();
	delete th;
}

#if !defined(ESP32)
std::vector<std::pair<libchess::Position, const std::string> > load_epd(const std::string & filename)
{
	std::vector<std::pair<libchess::Position, const std::string> > out;

	FILE *fh = fopen(filename.c_str(), "r");
	while(!feof(fh)) {
		char buffer[4096];
		if (!fgets(buffer, sizeof buffer, fh))
			break;

		auto parts = split(buffer, " ");
		std::string fen = parts[0] + " " + parts[1] + " " + parts[2] + " " + parts[3];

		auto check = parts[4];
		if (check != "bm")
			continue;

		auto   move = parts[5];
		size_t sc   = move.find(';');
		if (sc != std::string::npos)
			move = move.substr(0, sc);

		out.push_back({ libchess::Position(fen), move });
	}
	fclose(fh);

	return out;
}

void test_mate_finder(const std::string & filename, const int search_time)
{
	int         mates_found = 0;
	auto        positions   = load_epd(filename);
	size_t      n           = positions.size();
	printf("Loaded %zu tests\n", n);

	for(size_t i=0; i<n; i++) {
		clear_flag(sp.at(0)->stop);
		sp.at(0)->pos = positions.at(i).first;
		auto rc  = search_it(search_time, search_time, false, sp.at(0), -1, { }, O_NONE, false);

		bool hit = abs(std::get<1>(rc)) >= max_non_mate;
		mates_found += hit;
	}

	printf("%d %.2f %zu\n", mates_found, mates_found * 100. / n, n);
}
#endif
