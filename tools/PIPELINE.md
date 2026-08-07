# MaxDogOne experiment pipeline

Auto-worker handoff. A cron job (every 10 min) spawns a headless `opencode run`
when no SPRT match is running. Follow this file exactly. Repo:
/home/timmy/chess2/engine (run every command from the repo root).
Authoritative state: `tools/results.log` (verdicts + binary fingerprints) and
`git log`. This file's "Current state" section must be refreshed after every
commit that changes it.

## Hard rules
1. One SPRT at a time. In fast_sprt.sh the convention is A=CHANGE build, B=base
   build. ACCEPT => A stronger => KEEP the change; REJECT => drop it;
   KEEP/REVERT => game cap reached, decided by llr sign.
2. Never run cmake/make (tools/native_check.sh, ensure_build) while a match is
   running: the -j4 build OOM-killed cutechess-cli on this Pi before (swap was
   96% full; orphaned engine children spun at ~60% CPU for 20 min). Do ALL
   builds BEFORE launching the next match.
3. Never overwrite tools/runs/bin/Dog-* binaries a live match uses. After any
   build, md5sum the two binaries and confirm the pair matches the FINGERPRINT
   lines in tools/results.log before launching.
4. Launch matches fully detached (a shell session kills its children
   otherwise):
   CONC=2 TC=5+0.05 SPRT_MAX=1500 setsid nohup bash tools/fast_sprt.sh ab <tag> \
       tools/runs/bin/<A-binary> tools/runs/bin/<B-binary> > /tmp/opencode/<tag>.out 2>&1 &
5. Gate before any SPRT: tools/native_check.sh must pass all unit tests
   (17 test groups, 0 failures). Build must show 0 errors:
   `cmake --build app/src/linux-windows/build --target Dog-native -j4 2>&1 | grep -cE " error"`.
   Optional extra: `python3 tools/epd_test.py --engine <bin> --suite tools/suites/wacnew.epd --time 1000`
   (reference 260/299).
6. Bench NPS is meaningless while any match runs (CPU contention).
7. If the git state or artifacts deviate from what this file documents, STOP:
   write a short report to /tmp/opencode/pipeline.state and append a
   "PIPELINE STOP <tag> <reason>" line to tools/results.log. Do not guess.
   (Headless: you cannot ask the user questions.)
8. Only commit search/eval changes on main after SPRT ACCEPT. Work-in-progress
   changes live as patches in tools/patches/ until accepted. The unit-test
   gate is mandatory: variants that fail the mate-in-N sweep (two-rook ladder
   `6k1/8/8/8/8/8/8/R3R1K1` at depth 19 must score >= max_non_mate) are
   rejected BEFORE any SPRT - the accepted LMR table sits at the aggressive
   limit, and LMR x1.15, futility 220+180d, and LMP have all died on exactly
   that test.

## Current state (2026-08-07, HEAD 331e438 - in sync with origin/main)
- 8-item board-free plan: ALL CLOSED. Verdicts:
  item1 idf esp32s3 build RC=0 | item2 rating anchor -56.1 +/- 43.4 @2+0.02 (200g,
  63-95-42 vs Stockfish 17) | item3 RESEARCH.md | item4 tools/bench.csv |
  item5 time-management (MOVE_OVERHEAD_MS 100, forfeit-free, cutechess-validated) |
  item6 LMR table recalibrated mul 0.65 ACCEPT (+34.6, ab-lmr065) |
  item7 static-null/razor/futility calibration ALL REJECT |
  item8 RukChess 768->512->1 converter + gated Dog-ruk target REJECT (ab-ruk512,
  llr -2.22) - keep gated, do not retry without a real RukChess net
- Current best binary: tools/runs/bin/Dog-lmr065 = Dog-baseline-new
  md5 93b244eecd33079ba000545ed0ed57f4 (node-identical to Dog-native
  md5 b78c20ca66e79b8b8fcdf6f270101ac5 on 3 positions @ depth 10)
- Working tree: clean; committed + pushed to origin/main (46 ahead of upstream)
- No match running.
- Patches on disk (rejected WIP, recoverable):
  tools/patches/lmp.patch - LMP (quiet_played > 2+depth*depth @ depth<=3, non-PV,
  non-check, non-TT): FAILED unit-test gate (mate-in-N 3143 vs 32000 @ d19).
  No SPRT attempted. Same failure mode as LMR x1.15 and futility-220.

