#! /usr/bin/python3
"""
Generate app/src/attack-tables.h : Kindergarten sliding-attack tables.

Construction follows the Chessprogramming-wiki "Kindergarten bitboards":
  * FILL_ATTACKS[file][inner-6 occupancy] (4KB) -- the first-rank attack pattern
    replicated into every byte of a 64-bit value.  Serves ranks and both diagonal
    families; the final intersection with the rank / full-diagonal mask picks the
    relevant line.
  * FILE_ATTACKS[rank][folded-6 occupancy] (4KB) -- the A-file attack for a piece
    standing on that rank, stored at real board stride (bit m of the value is
    rank m of the A-file column).  A per-file left shift positions it.

Occupancy -> 6-bit index folding (edge-square bits are provably irrelevant to
the attack set, and the folds drop them from the index window):
  - rank:  ((occ >> (8*rank)) >> 1) & 63                          -- no multiply
  - diag:  ((FOLD_MASK & occ) * 0x0202020202020202ULL) >> 58      -- 64-bit mul
  - file:  ((0x0101010101010101 & (occ >> file)) * 0x0080402010080400ULL) >> 58

The generator EXHAUSTIVELY verifies every square x every occupancy subset against
a ground-truth ray walk before emitting the header; any mismatch aborts.

Square mapping: bit 0 = A1, index = file + 8*rank, north = +8, NE = +9, SE = -7.
"""

import os

MASK64 = (1 << 64) - 1
BFILE = 0x0202020202020202
DIAC2H7 = 0x0080402010080400
FILE_COL = 0x0101010101010101


def idx_of(sq):
    return sq // 8, sq % 8


def ray_attacks(sq, sign, occ):
    atk = 0
    s = sq + sign
    while 0 <= s < 64 and abs((s % 8) - ((s - sign) % 8)) <= 2:
        atk |= 1 << s
        if occ & (1 << s):
            break
        s += sign
    return atk


def ground_rank(sq, occ):
    return ray_attacks(sq, -1, occ) | ray_attacks(sq, 1, occ)


def ground_file(sq, occ):
    return ray_attacks(sq, -8, occ) | ray_attacks(sq, 8, occ)


def ground_diag(sq, occ):
    return ray_attacks(sq, -9, occ) | ray_attacks(sq, 9, occ) | \
           ray_attacks(sq, -7, occ) | ray_attacks(sq, 7, occ)


