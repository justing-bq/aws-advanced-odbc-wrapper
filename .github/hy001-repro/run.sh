#!/usr/bin/env bash
# TEMPORARY, do not merge. Runs hy001_repro and judges the outcome.
# Usage: run.sh <expect bug|clean> <hy001_repro args...>
#   bug    passes if the race shows in any form: HY001, a hang or a crash
#   clean  passes only if every connect was refused normally
set -u
expect="$1"
shift

ulimit -c unlimited
rm -f /tmp/cores/core.*

./hy001_repro "$@"
rc=$?
case "$rc" in
    0) outcome=clean ;;
    1) outcome=hy001 ;;
    134) outcome="abort (hang watchdog or assertion)" ;;
    139) outcome="crash (SIGSEGV)" ;;
    *) outcome="crash (exit $rc)" ;;
esac
echo "OUTCOME: $outcome"

core="$(ls -t /tmp/cores/core.* 2>/dev/null | head -n1)"
if [ -n "$core" ]; then
    echo "Backtrace of all threads from $core:"
    gdb -q -batch -ex "info sharedlibrary" -ex "thread apply all bt 30" ./hy001_repro "$core" 2>&1 \
        | grep -vE '^\[New LWP|^warning: (Can.t|Unable|Could not)'
fi

if [ "$expect" = clean ]; then
    [ "$outcome" = clean ] && echo "PASS: clean as expected" && exit 0
    echo "FAIL: expected clean, got $outcome"
    exit 1
fi
[ "$outcome" != clean ] && echo "PASS: the bug reproduced as $outcome" && exit 0
echo "FAIL: expected the bug to reproduce, but every connect was clean"
exit 1
