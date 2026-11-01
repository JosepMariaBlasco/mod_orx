#!/bin/sh
# Check how Apache manages oorexx-host: restarts with requests in flight,
# an upgraded module, crashes of each process, growth of the pool, and stop.
#
# Run after tests/rexxhttp-suite.sh, which leaves Apache configured with
# mod_orx (any MPM). Needs the module's source tree (it rebuilds the
# module to check an upgrade) and runs as root.
#
#   tests/lifecycle.sh
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

LAB=/var/www/html/oorexx-lifecycle
LOG=/var/log/apache2/error.log
MARK=/tmp/oorexx-lifecycle.mark
URL=http://127.0.0.1/oorexx-lifecycle
cd "$(dirname "$0")/.." || exit 1
FAILS=0

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected '$3', got '$2')"; fi; }
parent() { cat /var/run/apache2/apache2.pid; }
keeper() { ps -eo pid,ppid,args | awk -v a="$(parent)" '$2==a && /oorexx-host --listen/ {print $1}'; }
master() { ps -eo pid,ppid | awk -v k="$(keeper)" '$2==k {print $1}'; }
children() { ps -eo ppid | awk -v m="$(master)" '$1==m' | wc -l; }
get() { curl -s -m 20 -o /dev/null -w '%{http_code}' "$URL/$1"; }
segfaults() { dmesg 2>/dev/null | grep -c 'apache2.*segfault'; }
noise() { grep -v -E 'AH00(094|163|169|171|173|489|492|493|494)' "$LOG" | wc -l; }

echo "== Setup"
mkdir -p "$LAB"
cat > "$LAB/slow.rex" <<'EOF'
parse value value('QUERY_STRING',,'ENVIRONMENT') with 's=' secs .
if \datatype(secs, 'W') then secs = 0
say 'Content-Type: text/plain'
say ''
call SysSleep secs
say 'slept' secs
EOF
cat > "$LAB/mark.rex" <<EOF
say 'Content-Type: text/plain'
say ''
call SysSleep 3
call lineout '$MARK', 'finished'
call lineout '$MARK'
say 'done'
EOF
cat > /etc/apache2/conf-available/oorexx-lifecycle.conf <<EOF
Alias /oorexx-lifecycle/ $LAB/
<Directory "$LAB">
    Options +ExecCGI
    AddHandler oorexx-cgi .rex
    Require all granted
</Directory>
EOF
a2enconf oorexx-lifecycle >/dev/null
apachectl -k restart; sleep 2
: > "$LOG"
P=$(parent); S0=$(segfaults)
check "a request is served" "$(get slow.rex)" 200

echo "== Graceful restart with requests in flight"
for i in 1 2 3; do get 'slow.rex?s=3' > /tmp/lc.$i & done
sleep 1; apachectl -k graceful; sleep 0.5
check "a new request during the drain" "$(get slow.rex)" 200
wait
check "the requests in flight finish" "$(cat /tmp/lc.1 /tmp/lc.2 /tmp/lc.3)" 200200200
sleep 1
check "the old host is gone after the drain" \
      "$(ps -eo ppid,stat,args | awk '$1==1 && $2!~/Z/ && /oorexx-host --listen/' | wc -l)" 0

echo "== Upgraded module, then graceful and hard restarts"
for k in graceful restart; do
    rm -rf module/.libs module/*.lo module/*.slo
    if [ $k = graceful ]; then X=-Wc,-O0; else X=; fi
    apxs -c -Wc,-Wall $X module/mod_orx.c >/dev/null 2>&1 &&
        apxs -i -n orx module/.libs/mod_orx.so >/dev/null 2>&1
    get 'slow.rex?s=2' > /tmp/lc.1 &
    sleep 0.5; apachectl -k $k; wait; sleep 3
    check "Apache's parent survives a $k after an upgrade" "$(kill -0 "$P" 2>/dev/null && echo alive)" alive
    check "served after the $k" "$(get slow.rex)" 200
done
make >/dev/null 2>&1 && make install >/dev/null 2>&1
apachectl -k graceful; sleep 2

echo "== Crashes"
: > "$LOG"
kill -9 "$(ps -eo pid,ppid | awk -v m="$(master)" '$2==m {print $1}' | head -1)"; sleep 1
check "a host child killed: replaced" "$(get slow.rex)" 200
check "  ...silently" "$(noise)" 0
kill -9 "$(master)"; sleep 2
check "the host's master killed: restarted" "$(get slow.rex)" 200
kill -9 "$(keeper)"; sleep 2
check "the host's keeper killed: restarted" "$(get slow.rex)" 200
check "  ...one log line each" "$(grep -c 'mod_orx: oorexx-host .*restarting it' "$LOG")" 2

echo "== The pool grows and stays within its maximum"
MAX=$(ps -eo args | awk '/[o]orexx-host --listen/ {for (i=1;i<NF;i++) if ($i=="--max-processes") print $(i+1); exit}')
for i in $(seq 1 $((MAX + 2))); do get 'slow.rex?s=6' > /tmp/lc.g$i & done
PEAK=0; SEEN=
for t in $(seq 1 30); do        # the peak over 6 seconds, sampled
    N=$(children); [ "$N" -gt "$PEAK" ] && PEAK=$N; SEEN="$SEEN $N"; sleep 0.2
done
wait
check "grown to the maximum ($MAX), never beyond" "$PEAK" "$MAX"
[ "$PEAK" = "$MAX" ] || echo "        FAIL detail: children every 0.2 s:$SEEN"
check "every request answered" "$(cat /tmp/lc.g* | tr -d '\n' | sed 's/200//g')" ""
rm -f /tmp/lc.*

echo "== Stop with a request in flight"
: > "$MARK"; chmod 666 "$MARK"
get mark.rex > /dev/null &
sleep 1; apachectl -k stop; wait; sleep 3
check "the program in flight finished" "$(cat "$MARK")" finished
check "no host left" "$(ps -eo stat,args | grep '[o]orexx-host' | grep -vc '^Z')" 0
check "the socket is gone" "$(ls /var/run/apache2/oorexx.sock 2>/dev/null)" ""
check "no segfault of Apache in the whole run" "$(( $(segfaults) - S0 ))" 0

a2disconf oorexx-lifecycle >/dev/null; rm -rf "$LAB" "$MARK"
apachectl -k start; sleep 1
echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
