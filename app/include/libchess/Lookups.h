#ifndef LIBCHESS_LOOKUPS_H
#define LIBCHESS_LOOKUPS_H

#include <array>
#include <cstdint>

#include "Bitboard.h"
#include "Color.h"
#include "PieceType.h"
#include "attack-tables.h"

namespace libchess::lookups {

constexpr static Bitboard RANK_1_MASK{std::uint64_t(0xff)};
constexpr static Bitboard RANK_2_MASK{std::uint64_t(0xff00)};
constexpr static Bitboard RANK_3_MASK{std::uint64_t(0xff0000)};
constexpr static Bitboard RANK_4_MASK{std::uint64_t(0xff000000)};
constexpr static Bitboard RANK_5_MASK{std::uint64_t(0xff00000000)};
constexpr static Bitboard RANK_6_MASK{std::uint64_t(0xff0000000000)};
constexpr static Bitboard RANK_7_MASK{std::uint64_t(0xff000000000000)};
constexpr static Bitboard RANK_8_MASK{std::uint64_t(0xff00000000000000)};
constexpr static Bitboard FILE_A_MASK{std::uint64_t(0x0101010101010101)};
constexpr static Bitboard FILE_B_MASK{std::uint64_t(0x0202020202020202)};
constexpr static Bitboard FILE_C_MASK{std::uint64_t(0x0404040404040404)};
constexpr static Bitboard FILE_D_MASK{std::uint64_t(0x0808080808080808)};
constexpr static Bitboard FILE_E_MASK{std::uint64_t(0x1010101010101010)};
constexpr static Bitboard FILE_F_MASK{std::uint64_t(0x2020202020202020)};
constexpr static Bitboard FILE_G_MASK{std::uint64_t(0x4040404040404040)};
constexpr static Bitboard FILE_H_MASK{std::uint64_t(0x8080808080808080)};

constexpr static std::array<Bitboard, 8> RANK_MASK = {RANK_1_MASK,
                                            RANK_2_MASK,
                                            RANK_3_MASK,
                                            RANK_4_MASK,
                                            RANK_5_MASK,
                                            RANK_6_MASK,
                                            RANK_7_MASK,
                                            RANK_8_MASK};
constexpr static std::array<Bitboard, 8> FILE_MASK = {FILE_A_MASK,
                                            FILE_B_MASK,
                                            FILE_C_MASK,
                                            FILE_D_MASK,
                                            FILE_E_MASK,
                                            FILE_F_MASK,
                                            FILE_G_MASK,
                                            FILE_H_MASK};

inline Bitboard rank_mask(Rank rank) {
    return RANK_MASK[rank.value()];
}
inline Bitboard file_mask(File file) {
    return FILE_MASK[file.value()];
}

namespace init {

constexpr inline std::array<Bitboard, 64> north() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A1; sq <= constants::H7; ++sq) {
        Bitboard bb;
        for (Square atk_sq = sq + 8; atk_sq <= constants::H8; atk_sq = atk_sq + 8) {
            bb |= Bitboard{atk_sq};
        }
        attacks[sq] = bb;
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> south() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A2; sq <= constants::H8; ++sq) {
        Bitboard bb;
        for (Square atk_sq = sq - 8; atk_sq >= constants::A1; atk_sq = atk_sq - 8) {
            bb |= Bitboard{atk_sq};
        }
        attacks[sq] = bb;
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> east() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A1; sq <= constants::G8; ++sq) {
        Bitboard bb;
        for (Square atk_sq = sq + 1; atk_sq <= constants::H8; atk_sq = atk_sq + 1) {
            if (Bitboard{atk_sq} & FILE_A_MASK) {
                break;
            }
            bb |= Bitboard{atk_sq};
        }
        attacks[sq] = bb;
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> west() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::B1; sq <= constants::H8; ++sq) {
        Bitboard bb;
        for (Square atk_sq = sq - 1; atk_sq >= constants::A1; atk_sq = atk_sq - 1) {
            if (Bitboard{atk_sq} & FILE_H_MASK) {
                break;
            }
            bb |= Bitboard{atk_sq};
        }
        attacks[sq] = bb;
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> northwest() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A1; sq <= constants::H7; ++sq) {
        Bitboard bb;
        for (Square atk_sq = sq + 7; atk_sq <= constants::H8; atk_sq = atk_sq + 7) {
            if (Bitboard{atk_sq} & FILE_H_MASK) {
                break;
            }
            bb |= Bitboard{atk_sq};
        }
        attacks[sq] = bb;
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> southwest() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::B2; sq <= constants::H8; ++sq) {
        Bitboard bb;
        for (Square atk_sq = sq - 9; atk_sq >= constants::A1; atk_sq = atk_sq - 9) {
            if (Bitboard{atk_sq} & FILE_H_MASK) {
                break;
            }
            bb |= Bitboard{atk_sq};
        }
        attacks[sq] = bb;
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> northeast() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A1; sq <= constants::G7; ++sq) {
        Bitboard bb;
        for (Square atk_sq = sq + 9; atk_sq <= constants::H8; atk_sq = atk_sq + 9) {
            if (Bitboard{atk_sq} & FILE_A_MASK) {
                break;
            }
            bb |= Bitboard{atk_sq};
        }
        attacks[sq] = bb;
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> southeast() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A2; sq <= constants::H8; ++sq) {
        Bitboard bb;
        for (Square atk_sq = sq - 7; atk_sq >= constants::A1; atk_sq = atk_sq - 7) {
            if (Bitboard{atk_sq} & FILE_A_MASK) {
                break;
            }
            bb |= Bitboard{atk_sq};
        }
        attacks[sq] = bb;
    }
    return attacks;
}

}  // namespace init

