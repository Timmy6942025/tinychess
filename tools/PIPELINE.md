# MaxDogOne experiment pipeline

Auto-worker handoff. A cron job (every 10 min) spawns a headless `opencode run`
when no SPRT match is running. Follow this file exactly. Repo:
/home/timmy/chess2/engine (run every command from the repo root).

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
5. Gate before any SPRT: tools/native_check.sh must pass 12/12 unit tests.
6. Bench NPS is meaningless while any match runs (CPU contention).
7. If the git state or artifacts deviate from what this file documents, STOP:
   write a short report to /tmp/opencode/pipeline.state and append a
   "PIPELINE STOP <tag> <reason>" line to tools/results.log. Do not guess.
   (Headless: you cannot ask the user questions.)
8. Only commit search/eval changes on main after SPRT ACCEPT. Work-in-progress
   changes live as patches in tools/patches/ until accepted.

## Current state (2026-08-04 ~20:00)
- RUNNING match: ab-hist3d-20260804-193506
  A=tools/runs/bin/Dog-diff      md5 ca8a94ba3c70975d66270962a1494a87  (3D from-square history)
  B=tools/runs/bin/Dog-baseline  md5 1090d5e073c52dd034e7c3313582b524
  Live log: /tmp/opencode/hist3d-sprt.out, tools/runs/ab-hist3d-20260804-193506.txt
- Patches (apply to a clean tree, verified to apply in either order):
  tools/patches/hist3d.patch   - 3D from-square history: main.h history_size
                                 = 2*6*64*64; search.cpp history_index takes
                                 (side, from_type, from_sq, to_sq)
  tools/patches/checkext.patch - check extensions with a per-line extension
                                 budget (max 2): new `extensions` param on
                                 search(); gives_check re-search at depth,
                                 LMR skipped for checking moves
- Working tree: clean (tools/results.log showing modified is NORMAL - it is
  the live log; untracked files under tools/runs/ are NORMAL).

## Decision tree (evaluate top-down each run)
A. cutechess-cli process running? -> nothing to do. Exit.
B. The newest tools/runs/ab-*.txt has a VERDICT line in tools/results.log?
   -> go to "Verdict flow" for that tag.
C. Otherwise (match died): first kill any orphaned Dog-* engine processes
   (they spin at ~60% CPU after cutechess dies). Reconstruct the pairing from
   the run txt header ("$a vs $b") and the FINGERPRINT line in results.log,
   then relaunch the same pair under a fresh tag: ab-<feature>-<timestamp>.
   If the same feature has died three times, STOP and report instead.

## Verdict flow for ab-hist3d-* (feature "hist3d", A=Dog-diff)
1. Read the verdict from tools/results.log:
   ACCEPT / KEEP (llr>0) -> KEEP 3D history
   REJECT / REVERT      -> DROP 3D history
2. KEEP path:
   git apply tools/patches/hist3d.patch
   git add app/src/main.h app/src/search.cpp
   git commit -m "search: 3D from-square history heuristic (SPRT ACCEPT ab-hist3d-...)"
3. DROP path: nothing to apply.
4. Apply the checkext experiment and gate it:
   git apply tools/patches/checkext.patch
   tools/native_check.sh        # must pass 12/12
   cp app/src/linux-windows/build/Dog-native tools/runs/bin/Dog-checkext
5. Build the base for the checkext SPRT:
   - hist3d KEPT: base must be baseline+hist3d:
       git checkout -- app/src/search.cpp        # drop checkext only
       tools/native_check.sh                     # rebuild
       cp app/src/linux-windows/build/Dog-native tools/runs/bin/Dog-hist3d-base
       git apply tools/patches/checkext.patch    # restore checkext in the tree
   - hist3d DROPPED: base is tools/runs/bin/Dog-baseline (already correct).
6. Launch (A=change, B=base):
   CONC=2 TC=5+0.05 SPRT_MAX=1500 setsid nohup bash tools/fast_sprt.sh ab checkext \
       tools/runs/bin/Dog-checkext tools/runs/bin/Dog-hist3d-base \
       > /tmp/opencode/checkext-sprt.out 2>&1 &
   (use tools/runs/bin/Dog-baseline instead of Dog-hist3d-base if hist3d dropped)
   Verify: ps aux | grep cutechess ; tail /tmp/opencode/checkext-sprt.out
7. Update this file: new state paragraph (running match, tag, md5s of both
   binaries), and append a line to History.

## When the checkext match also concludes
Same flow. KEEP -> commit the tree as it stands (it contains hist3d+checkext
if hist3d was kept, else checkext alone):
   git add app/src/search.cpp app/src/main.h
   git commit -m "search: check extensions with per-line budget (SPRT ACCEPT ab-checkext-...)"
Then pick the next experiment from RESEARCH.md (next candidates: NNUE leaf eval
caching; LMR table retune). For each: implement it, save the WIP diff to
tools/patches/<feature>.patch BEFORE any stashing, gate with native_check.sh,
build tools/runs/bin/Dog-<feature>, SPRT it vs the current baseline build, and
never touch the running match's binaries.

## History
(append one line per completed experiment: tag | verdict | commit)
