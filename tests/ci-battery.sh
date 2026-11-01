#!/bin/sh
# For CI (Debian/Ubuntu, as root): every test, with the MPM Apache has now,
# each through tests/ci-run.sh. Continues after a failure; fails if any did.
#
#   tests/ci-battery.sh LABEL /path/to/rexxhttp
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

L=$1; RH=$2
cd "$(dirname "$0")/.." || exit 1
R=tests/ci-run.sh
F=0
run() { "$R" "$@" || F=$((F + 1)); }
leftovers() { rm -f /etc/apache2/conf-enabled/oorexx-*.conf; }

run "$L suite"          tests/rexxhttp-suite.sh "$RH"
run "$L phases"         tests/phases.sh
run "$L preload"        tests/preload.sh
run "$L modrexx"        tests/modrexx.sh
run "$L modrexx (first)" env ORX_REXX_FIRST=1 tests/modrexx.sh
run "$L modrexx-phases" tests/modrexx-phases.sh
run "$L modrexx-phases (first)" env ORX_REXX_FIRST=1 tests/modrexx-phases.sh
run "$L queue"          tests/queue.sh
run "$L apache"         tests/apache.sh
run "$L native-phases"  tests/native-phases.sh
run "$L lifecycle"      tests/lifecycle.sh;    leftovers
run "$L restart"        tests/restart.sh
run "$L standalone"     tests/standalone.sh "$L"
exit $F