// Direction bitboards
constexpr static std::array<Bitboard, 64> NORTH = init::north();
constexpr static std::array<Bitboard, 64> SOUTH = init::south();
constexpr static std::array<Bitboard, 64> EAST = init::east();
constexpr static std::array<Bitboard, 64> WEST = init::west();
constexpr static std::array<Bitboard, 64> NORTHWEST = init::northwest();
constexpr static std::array<Bitboard, 64> SOUTHWEST = init::southwest();
constexpr static std::array<Bitboard, 64> NORTHEAST = init::northeast();
constexpr static std::array<Bitboard, 64> SOUTHEAST = init::southeast();

constexpr static Bitboard north(Square square) {
    return NORTH[square];
}
constexpr static Bitboard south(Square square) {
    return SOUTH[square];
}
constexpr static Bitboard east(Square square) {
    return EAST[square];
}
constexpr static Bitboard west(Square square) {
    return WEST[square];
}
constexpr static Bitboard northwest(Square square) {
    return NORTHWEST[square];
}
constexpr static Bitboard southwest(Square square) {
    return SOUTHWEST[square];
}
constexpr static Bitboard northeast(Square square) {
    return NORTHEAST[square];
}
constexpr static Bitboard southeast(Square square) {
    return SOUTHEAST[square];
}

constexpr static std::uint64_t FILE_A_COLUMN{0x0101010101010101ULL};
constexpr static std::uint64_t B_FILE_FOLD{0x0202020202020202ULL};
constexpr static std::uint64_t DIAG_C2H7_FOLD{0x0080402010080400ULL};

