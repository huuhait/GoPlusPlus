#!/usr/bin/env bash
# Runs the parser/formatter fuzzer (P02.10). See tests/fuzz/README.md.
#
#   tests/fuzz/run-fuzz.sh smoke             # 16 workers x 4 min (~1 CPU-hour)
#   tests/fuzz/run-fuzz.sh full              # 24 workers x 1 hour (24 CPU-hours)
#   tests/fuzz/run-fuzz.sh fork JOBS SECONDS # -fork=JOBS for SECONDS (wall)
#   tests/fuzz/run-fuzz.sh custom WORKERS SECONDS
#
# CPU time is workers (or fork jobs) x seconds: the 24 CPU-hours of P02.10 are
# `full`, or e.g. `fork 96 900` on a big machine.
# Environment: BUILD_DIR (default build-fuzz), WORK (default
# $BUILD_DIR/fuzz-work: the growing corpus and the crash-* files),
# MAX_WORKERS (default 32: leave the rest of a shared machine to others).
set -eu
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build=${BUILD_DIR:-$root/build-fuzz}
work=${WORK:-$build/fuzz-work}
max=${MAX_WORKERS:-32}
mode=${1:-smoke}
case "$mode" in
  smoke) workers=16; seconds=240 ;;
  full) workers=24; seconds=3600 ;;
  fork | custom) workers=$2; seconds=$3 ;;
  *) echo "usage: $0 smoke|full|fork JOBS SECONDS|custom WORKERS SECONDS" >&2; exit 2 ;;
esac
if [ "$workers" -gt "$max" ]; then
  echo "error: $workers workers is more than MAX_WORKERS=$max" >&2
  exit 2
fi
if [ ! -x "$build/bin/gopp-fuzz-parse" ]; then
  echo "error: $build/bin/gopp-fuzz-parse is missing; build it first:" >&2
  echo "  cmake -S . -B build-fuzz -G Ninja -DGOPP_BUILD_FUZZERS=ON" >&2
  echo "  ninja -C build-fuzz gopp-fuzz-parse" >&2
  exit 2
fi
mkdir -p "$work/corpus"
cp -n "$root"/tests/fuzz/corpus/* "$root"/tests/fuzz/regressions/* "$work/corpus/" 2>/dev/null || true
cd "$work"
# -max_len keeps inputs small (the parser is linear anyway); -timeout catches
# hangs (the seed files parse in milliseconds); the dictionary holds the
# keywords, operators and Go++ forms.
common=(-max_len=8192 -timeout=10 -rss_limit_mb=2048 -print_final_stats=1
  -dict="$root/tests/fuzz/gopp.dict" -artifact_prefix="$work/")
if [ "$mode" = fork ]; then
  # One parent, JOBS children that share the corpus; a crash is saved and the
  # run goes on (triage afterwards).
  exec "$build/bin/gopp-fuzz-parse" corpus -fork="$workers" \
    -ignore_crashes=1 -ignore_timeouts=1 -ignore_ooms=1 \
    -max_total_time="$seconds" "${common[@]}"
fi
# -jobs/-workers: that many processes share the corpus directory; each stops
# after $seconds.
exec "$build/bin/gopp-fuzz-parse" corpus \
  -jobs="$workers" -workers="$workers" -max_total_time="$seconds" "${common[@]}"
