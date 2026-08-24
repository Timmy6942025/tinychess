#include <cassert>
#include <libchess/Position.h>
#include "eval.h"
#include "main.h"
#include "nnue.h"

#if defined(ESP32)
#include <esp_attr.h>
#endif


using namespace libchess;

int IRAM_ATTR nnue_evaluate(const Eval *const e, const Position & pos)
{
        return e->evaluate(pos.side_to_move() == constants::WHITE);
}

int IRAM_ATTR nnue_evaluate(const Eval *const e, const Color & c)
{
        return e->evaluate(c == constants::WHITE);
}

void init_move(Eval *const e, const libchess::Position & pos)
{
	e->set(pos);
}

// All of a move's accumulator deltas are batched into a single sweep over
// both perspectives (see nnue_kernels.h). Each branch below records the same
// facts the old code applied immediately - removals first, additions last -
// into the batch plus the undo journal, so unmake_move can rebuild and apply
// the exact inverse batch. Order inside a batch is arithmetically free
// (addition mod 2^16 commutes); fixing it keeps the board's decomposition
// simple.

std::pair<int, std::array<undo_t, 4> > IRAM_ATTR make_move(Eval *const e, Position & pos, const Move & move)
{
	int                   n_actions = 0;
	std::array<undo_t, 4> actions { {
		{ libchess::constants::A1, libchess::constants::PAWN, false, false },
		{ libchess::constants::A1, libchess::constants::PAWN, false, false },
		{ libchess::constants::A1, libchess::constants::PAWN, false, false },
		{ libchess::constants::A1, libchess::constants::PAWN, false, false }
	} };

	nnue_k::Delta deltas[4];
	int           n_deltas = 0;

	Square from_square = move.from_square();
	Square to_square   = move.to_square  ();

	auto moving_pt    = pos.piece_type_on(from_square);
	assert(moving_pt.has_value());
	auto captured_pt  = pos.piece_type_on(to_square  );
	auto promotion_pt = move.promotion_piece_type();

	bool is_white     = pos.side_to_move() == constants::WHITE;

	assert(move.type() != Move::Type::ENPASSANT || (is_white && move.to_square().rank() == 5) || (!is_white && move.to_square().rank() == 2));

	auto remove_piece = [&](const Square & loc, const PieceType & pt, const bool was_white) {
		e->push_delta(deltas, n_deltas, pt, loc.value(), was_white, false);
		actions.at(n_actions++) = { loc, pt, was_white, true };   // inverse: put back
	};
	auto add_piece = [&](const Square & loc, const PieceType & pt, const bool now_white) {
		e->push_delta(deltas, n_deltas, pt, loc.value(), now_white, true);
		actions.at(n_actions++) = { loc, pt, now_white, false };  // inverse: remove
	};
	auto move_piece = [&](const Square & from, const Square & to, const PieceType & pt, const bool piece_is_white) {
		remove_piece(from, pt, piece_is_white);
		add_piece   (to,   pt, piece_is_white);
	};

	switch(move.type()) {
		case Move::Type::NORMAL:
		case Move::Type::DOUBLE_PUSH:
			assert(moving_pt.has_value());
			move_piece(from_square, to_square, *moving_pt, is_white);
			assert(*moving_pt == constants::PAWN || move.type() != Move::Type::DOUBLE_PUSH);
			break;
		case Move::Type::CAPTURE:
			assert(captured_pt.has_value());
			assert(pos.color_of(from_square) != pos.color_of(to_square));
			remove_piece(to_square, *captured_pt, !is_white);
			move_piece(from_square, to_square, *moving_pt, is_white);
			break;
		case Move::Type::ENPASSANT:
			assert(*moving_pt == constants::PAWN);
			assert(pos.color_of(from_square) != pos.color_of(is_white ? Square(to_square - 8) : Square(to_square + 8)));
			remove_piece(from_square, constants::PAWN, is_white);
			assert(pos.piece_type_on(is_white ? Square(to_square - 8) : Square(to_square + 8)) == constants::PAWN);
			remove_piece(is_white ? Square(to_square - 8) : Square(to_square + 8), constants::PAWN, !is_white);
			add_piece   (to_square, constants::PAWN, is_white);
			break;
		case Move::Type::CASTLING:
			assert(*moving_pt == constants::KING);
			assert(pos.color_of(from_square) == (is_white ? constants::WHITE : constants::BLACK));
			switch (to_square) {
				case constants::C1:
					assert(is_white);
					assert(pos.color_of(constants::A1) == constants::WHITE);
					assert(pos.piece_type_on(constants::D1).has_value() == false);
					remove_piece(constants::E1, constants::KING, true);
					remove_piece(constants::A1, constants::ROOK, true);
					add_piece   (constants::C1, constants::KING, true);
					add_piece   (constants::D1, constants::ROOK, true);
					break;
				case constants::G1:
					assert(is_white);
					assert(pos.color_of(constants::H1) == constants::WHITE);
					assert(pos.piece_type_on(constants::F1).has_value() == false);
					remove_piece(constants::E1, constants::KING, true);
					remove_piece(constants::H1, constants::ROOK, true);
					add_piece   (constants::G1, constants::KING, true);
					add_piece   (constants::F1, constants::ROOK, true);
					break;
				case constants::C8:
					assert(!is_white);
					assert(pos.color_of(constants::A8) == constants::BLACK);
					assert(pos.piece_type_on(constants::D8).has_value() == false);
					remove_piece(constants::E8, constants::KING, false);
					remove_piece(constants::A8, constants::ROOK, false);
					add_piece   (constants::C8, constants::KING, false);
					add_piece   (constants::D8, constants::ROOK, false);
					break;
				case constants::G8:
					assert(!is_white);
					assert(pos.color_of(constants::H8) == constants::BLACK);
					assert(pos.piece_type_on(constants::F8).has_value() == false);
					remove_piece(constants::E8, constants::KING, false);
					remove_piece(constants::H8, constants::ROOK, false);
					add_piece   (constants::G8, constants::KING, false);
					add_piece   (constants::F8, constants::ROOK, false);
					break;
				default:
					assert(false);
					break;
			}
			break;
		case Move::Type::PROMOTION:
			assert(*moving_pt    == constants::PAWN);
			assert(*promotion_pt != constants::PAWN);
			assert((pos.color_of(from_square) == constants::WHITE && to_square.rank() == 7) ||
			       (pos.color_of(from_square) == constants::BLACK && to_square.rank() == 0));
			remove_piece(from_square, constants::PAWN, is_white);
			add_piece   (to_square,  *promotion_pt,    is_white);
			break;
		case Move::Type::CAPTURE_PROMOTION:
			assert(*moving_pt == constants::PAWN);
			assert(*promotion_pt != constants::PAWN);
			assert((pos.color_of(from_square) == constants::WHITE && to_square.rank() == 7) ||
			       (pos.color_of(from_square) == constants::BLACK && to_square.rank() == 0));
			remove_piece(to_square,  *captured_pt,    !is_white);
			remove_piece(from_square, constants::PAWN, is_white);
			add_piece   (to_square,  *promotion_pt,    is_white);
			break;
		default:
			printf("type is %d\n", int(move.type()));
			assert(false);
			break;
	}

	nnue_k::apply(e->acc_white().data(), e->acc_black().data(), deltas, n_deltas);

	pos.make_move(move);

#if !defined(NDEBUG)
	if (pos.enpassant_square().has_value()) {
		auto file = pos.enpassant_square().value().file();
		if (is_white) {
			assert(pos.enpassant_square().value().rank() == 2);
			assert(pos.piece_on(libchess::Square::from(file, libchess::Rank(1)).value()).has_value() == false);
		}
		else {
			assert(pos.enpassant_square().value().rank() == 5);
			assert(pos.piece_on(libchess::Square::from(file, libchess::Rank(6)).value()).has_value() == false);
		}
	}
#endif

	return { n_actions, actions };
}

void IRAM_ATTR unmake_move(Eval *const e, Position & pos, const std::pair<int, std::array<undo_t, 4> > & actions)
{
	nnue_k::Delta deltas[4];
	int           n_deltas = 0;

	// rebuild the inverse batch: every recorded fact flipped, removals
	// first, additions last (canonical order for the board kernels)
	for(int pass=0; pass<2; pass++) {
		for(int i=0; i<actions.first; i++) {
			auto & action = actions.second[i];
			const bool add_now = action.is_put;
			if ((pass == 0) == add_now)
				continue;
			e->push_delta(deltas, n_deltas, action.type, action.location.value(), action.is_white, add_now);
		}
	}

	nnue_k::apply(e->acc_white().data(), e->acc_black().data(), deltas, n_deltas);

	pos.unmake_move();
}