namespace init {

constexpr inline std::array<std::array<libchess::Bitboard, 64>, 2> pawn_attacks() {
    std::array<std::array<libchess::Bitboard, 64>, 2> attacks{};
    for (Square sq = constants::A1; sq <= constants::H8; ++sq) {
        if (sq <= constants::H7) {
            attacks[constants::WHITE][sq] |= Bitboard{sq + 7} & ~FILE_H_MASK;
        }
        if (sq <= constants::G7) {
            attacks[constants::WHITE][sq] |= Bitboard{sq + 9} & ~FILE_A_MASK;
        }
        if (sq >= constants::A2) {
            attacks[constants::BLACK][sq] |= Bitboard{sq - 7} & ~FILE_A_MASK;
        }
        if (sq >= constants::B2) {
            attacks[constants::BLACK][sq] |= Bitboard{sq - 9} & ~FILE_H_MASK;
        }
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> knight_attacks() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A1; sq <= constants::H8; ++sq) {
        if (sq <= constants::G6) {
            attacks[sq] |= Bitboard{sq + 17} & ~FILE_A_MASK;
        }
        if (sq <= constants::H6) {
            attacks[sq] |= Bitboard{sq + 15} & ~FILE_H_MASK;
        }
        if (sq >= constants::B3) {
            attacks[sq] |= Bitboard{sq - 17} & ~FILE_H_MASK;
        }
        if (sq >= constants::A3) {
            attacks[sq] |= Bitboard{sq - 15} & ~FILE_A_MASK;
        }
        if (sq <= constants::F7) {
            attacks[sq] |= Bitboard{sq + 10} & ~(FILE_A_MASK | FILE_B_MASK);
        }
        if (sq <= constants::H7) {
            attacks[sq] |= Bitboard{sq + 6} & ~(FILE_H_MASK | FILE_G_MASK);
        }
        if (sq >= constants::C2) {
            attacks[sq] |= Bitboard{sq - 10} & ~(FILE_H_MASK | FILE_G_MASK);
        }
        if (sq >= constants::A2) {
            attacks[sq] |= Bitboard{sq - 6} & ~(FILE_A_MASK | FILE_B_MASK);
        }
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> king_attacks() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A1; sq <= constants::H8; ++sq) {
        if (sq <= constants::G7) {
            attacks[sq] |= Bitboard{sq + 9} & ~FILE_A_MASK;
        }
        if (sq <= constants::H7) {
            attacks[sq] |= Bitboard{sq + 8};
            attacks[sq] |= Bitboard{sq + 7} & ~FILE_H_MASK;
        }
        if (sq <= constants::G8) {
            attacks[sq] |= Bitboard{sq + 1} & ~FILE_A_MASK;
        }
        if (sq >= constants::B1) {
            attacks[sq] |= Bitboard{sq - 1} & ~FILE_H_MASK;
        }
        if (sq >= constants::A2) {
            attacks[sq] |= Bitboard{sq - 7} & ~FILE_A_MASK;
            attacks[sq] |= Bitboard{sq - 8};
        }
        if (sq >= constants::B2) {
            attacks[sq] |= Bitboard{sq - 9} & ~FILE_H_MASK;
        }
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> bishop_attacks() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A1; sq <= constants::H8; ++sq) {
        attacks[sq] = lookups::northeast(sq) | lookups::southeast(sq) | lookups::southwest(sq) |
                      lookups::northwest(sq);
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> rook_attacks() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A1; sq <= constants::H8; ++sq) {
        attacks[sq] =
            lookups::north(sq) | lookups::east(sq) | lookups::south(sq) | lookups::west(sq);
    }
    return attacks;
}

constexpr inline std::array<Bitboard, 64> queen_attacks() {
    std::array<Bitboard, 64> attacks{};
    for (Square sq = constants::A1; sq <= constants::H8; ++sq) {
        attacks[sq] = lookups::north(sq) | lookups::east(sq) | lookups::south(sq) |
                      lookups::west(sq) | lookups::northwest(sq) | lookups::northeast(sq) |
                      lookups::southwest(sq) | lookups::southeast(sq);
    }
    return attacks;
}

}  // namespace init

// Piece attack bitboards
constexpr static std::array<std::array<Bitboard, 64>, 2> PAWN_ATTACKS = init::pawn_attacks();
constexpr static std::array<Bitboard, 64> KNIGHT_ATTACKS = init::knight_attacks();
constexpr static std::array<Bitboard, 64> KING_ATTACKS = init::king_attacks();
constexpr static std::array<Bitboard, 64> BISHOP_ATTACKS = init::bishop_attacks();
constexpr static std::array<Bitboard, 64> ROOK_ATTACKS = init::rook_attacks();
constexpr static std::array<Bitboard, 64> QUEEN_ATTACKS = init::queen_attacks();