## Decision tree (evaluate top-down each run)
A. cutechess-cli process running? -> nothing to do. Exit.
B. A NEW experiment tag exists in tools/patches/ or a WIP diff in the working
   tree? -> Gate it first (rule 5). If it fails the unit-test gate, log
   "GATE FAIL <tag>: <reason>" in tools/results.log, move its patch to
   tools/patches/<tag>.patch, restore the tree to HEAD, and exit. Do NOT
   SPRT a gate-failing variant.
C. Gate passed -> build the change binary, copy to tools/runs/bin/Dog-<tag>,
   build the base (currently tools/runs/bin/Dog-lmr065), then launch:
   CONC=2 TC=5+0.05 SPRT_MAX=1500 setsid nohup bash tools/fast_sprt.sh ab <tag> \
       tools/runs/bin/Dog-<tag> tools/runs/bin/Dog-lmr065 \
       > /tmp/opencode/<tag>-sprt.out 2>&1 &
   Verify: ps aux | grep cutechess-cli ; tail /tmp/opencode/<tag>-sprt.out
D. The newest tools/runs/ab-*.txt has a VERDICT line in tools/results.log?
   -> go to "Verdict flow" for that tag.
E. Otherwise (match died): first kill any orphaned Dog-* engine processes
   (they spin at ~60% CPU after cutechess dies). Reconstruct the pairing from
   the run txt header ("$a vs $b") and the FINGERPRINT line in results.log,
   then relaunch the same pair under a fresh tag: ab-<feature>-<timestamp>.
   If the same feature has died three times, STOP and report instead.

## Verdict flow
1. Read the verdict from tools/results.log:
   ACCEPT / KEEP (llr>0) -> KEEP the change
   REJECT / REVERT      -> DROP the change
2. KEEP path: the change must already be committed (or commit it now with a
   message "search: <feature> (SPRT ACCEPT ab-<tag>-...)").
3. DROP path: ensure the working tree matches HEAD (git restore any stray
   files), no patch file needed unless the experiment may be revisited - in
   that case save the WIP diff to tools/patches/<feature>.patch FIRST.
4. Update this file: new state paragraph (HEAD, running match + tag + md5s,
   or idle), and append one line to History.
5. Commit + push docs/log updates to origin/main.

## Next experiment candidates (from RESEARCH.md "Open questions")
1. Leaf eval caching - evalcache REJECTED twice; needs a correct caching
   scheme before retry. High effort.
2. RukChess 512-net port - done (item 8), REJECTED on desktop; blocked on a
   real RukChess net. Do not retry as-is.
3. LMR/futility/LMP calibration - exhausted; all aggressive-limit variants
   fail the mate-in-N gate. Do not retry without a different mechanism.
4. Hardware-only work (PSRAM TT, SIMD NNUE, flash net swap, threading,
   serial smoke) - S3 board required, out of board-free scope (plan phases
   0.4/1.x).

## History
(append one line per completed experiment: tag | verdict | commit)
- killer-moves | REJECT (llr -3.08) | - (baseline only)
- bignet | ACCEPT (llr 3.04) | commit 9aea532-era tree (256-hidden net)
- bignet2 | ACCEPT (llr 3.05) | - (eval net finalised)
- evalcache | REJECT (llr -2.26) | 513c133 (reverted)
- agingfix | REJECT (llr -2.26) | 513c133 (generation cycle reverted)
- delta-prune | REJECT (llr -3.01) | -
- futility (150+110d) | REJECT (llr -2.23) | ee21454
- futility (220+180d) | GATE FAIL (mate-in-N) | never SPRTed
- ttaging | ACCEPT (llr 2.95) | 56d30e5
- mdp | ACCEPT (llr 2.22) | 56d30e5
- SEE+QS ordering | ACCEPT (no standalone SPRT; part of 56d30e5) | 56d30e5
- lmr065 (table mul 0.65) | ACCEPT (llr 2.2, +34.6) | e2dda8e
- static-null 121->100 | REJECT (llr -2.23) | ee21454
- ruk512 (RukChess 768->512->1) | REJECT (llr -2.22) | 42d99a3
- LMR x1.15 | GATE FAIL (mate-in-N) | never SPRTed
- LMP | GATE FAIL (mate-in-N) | never SPRTed; patch tools/patches/lmp.patch
- bug-hunt tt guards | defensive only (no SPRT) | 331e438
