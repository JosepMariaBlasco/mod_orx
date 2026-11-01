#!/bin/sh
# For CI: one notice with the line of every test tests/ci-run.sh ran.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0
summary=${ORX_CI_SUMMARY:-/tmp/orx-ci-summary}
[ -f "$summary" ] || { echo "::warning title=No tests ran::no summary at $summary"; exit 0; }
cat "$summary"
echo "::notice title=${1:-Summary}::$(sed -e 's/%/%25/g' "$summary" | awk '{ printf "%s%%0A", $0 }')"
