#!/bin/sh
# The master's wait after a child dies while starting (oorexx-host, Unix).
# No Apache: the host runs standalone, with a preloaded package that kills
# its own process while a marker file exists, so every child dies while it
# loads its anchor (not a configuration error: that stops the host).
#
#   - Children that die while starting are not started again at once: the
#     master waits 1 s, then 2, 4, 8... (up to 60), one child at a time.
#     Without the wait it started thousands of children in a few seconds
#     (measured: 3521 in 8 s, with --processes 2).
#   - Each such death is logged, with the wait that follows.
#   - Once children can start again (the marker goes), the pool fills.
#
# Needs oorexx-host built or installed and ooRexx. Leaves nothing behind.
#
#   tests/start-hold.sh
#
# OOREXX_HOST (default: /usr/local/bin/oorexx-host) is the host.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

HOSTBIN=${OOREXX_HOST:-/usr/local/bin/oorexx-host}
FAILS=0
ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
children_of() { ps -eo pid=,ppid= | awk -v p="$1" '$2 == p { print $1 }' | wc -l | tr -d ' '; }

T=$(mktemp -d "${TMPDIR:-/tmp}/start-hold.XXXXXX") || exit 1
trap 'kill -9 $P 2>/dev/null; rm -rf "$T"' EXIT
cat > "$T/flaky.rex" <<EOF
if SysFileExists('$T/broken') then Address SYSTEM 'kill -9' SysQueryProcess('PID')
EOF

echo "== Children that die while starting"
touch "$T/broken"
"$HOSTBIN" --socket "$T/s.sock" --processes 3 --preload "$T/flaky.rex" 2> "$T/err" &
P=$!
sleep 6
DEATHS=$(grep -c 'died while starting' "$T/err")
# 3 at once (one failed start), then one at a time, after 1 s and 2 more: 5.
# Allow some slack for a slow machine; without the wait it is thousands.
if [ "$DEATHS" -ge 3 ] && [ "$DEATHS" -le 8 ]; then
    ok "$DEATHS children died while starting in 6 s, not one after another"
else
    fail "$DEATHS children died while starting in 6 s (expected 3 to 8)"
fi
WAITS=$(sed -n 's/.*next start in \([0-9.]*\) s$/\1/p' "$T/err" | sort -un | tr '\n' ' ')
case "$WAITS" in
    "1.0 2.0 4.0 "*) ok "each death logged, the waits growing: $WAITS" ;;
    *) fail "the logged waits: '$WAITS' (expected 1.0 2.0 4.0 ...)" ;;
esac

echo "== Children can start again"
rm -f "$T/broken"
# The current wait (at most 8 s here) ends, a child starts and is ready,
# and then the pool fills.
n=0; i=0
while [ $i -lt 20 ]; do
    n=$(children_of $P)
    [ "$n" -ge 3 ] && break
    sleep 1; i=$((i + 1))
done
if [ "$n" -ge 3 ]; then ok "the pool filled again, $n children, after $i s"
else fail "the pool did not fill again ($n children after $i s)"; fi
D2=$(grep -c 'died while starting' "$T/err")
sleep 2
if [ "$(grep -c 'died while starting' "$T/err")" = "$D2" ] && [ "$(children_of $P)" -ge 3 ]; then
    ok "and stays full"
else
    fail "children kept dying, or the pool shrank"
fi

kill -TERM $P; wait $P 2>/dev/null
[ $FAILS -eq 0 ] && echo "start-hold: all ok" || echo "start-hold: $FAILS failure(s)"
exit $FAILS
