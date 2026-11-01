#!/bin/sh
# For CI: run one test, with its output folded in the log, and report it as
# annotations, which can be read without the log (through the checks API):
# on failure an error with its FAIL lines; and, always, a line in the
# summary that tests/ci-summary.sh turns into one notice at the end.
#
#   tests/ci-run.sh NAME command [arguments...]
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

name=$1; shift
summary=${ORX_CI_SUMMARY:-/tmp/orx-ci-summary}
out=$(mktemp /tmp/orx-ci.XXXXXX)
start=$(date +%s)
echo "::group::$name"
"$@" > "$out" 2>&1; rc=$?
cat "$out"
echo "::endgroup::"
secs=$(($(date +%s) - start))
# Annotation text: %, CR and LF escaped, at most 40 lines.
esc() { sed -e 's/%/%25/g' -e 's/\r/%0D/g' | head -40 | awk '{ printf "%s%%0A", $0 }'; }
if [ $rc = 0 ]; then
    echo "$name: ok (${secs} s) $(tail -1 "$out")" >> "$summary"
else
    fails=$(grep -E 'FAIL|Failures:|Errors:|error\.log|^  log: |^  diag: |FAIL detail' "$out")
    [ -n "$fails" ] || fails=$(tail -20 "$out")
    echo "::error title=$name failed (rc $rc)::$(echo "$fails" | esc)"
    echo "$name: FAILED rc=$rc (${secs} s)" >> "$summary"
fi
rm -f "$out"
exit $rc
