#!/bin/sh
# sprt_pipeline.sh - cron driver (every 10 min).
# While a match runs: nothing to do. When no match is running: spawn a
# headless opencode worker to continue the experiment pipeline (handle the
# verdict, build+test the next experiment, launch the next SPRT detached).
# Follows tools/PIPELINE.md. TEST_CMD overrides the worker for dry runs.
ENGINE=/home/timmy/chess2/engine
LOG=/tmp/opencode/sprt-watch.log
LOCK=/tmp/opencode/pipeline.lock
OPENCODE="${OPENCODE:-/home/timmy/.npm-global/bin/opencode}"
mkdir -p /tmp/opencode

exec 9>"$LOCK"
flock -n 9 || exit 0

if pgrep -f cutechess-cli >/dev/null 2>&1; then
    echo "[$(date '+%F %T')] match running, nothing to do" >> "$LOG"
    exit 0
fi

echo "[$(date '+%F %T')] no match running -> spawning pipeline worker" >> "$LOG"
if [ -n "$TEST_CMD" ]; then
    eval "$TEST_CMD" >> "$LOG" 2>&1
else
    "$OPENCODE" run "You are the MaxDogOne experiment pipeline worker. A SPRT match has just finished or died. Read /home/timmy/chess2/engine/tools/PIPELINE.md and follow it exactly: handle the verdict, build and gate the next experiment, launch the next SPRT fully detached, update the state files. Never build while a match is running, never start a second match, and if the state deviates from PIPELINE.md stop and report to /tmp/opencode/pipeline.state instead of guessing." >> "$LOG" 2>&1
fi
echo "[$(date '+%F %T')] worker exited" >> "$LOG"