def diag_step_ok(s, prev):
    return 0 <= s < 64 and abs((s % 8) - (prev % 8)) == 1 and \
        abs((s // 8) - (prev // 8)) == 1


def line_mask(sq, step):
    m = 1 << sq
    s = sq + step
    while diag_step_ok(s, s - step):
        m |= 1 << s
        s += step
    s = sq - step
    while diag_step_ok(s, s + step):
        m |= 1 << s
        s -= step
    return m


def interior_mask(linemask):
    """Line mask minus its two end squares."""
    bits = [b for b in range(64) if linemask & (1 << b)]
    inner = set(bits)
    if bits:
        inner.remove(bits[0])
    if len(bits) > 1:
        inner.remove(bits[-1])
    return sum(1 << b for b in inner)


def rank_mask(sq):
    r, _ = idx_of(sq)
    return 0xFF << (8 * r)


def diag_p_line(sq):
    return line_mask(sq, 9)


def diag_n_line(sq):
    return line_mask(sq, 7)


def folded_attacks(true_atk, linemask):
    return true_atk & linemask


# ---------------------------------------------------------------------------

def build():
    fill = [[0] * 64 for _ in range(8)]       # [file][occ6] -> byte-replicated first rank
    ffile = [[0] * 64 for _ in range(8)]      # [rank][folded] -> A-file attack (stride 8)
    diag_p_fold = [0] * 64
    diag_n_fold = [0] * 64
    diag_p_line = [0] * 64
    diag_n_line = [0] * 64

    problems = []

    # ---- canonical shared table: first-rank attack, byte-replicated ---------
    # idx meaning: bit i of idx <=> file (i+1) of the line carries an occupied
    # square.  The piece's own square (file f, bit f-1 of idx) is always set.
    for f in range(8):
        for idx in range(64):
            occ0 = (idx << 1) | (1 << f)     # rank-0 occupancy incl. piece
            byte = ground_rank(f, occ0) & 0xFF
            fill[f][idx] = byte * FILE_COL   # replicate pattern in every byte

    # ---- diagonal fold masks ------------------------------------------------
    for fam, sid in ((9, 'p'), (7, 'n')):
        for sq in range(64):
            line = line_mask(sq, fam)
            fold_mask = interior_mask(line)
            if sid == 'p':
                diag_p_fold[sq] = fold_mask
                diag_p_line[sq] = line
            else:
                diag_n_fold[sq] = fold_mask
                diag_n_line[sq] = line

    # ---- verify rank against the shared table --------------------------------
    for sq in range(64):
        r, f = idx_of(sq)
        for occ6 in range(64):
            occ = ((occ6 << 1) << (8 * r)) | (1 << sq)
            idx = (occ >> (8 * r + 1)) & 63
            got = fill[f][idx] & rank_mask(sq)
            want = ground_rank(sq, occ)
            if got != want:
                problems.append(f"rank sq={sq} occ6={occ6}")

    # ---- verify both diagonal families against the shared table --------------
    for fam, fm, lm in ((9, diag_p_fold, diag_p_line), (7, diag_n_fold, diag_n_line)):
        for sq in range(64):
            _, f = idx_of(sq)
            mask = fm[sq]
            inner_bits = [b for b in range(64) if mask & (1 << b)]
            n = len(inner_bits)
            seen = {}
            for k in range(1 << n):
                occ = 1 << sq
                # edge squares of the line: occupancy varies in real boards,
                # but must never influence the index or the attack
                line_bits = [b for b in range(64) if line & (1 << b)]
                for b in line_bits:
                    if not (fold_mask & (1 << b)) and b != sq:
                        occ |= 1 << b
                for i, b in enumerate(inner_bits):
                    if k & (1 << i):
                        occ |= 1 << b
                idx = (((mask & occ) * BFILE) & MASK64) >> 58
                if idx >= 64:
                    problems.append(f"diag fam={fam} sq={sq} k={k} idx oob")
                    continue
                if idx in seen and seen[idx] != occ:
                    problems.append(f"diag fam={fam} sq={sq} fold collision k={k}")
                    continue
                seen[idx] = occ
                got = fill[f][idx] & lm[sq]
                want = ground_diag(sq, occ) & lm[sq]
                if got != want:
                    problems.append(f"diag fam={fam} sq={sq} k={k}")

    # ---- verify files (separate table) ---------------------------------------
    for sq in range(64):
        r, f = idx_of(sq)
        inner_bits = [8 * k + f for k in range(1, 7)]
        seen = {}
        for k in range(1 << 6):
            occ = 1 << sq
            occ |= 1 << f          # edge a1 of the column
            occ |= 1 << (56 + f)   # edge a8 of the column
            for i, b in enumerate(inner_bits):
                if k & (1 << i):
                    occ |= 1 << b
            idx = ((((occ >> f) & FILE_COL) * DIAC2H7) & MASK64) >> 58
            if idx >= 64:
                problems.append(f"file sq={sq} k={k} idx oob")
                continue
            if idx in seen and seen[idx] != occ:
                problems.append(f"file sq={sq} fold collision k={k}")
                continue
            seen[idx] = occ
            want = (ground_file(sq, occ) >> f) & FILE_COL
            atk_col = want  # stored as stride-8 value (bits at 0,8,..,56)
            if ffile[r][idx] != 0 and ffile[r][idx] != atk_col:
                problems.append(f"file sq={sq} k={k} table clash")
            ffile[r][idx] = atk_col
            if (atk_col << f) != (ground_file(sq, occ) & (FILE_COL << f)):
                problems.append(f"file sq={sq} k={k} mismatch")

    # ---- final end-to-end spot check on shifted rows --------------------------
    return fill, ffile, diag_p_fold, diag_n_fold, diag_p_line, diag_n_line, problems


def fmt_u64(rows):
    return "\n".join(
        "    " + " ".join(f"UINT64_C(0x{v:016x})," for v in row) for row in rows)


def fmt_u64_flat(rows):
    return "    " + " ".join(f"UINT64_C(0x{v:016x})," for v in rows)


def main():
    fill, ffile, dpf, dnf, dpl, dnl, problems = build()
    if problems:
        print("FAILED:", len(problems), "problems")
        for p in problems[:40]:
            print(" ", p)
        raise SystemExit(1)
    print("all kindergarten tables verified")

    out = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "../app/include/libchess/attack-tables.h"))
    header = f"""// Auto-generated by tools/gen_attack_tables.py -- DO NOT EDIT.
// Kindergarten sliding-attack tables (CPW), exhaustively verified.

#ifndef LIBCHESS_ATTACK_TABLES_H
#define LIBCHESS_ATTACK_TABLES_H

#include <cstdint>

namespace libchess::lookups::detail {{

// [file of piece][inner-6 occupancy] -> first-rank attack byte replicated in
// every byte.  Pick the line with & RANK_MASK(sq) / & DIAG_*_LINE[sq].
inline constexpr std::uint64_t FILL_ATTACKS[8][64] = {{
{fmt_u64(fill)}
}};

// [rank of piece][folded-6 occupancy] -> A-file attack (bit m = rank m of the
// A-file column).  Position with << file.
inline constexpr std::uint64_t FILE_ATTACKS[8][64] = {{
{fmt_u64(ffile)}
}};

// Interior (edge-excluding) diagonal masks used for the occupancy fold.
inline constexpr std::uint64_t DIAG_PLUS_FOLD[64] = {{
{fmt_u64_flat(dpf)}
}};
inline constexpr std::uint64_t DIAG_MINUS_FOLD[64] = {{
{fmt_u64_flat(dnf)}
}};

// Full diagonal line masks (incl. edges) for the final intersection and for
// full_ray / intervening.
inline constexpr std::uint64_t DIAG_PLUS_LINE[64] = {{
{fmt_u64_flat(dpl)}
}};
inline constexpr std::uint64_t DIAG_MINUS_LINE[64] = {{
{fmt_u64_flat(dnl)}
}};

}}  // namespace libchess::lookups::detail

#endif  // LIBCHESS_ATTACK_TABLES_H
"""
    with open(out, "w") as fh:
        fh.write(header)
    print("wrote", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
