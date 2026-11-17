#!/bin/sh
# Check the modrexx class (handler rexx-handler): programs written for
# mod_rexx run unchanged. The expected answers were measured under mod_rexx
# 2.4.0 itself, built from its SourceForge repository, on Apache 2.4.58 and
# ooRexx 5.3.0, with the same programs (tests/modrexx/*.rex):
#
#   probe.rex     every initial variable and WWW* function, every write
#                 and what a read sees afterwards (probe.expected)
#   respond.rex   status, headers and body for each kind of return value,
#                 a syntax error, WWWReqRecStatus, WWWInternal_Redirect
#   checks.rex    what only mod_orx answers (checks.expected): bad handles,
#                 functions not there yet, a POST that is not urlencoded
#
# The checks themselves are a Rexx program, tests/modrexx/run-checks.rex,
# which tests/modrexx.ps1 runs on Windows too.
#
# Run after tests/rexxhttp-suite.sh, which leaves Apache configured with
# mod_orx (any MPM), and after make install-modrexx: it loads the mod_rexx
# layer for the run (before mod_orx with ORX_REXX_FIRST=1) and takes it out
# again. Runs as root. Leaves nothing behind.
#
#   tests/modrexx.sh
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/modrexx/layer.sh"
LAB=/var/www/html/modrexx
CONF=/etc/apache2/conf-available/modrexx.conf
LOG=/var/log/apache2/error.log
URL=/modrexx
BASE=http://127.0.0.1$URL

echo "== Setup"
mkdir -p "$LAB"
for p in probe respond checks; do cp "$HERE/modrexx/$p.rex" "$LAB/"; done
echo hello > "$LAB/static.txt"
chmod -R a+rX "$LAB"
{
    echo "<Directory \"$LAB\">"
    echo "    AddHandler rexx-handler .rex"
    echo "    RexxSetVar ExampleVar \"Hello world\""
    echo "    RexxSetVar EXAMPLEVAR2 \"Second one\""
    echo "    Require all granted"
    echo "</Directory>"
} > "$CONF"
layer_on
a2enconf modrexx >/dev/null
apachectl -k graceful; sleep 2
: > "$LOG"

rexx "$HERE/modrexx/run-checks.rex" "$BASE" "$URL" "$LOG"
FAILS=$?

echo "== Cleanup"
a2disconf modrexx >/dev/null
layer_off
rm -f "$CONF"
rm -rf "$LAB"
apachectl -k graceful; sleep 1

echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
