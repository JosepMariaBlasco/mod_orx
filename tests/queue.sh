#!/bin/sh
# Check OoRexxQueueTimeout and OoRexxLogQueueTimeout: with every host process
# busy, a phase waits that long for one and then gives up -- 503 for content
# after OoRexxQueueTimeout, a skipped logging hook after OoRexxLogQueueTimeout
# (2 s unless set) -- and the host does not run what the module gave up on.
#
# Run after tests/rexxhttp-suite.sh, which leaves Apache configured with
# mod_orx (any MPM). Runs as root. Restores the configuration it found.
#
#   tests/queue.sh
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

LAB=/var/www/html/oorexx-queue
BIN=/var/www/oorexx-queue-bin
LOG=/var/log/apache2/error.log
CONF=/etc/apache2/mods-available/orx.conf
OUT=/tmp/oorexx-queue.out
URL=http://127.0.0.1/oorexx-queue
FAILS=0

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected '$3', got '$2')"; fi; }
get() { curl -s -m 30 -o /dev/null -w '%{http_code}' "$URL/$1"; }
restart() { apachectl -k stop; sleep 2; : > "$LOG"; apachectl -k start; sleep 3; }

echo "== Setup"
mkdir -p "$LAB/logged" "$LAB/logged1" "$BIN"
cp "$CONF" "$BIN/orx.conf.saved"
: > "$OUT"; chmod 666 "$OUT"
cat > "$LAB/slow.rex" <<'EOF'
say 'Content-Type: text/plain'
say ''
call SysSleep 8
say 'slow'
EOF
cat > "$LAB/fast.rex" <<EOF
call lineout '$OUT', 'fast ran'; call lineout '$OUT'
say 'Content-Type: text/plain'
say ''
say 'fast'
EOF
echo hello > "$LAB/logged/hello.txt"
echo hello > "$LAB/logged1/hello.txt"
cat > "$BIN/log.rex" <<EOF
call lineout '$OUT', 'log ran'; call lineout '$OUT'
EOF
chmod -R a+rX "$LAB" "$BIN"
cat > /etc/apache2/conf-available/oorexx-queue.conf <<EOF
<Directory "$LAB">
    Options +ExecCGI
    AddHandler oorexx-cgi .rex
    Require all granted
    OoRexxQueueTimeout 3
</Directory>
<Directory "$LAB/logged">
    OoRexxLogHook $BIN/log.rex
</Directory>
<Directory "$LAB/logged1">
    OoRexxLogHook $BIN/log.rex
    OoRexxLogQueueTimeout 1
</Directory>
EOF
a2enconf oorexx-queue >/dev/null
# One process, never more: a slow phase keeps it busy.
grep -v -E '^OoRexxProcesses' "$BIN/orx.conf.saved" > "$CONF"
echo "OoRexxProcesses 1 1" >> "$CONF"
restart

echo "== A free process: no wait"
check "fast.rex answers"       "$(get fast.rex)" 200

echo "== Every process busy"
get slow.rex > /dev/null & sleep 1
T0=$(date +%s)
check "fast.rex gets a 503"    "$(get fast.rex)" 503
T1=$(date +%s)
check "after about OoRexxQueueTimeout (3 s)" \
      "$([ $((T1 - T0)) -ge 2 ] && [ $((T1 - T0)) -le 4 ] && echo yes)" yes
check "the reason, in the log" \
      "$(grep -c 'no oorexx-host process free within 3 s (OoRexxQueueTimeout)' "$LOG")" 1
T0=$(date +%s)
check "a static file is served" "$(get logged/hello.txt)" 200
T1=$(date +%s)
check "and the client does not wait for its logging hook" \
      "$([ $((T1 - T0)) -le 1 ] && echo yes)" yes
check "here too"               "$(get logged1/hello.txt)" 200
sleep 3
check "the logging hook gave up after 2 s (the default), and said so" \
      "$(grep -c 'no oorexx-host process free within 2 s (OoRexxLogQueueTimeout)' "$LOG")" 1
check "and after 1 s where OoRexxLogQueueTimeout says so" \
      "$(grep -c 'no oorexx-host process free within 1 s (OoRexxLogQueueTimeout)' "$LOG")" 1
check "before the busy process was free"     "$(grep -c 'log ran' "$OUT")" 0
wait
sleep 1
check "what the module gave up on never ran" "$(grep -c 'fast ran' "$OUT")" 1
check "nor did the skipped logging hooks"    "$(grep -c 'log ran' "$OUT")" 0

echo "== Free again"
check "fast.rex answers"       "$(get fast.rex)" 200
check "the logging hook runs"  "$(get logged/hello.txt)" 200
sleep 1
check "and logs"               "$(grep -c 'log ran' "$OUT")" 1

echo "== A bad value: refused when the configuration is read"
cp "$CONF" "$BIN/orx.conf.good"
echo "OoRexxQueueTimeout 0" >> "$CONF"
check "apachectl configtest names it" \
      "$(apachectl configtest 2>&1 | grep -c 'OoRexxQueueTimeout must be a positive number')" 1
cp "$BIN/orx.conf.good" "$CONF"
echo "OoRexxLogQueueTimeout x" >> "$CONF"
check "and OoRexxLogQueueTimeout too" \
      "$(apachectl configtest 2>&1 | grep -c 'OoRexxLogQueueTimeout must be a positive number')" 1

echo "== Cleanup"
cp "$BIN/orx.conf.saved" "$CONF"
a2disconf oorexx-queue >/dev/null; rm -rf "$LAB" "$BIN" "$OUT"
restart
check "the configuration found is back" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1/test/hola)" 200
echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
