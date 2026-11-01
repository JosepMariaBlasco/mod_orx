#!/bin/sh
# Check the wait between restarts of oorexx-host: a host that dies without
# having run for 60 s is restarted after 1 s, then 2, 4... (up to 60), not
# every second; a stop of Apache during a wait leaves nothing behind; and a
# host that had run for 60 s is restarted at once.
#
# Run after tests/rexxhttp-suite.sh, which leaves Apache configured with
# mod_orx (any MPM). Runs as root. Restores the configuration it found.
# Takes about two minutes (it waits for a host to be stable).
#
#   tests/restart.sh
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

BIN=/var/www/oorexx-restart-bin
LOG=/var/log/apache2/error.log
CONF=/etc/apache2/mods-available/orx.conf
STARTS=/tmp/oorexx-restart.starts
FAILS=0

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected '$3', got '$2')"; fi; }
parent() { cat /var/run/apache2/apache2.pid; }
keeper() { ps -eo pid,ppid,args | awk -v a="$(parent)" '$2==a && /oorexx-host --listen/ {print $1}'; }
starts() { wc -l < "$STARTS"; }
restart() { apachectl -k stop; sleep 2; : > "$LOG"; apachectl -k start; }

echo "== Setup"
mkdir -p "$BIN"
cp "$CONF" "$BIN/orx.conf.saved"
: > "$STARTS"; chmod 666 "$STARTS"
# A host that dies as soon as it starts, and says when it started.
cat > "$BIN/dies.sh" <<EOF
#!/bin/sh
date +%s.%N >> $STARTS
exit 1
EOF
chmod 755 "$BIN/dies.sh"
grep -v -E '^OoRexxHost' "$BIN/orx.conf.saved" > "$CONF"
echo "OoRexxHost $BIN/dies.sh" >> "$CONF"
restart

echo "== A host that cannot stay up"
sleep 17
N=$(starts)
check "started 4 or 5 times in 17 s, not every second" \
      "$([ "$N" -ge 4 ] && [ "$N" -le 5 ] && echo yes)" yes
check "each wait longer than the one before" \
      "$(awk 'NR > 1 { g = $1 - p; if (NR > 2 && g <= q) bad = 1; q = g } { p = $1 }
              END { print bad ? "no" : "yes" }' "$STARTS")" yes
check "the log says how long: 1 s" "$(grep -c 'restarting it in 1 s' "$LOG")" 1
check "  then 2 s"                 "$(grep -c 'restarting it in 2 s' "$LOG")" 1
check "  then 4 s"                 "$(grep -c 'restarting it in 4 s' "$LOG")" 1

echo "== Apache stops during a wait"
apachectl -k stop; sleep 3
N=$(starts)
check "nothing of Apache is left" "$(pgrep -c apache2)" 0
sleep 10
check "and nothing starts afterwards" "$(starts)" "$N"

echo "== A stable host"
cp "$BIN/orx.conf.saved" "$CONF"
restart; sleep 2
check "the real host serves" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1/test/hola)" 200
sleep 60
kill -9 "$(keeper)"; sleep 2
check "after 60 s up, restarted at once" "$(grep -c 'restarting it$' "$LOG")" 1
check "  and serves" "$(curl -s -m 20 -o /dev/null -w '%{http_code}' http://127.0.0.1/test/hola)" 200
kill -9 "$(keeper)"; sleep 3
check "dying again at once, after 1 s" "$(grep -c 'restarting it in 1 s' "$LOG")" 1
check "  and serves" "$(curl -s -m 20 -o /dev/null -w '%{http_code}' http://127.0.0.1/test/hola)" 200

echo "== Cleanup"
rm -rf "$BIN" "$STARTS"
restart; sleep 2
check "the configuration found is back" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1/test/hola)" 200
echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
