#!/usr/bin/env python3
# Generate app/src/lmr-red.h reduction tables.
# Usage: python3 tools/gen_lmr_table.py MUL BASE > app/src/lmr-red.h
import math, sys

def gen(mul, base):
    n_d, n_m = 64, 64
    out = [f"#define N_LMR_DEPTH {n_d}", f"#define N_LMR_MOVES {n_m}",
           "constexpr const uint8_t lmr_reductions[N_LMR_DEPTH][N_LMR_MOVES] = {"]
    for depth in range(n_d):
        row = ["0 "]*n_m if depth == 0 else [f"{int(math.log(depth)*math.log(np+1)*mul+base)} " for np in range(n_m)]
        out.append("	{" + " ".join(row).strip() + "},")
    out.append("};")
    return "
".join(out)

if __name__ == "__main__":
    print(gen(float(sys.argv[1]), float(sys.argv[2])))