constexpr inline Bitboard pawn_attacks(Square square, Color color) {
    return PAWN_ATTACKS[color][square];
}
constexpr inline Bitboard knight_attacks(Square square, Bitboard = Bitboard{}) {
    return KNIGHT_ATTACKS[square];
}
constexpr inline Bitboard king_attacks(Square square, Bitboard = Bitboard{}) {
    return KING_ATTACKS[square];
}
constexpr inline Bitboard bishop_attacks(Square square) {
    return BISHOP_ATTACKS[square];
}
constexpr inline Bitboard rook_attacks(Square square) {
    return ROOK_ATTACKS[square];
}
constexpr inline Bitboard queen_attacks(Square square) {
    return QUEEN_ATTACKS[square];
}
LIBCHESS_IRAM_ATTR inline Bitboard bishop_attacks(Square square, Bitboard occupancy) {
    unsigned sq = static_cast<unsigned>(square.value());
    unsigned file = static_cast<unsigned>(square.file().value());
    std::uint64_t occ = static_cast<std::uint64_t>(occupancy) | (UINT64_C(1) << sq);
    std::uint64_t idx_p = ((detail::DIAG_PLUS_FOLD[sq] & occ) * B_FILE_FOLD) >> 58;
    std::uint64_t idx_n = ((detail::DIAG_MINUS_FOLD[sq] & occ) * B_FILE_FOLD) >> 58;
    return (Bitboard{detail::FILL_ATTACKS[file][idx_p]} & Bitboard{detail::DIAG_PLUS_LINE[sq]})
           | (Bitboard{detail::FILL_ATTACKS[file][idx_n]} & Bitboard{detail::DIAG_MINUS_LINE[sq]});
}
LIBCHESS_IRAM_ATTR inline Bitboard rook_attacks(Square square, Bitboard occupancy) {
    unsigned sq = static_cast<unsigned>(square.value());
    unsigned rank = static_cast<unsigned>(square.rank().value());
    unsigned file = static_cast<unsigned>(square.file().value());
    std::uint64_t occ = static_cast<std::uint64_t>(occupancy) | (UINT64_C(1) << sq);
    std::uint64_t idx_r = (occ >> (rank * 8 + 1)) & 63;
    std::uint64_t idx_f = (((occ >> file) & FILE_A_COLUMN) * DIAG_C2H7_FOLD) >> 58;
    return (Bitboard{detail::FILL_ATTACKS[file][idx_r]} & rank_mask(square.rank()))
           | (Bitboard{detail::FILE_ATTACKS[rank][idx_f]} << static_cast<int>(file));
}
LIBCHESS_IRAM_ATTR inline Bitboard queen_attacks(Square square, Bitboard occupancy) {
    return rook_attacks(square, occupancy) | bishop_attacks(square, occupancy);
}
LIBCHESS_IRAM_ATTR inline Bitboard pawn_shift(Bitboard bb, Color c, int times = 1) {
    return c == constants::WHITE ? bb << (8 * times) : bb >> (8 * times);
}
inline Square pawn_shift(Square sq, Color c, int times = 1) {
    return c == constants::WHITE ? sq + (8 * times) : sq - (8 * times);
}
inline Rank relative_rank(Rank rank, Color c) {
    return c == constants::WHITE
               ? rank
               : Rank{static_cast<Rank::value_type>(constants::RANK_8.value() - rank.value())};
}
LIBCHESS_IRAM_ATTR inline Bitboard relative_rank_mask(Rank rank, Color c) {
    return rank_mask(relative_rank(rank, c));
}
LIBCHESS_IRAM_ATTR inline Bitboard non_pawn_piece_type_attacks(PieceType piece_type,
                                            Square square,
                                            Bitboard occupancies = Bitboard{}) {
    switch (piece_type) {
        case constants::KNIGHT:
            return knight_attacks(square);
        case constants::BISHOP:
            return bishop_attacks(square, occupancies);
        case constants::ROOK:
            return rook_attacks(square, occupancies);
        case constants::QUEEN:
            return queen_attacks(square, occupancies);
        case constants::KING:
            return king_attacks(square);
        default:
            return Bitboard{};
    }
}

LIBCHESS_IRAM_ATTR inline Bitboard full_ray(Square from, Square to) {
    if (from == to) {
        return Bitboard{};
    }
    Square high = to;
    Square low = from;
    if (low > high) {
        high = from;
        low = to;
    }
    if (high.file() == low.file()) {
        return file_mask(high.file());
    }
    if (high.rank() == low.rank()) {
        return rank_mask(high.rank());
    }
    if (high.file() - low.file() == high.rank() - low.rank()) {
        return Bitboard{detail::DIAG_PLUS_LINE[from.value()]};
    }
    if (low.file() - high.file() == high.rank() - low.rank()) {
        return Bitboard{detail::DIAG_MINUS_LINE[from.value()]};
    }
    return Bitboard{};
}

LIBCHESS_IRAM_ATTR inline Bitboard intervening(Square from, Square to) {
    if (from == to) {
        return Bitboard{};
    }
    Square high = to;
    Square low = from;
    if (low > high) {
        high = from;
        low = to;
    }
    if (high.file() == low.file()) {
        return north(low) & south(high);
    }
    if (high.rank() == low.rank()) {
        return east(low) & west(high);
    }
    if (high.file() - low.file() == high.rank() - low.rank()) {
        return northeast(low) & southwest(high);
    }
    if (low.file() - high.file() == high.rank() - low.rank()) {
        return northwest(low) & southeast(high);
    }
    return Bitboard{};
}

}  // namespace libchess::lookups

#endif  // LIBCHESS_LOOKUPS_H
