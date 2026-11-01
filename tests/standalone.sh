#!/bin/sh
# A small check that runs anywhere mod_orx builds (Linux, macOS...): an
# Apache of its own, with a minimal configuration in a temporary directory,
# serves a Rexx CGI through mod_orx; then its parent is killed (kill -9),
# with a program still running, and every process of the host must end on
# its own, after letting that program finish.
#
# Needs mod_orx and oorexx-host installed (make install), curl, and either
# root (Apache then runs as nobody) or nothing (Apache runs as you, on a
# high port). Leaves nothing behind.
#
#   tests/standalone.sh [event|prefork]    the MPM (default: event)
#
# APXS (default: apxs) finds Apache; OOREXX_HOST (default:
# /usr/local/bin/oorexx-host), the host.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

MPM=${1:-event}
APXS=${APXS:-apxs}
HOSTBIN=${OOREXX_HOST:-/usr/local/bin/oorexx-host}
PORT=8094
URL=http://127.0.0.1:$PORT
FAILS=0

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected '$3', got '$2')"; fi; }
children_of() { ps -eo pid=,ppid= | awk -v p="$1" '$2 == p { print $1 }'; }
# How many of these processes still run (a zombie, already dead, does not).
alive() {
    n=0
    for p in $1; do
        s=$(ps -o stat= -p "$p" 2>/dev/null)
        [ -n "$s" ] && case "$s" in Z*) ;; *) n=$((n + 1)) ;; esac
    done
    echo $n
}

HTTPD=$($APXS -q SBINDIR)/$($APXS -q TARGET)
MODS=$($APXS -q LIBEXECDIR)
T=$(mktemp -d /tmp/orx-standalone.XXXXXX)
chmod 755 "$T"
mkdir "$T/htdocs"

echo "== Setup ($HTTPD, $MPM)"
STATIC=$("$HTTPD" -l)
load() {   # LoadModule, unless it is built in
    echo "$STATIC" | grep -q "mod_$1\.c" || echo "LoadModule $1_module $MODS/mod_$1.so"
}
{
    load "mpm_$MPM"
    load authz_core
    load unixd
    echo "LoadModule orx_module $MODS/mod_orx.so"
    if [ "$(id -u)" = 0 ]; then
        echo "User nobody"
        echo "Group $(id -gn nobody)"
    fi
    cat <<EOF
ServerName localhost
Listen 127.0.0.1:$PORT
ServerRoot $T
DefaultRuntimeDir $T
PidFile $T/httpd.pid
ErrorLog $T/error.log
LogLevel info
DocumentRoot $T/htdocs
OoRexxHost $HOSTBIN
OoRexxProcesses 2 4
<Directory "$T/htdocs">
    Options +ExecCGI
    Require all granted
    <Files "*.rex">
        SetHandler oorexx-cgi
    </Files>
</Directory>
EOF
} > "$T/httpd.conf"
cat > "$T/htdocs/hello.rex" <<'EOF'
say 'Content-Type: text/plain'
say ''
say 'hello'
EOF
cat > "$T/htdocs/slow.rex" <<EOF
say 'Content-Type: text/plain'
say ''
call SysSleep 3
call lineout '$T/mark', 'finished'
call lineout '$T/mark'
say 'done'
EOF
: > "$T/mark"; chmod 666 "$T/mark"
chmod -R a+rX "$T"
if ! "$HTTPD" -f "$T/httpd.conf" -k start; then
    echo "  FAIL  Apache does not start"; cat "$T/error.log" 2>/dev/null
    rm -rf "$T"; exit 1
fi
sleep 3

echo "== A Rexx CGI through mod_orx"
check "it is served" "$(curl -s -m 20 "$URL/hello.rex")" hello

echo "== Apache's parent dies (kill -9), with a program running"
P=$(cat "$T/httpd.pid")
K=$(for c in $(children_of "$P"); do ps -o args= -p "$c" | grep -q oorexx-host && echo "$c"; done)
M=$(children_of "$K")
H="$K $M $(for m in $M; do children_of "$m"; done)"
check "the host's processes are found (keeper, master, children)" \
      "$([ "$(echo $H | wc -w)" -ge 4 ] && echo yes)" yes
curl -s -m 20 "$URL/slow.rex" > /dev/null &
sleep 1
kill -9 "$P"
T0=$(date +%s)
while [ "$(alive "$H")" != 0 ] && [ $(($(date +%s) - T0)) -lt 15 ]; do sleep 0.2; done
check "every process of the host ends on its own" "$(alive "$H")" 0
echo "        (in $(($(date +%s) - T0)) s)"
if [ "$(alive "$H")" != 0 ]; then
    echo "        FAIL detail: Apache's parent was $P; keeper $K, master $M"
    for p in $H; do
        ps -o pid=,ppid=,stat=,etime=,args= -p "$p" 2>/dev/null | sed 's/^/        FAIL detail: /'
    done
    grep -E 'orx|oorexx' "$T/error.log" | tail -8 | sed 's/^/        FAIL detail: log: /'
fi
check "after the program running finished" "$(cat "$T/mark")" finished
wait

echo "== Cleanup"
ps -eo pid=,args= > "$T/ps"
for p in $(grep -F "$T/httpd.conf" "$T/ps" | awk '{ print $1 }'); do
    kill -9 "$p" 2>/dev/null
done
sleep 1
ps -eo args= > "$T/ps"
check "nothing of this Apache is left" "$(grep -c -F "$T/httpd.conf" "$T/ps" | tr -d ' ')" 0
grep -E '\[(orx|core|mpm_[a-z]+):(error|crit|emerg)' "$T/error.log" | sed 's/^/        /'
rm -rf "$T"
echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
