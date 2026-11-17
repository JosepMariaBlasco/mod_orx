#!/bin/sh
# checks.sh BASE URL LOG [DOC [PORT]] -- the checks of the modrexx class
# (tests/modrexx.sh says what they are), against an Apache that already
# serves tests/modrexx/*.rex and a static.txt ("hello") at BASE, with
# rexx-handler and the two RexxSetVar of tests/modrexx.sh. URL is BASE's
# path, LOG Apache's error log, DOC its DocumentRoot as Apache names it
# (/var/www/html), PORT the port of BASE (80). Exits with the number of
# failures. Run by tests/modrexx.sh and, on Windows, under Git's bash, by
# tests/modrexx.ps1.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

BASE=$1; URL=$2; LOG=$3; DOC=${4:-/var/www/html}; PORT=${5:-80}
HERE=$(cd "$(dirname "$0")/.." && pwd)
HP=; [ "$PORT" = 80 ] || HP=:$PORT          # as ap_construct_url writes it
TMP=$(mktemp -d)
# Windows (Git's bash): paths that curl.exe takes too, and arguments as
# they are (a "/x" in a header is not a path to convert).
case "$(uname -s)" in
    MINGW*|MSYS*) WINDOWS=1; TMP=$(cygpath -m "$TMP")
                  export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' ;;
esac
FAILS=0
# Only what the error log gets from here on.
LOG0=$(wc -l < "$LOG")
newlog() { tail -n +$((LOG0 + 1)) "$LOG"; }

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected '$3', got '$2')"; fi; }
# "status content-type" of a response of respond.rex, and its body.
head_of() { curl -s -o "$TMP/null" -w '%{http_code} %{content_type}' "$BASE/respond.rex?$1"; }
body_of() { curl -s "$BASE/respond.rex?$1" | tr -d '\r'; }
# An expected file, for this server.
expected() { sed -e "s#@URL@#$URL#g" -e "s#@DOC@#$DOC#g" -e "s#@PORT@#$PORT#g" -e "s#@HP@#$HP#g" "$1"; }

echo "== probe.rex: what a program sees, line by line, as under mod_rexx"
curl -s -A probe/1.0 -H 'Cookie: c1=v1; c2=a&b; c3=%41+' \
     "$BASE/probe.rex/p?a=1&name=nm&zz=Z&e=%C3%A9+x%2By&f" | tr -d '\r' > "$TMP/probe"
expected "$HERE/modrexx/probe.expected" > "$TMP/expected"
# On Windows the time zone of WWWHTTP_time is the C library's whole name
# for it, of several words.
[ -z "$WINDOWS" ] || sed -i 's/^WWWHTTP_time words=.*/WWWHTTP_time words=*/' "$TMP/expected"
check "as many lines" "$(wc -l < "$TMP/probe" | tr -d ' ')" "$(wc -l < "$TMP/expected" | tr -d ' ')"
n=0; bad=0
while IFS= read -r want; do
    n=$((n + 1))
    got=$(sed -n "${n}p" "$TMP/probe")
    case "$want" in
        # name=*: a value that depends on the machine; only the name counts.
        *=\*) [ "${got%%=*}" = "${want%=\*}" ] || { fail "line $n: '$got', expected '$want'"; bad=1; } ;;
        *)    [ "$got" = "$want" ]             || { fail "line $n: '$got', expected '$want'"; bad=1; } ;;
    esac
done < "$TMP/expected"
[ $bad = 0 ] && ok "every line as under mod_rexx ($n)"
check "the writes reach the response: the status" \
      "$(curl -s -o "$TMP/null" -w '%{http_code}' "$BASE/probe.rex")" 201
curl -s -D "$TMP/headers" -o "$TMP/body" "$BASE/probe.rex"
for h in 'X-Out: out1' 'X-Set: set1' 'X-Err: err1' 'Set-Cookie: k=v' \
         'Content-Type: text/plain; charset=x-probe' 'Content-Encoding: identity' \
         'Content-Language: ca,es'; do
    check "the writes reach the response: $h" "$(tr -d '\r' < "$TMP/headers" | grep -c -x "$h")" 1
done

echo "== respond.rex: what the return value and the output make of the response"
check "nothing returned, nothing said"       "$(head_of '')" "200 "
check "WWWSendHTTPHeader and Say" \
      "$(head_of 'ct=text/plain&say=2') $(body_of 'ct=text/plain&say=2' | tr '\n' '|')" \
      "200 text/plain line 1|line 2|"
check "no content type set: none sent"       "$(head_of 'say=1')" "200 "
check "404 replaces what was said" \
      "$(head_of 'say=2&ret=404') $(body_of 'say=2&ret=404' | grep -c '404 Not Found')" \
      "404 text/html; charset=iso-8859-1 1"
check "-1 (DECLINED): the next handler adds the file itself" \
      "$(body_of 'ct=text/plain&say=2&ret=-1' | sed -n '1,3p' | tr '\n' '|')" \
      "line 1|line 2|/* respond.rex -- how a mod_rexx program's response comes out. The query  */|"
check "-2 (DONE)"                            "$(body_of 'say=1&ret=-2')" "line 1"
for v in abc 1.5 99999; do
    check "$v counts as 0" "$(head_of "say=1&ret=$v") $(body_of "say=1&ret=$v")" "200  line 1"
done
check "301"                                  "$(head_of 'say=1&ret=301')" "301 text/html; charset=iso-8859-1"
check "200 returned is Apache's error page for a 200" \
      "$(body_of 'ret=200&say=1' | grep -c '<title>200 OK</title>')" 1
check "204"                                  "$(head_of 'say=1&ret=204')" "204 "
check "304"                                  "$(head_of 'say=1&ret=304')" "304 "
check "a syntax error: 500, and what was said is dropped" \
      "$(head_of 'say=2&err=1') $(body_of 'say=2&err=1' | grep -c '^line')" \
      "500 text/html; charset=iso-8859-1 0"
check "... and the error, with its line, in the error log" \
      "$(newlog | grep -c 'respond.rex: line 22: Error 41')" 2
check "WWWReqRecStatus 404, with a body" \
      "$(head_of 'ct=text/plain&say=1&status=404') $(body_of 'ct=text/plain&say=1&status=404')" \
      "404 text/plain line 1"
check "WWWInternal_Redirect: the other resource, without the program's output" \
      "$(head_of "say=1&redirect=$URL/static.txt") $(body_of "say=1&redirect=$URL/static.txt")" \
      "200 text/plain hello"
check "a long output goes as it comes: a later 500 cannot replace it" \
      "$(head_of 'say=3000&ret=500') $(body_of 'say=3000&ret=500' | head -1)" "200  line 1"
check "HEAD: answered without running the program, as text/html" \
      "$(curl -s -I "$BASE/respond.rex?say=1&ct=text/plain" | tr -d '\r' | grep -E '^(HTTP|Content-Type)' | tr '\n' '|')" \
      "HTTP/1.1 200 OK|Content-Type: text/html|"

echo "== checks.rex: what only mod_orx answers"
curl -s -X POST -H 'Content-Type: text/plain' -d raw "$BASE/checks.rex" | tr -d '\r' > "$TMP/checks"
expected "$HERE/modrexx/checks.expected" > "$TMP/expected"
if diff "$TMP/expected" "$TMP/checks" > "$TMP/diff"; then
    ok "every line as expected"
else
    fail "checks.rex:"; cat "$TMP/diff"
fi
check "WWWLogReason: mod_rexx's line, at its level (emerg)" \
      "$(newlog | grep -c '\[orx:emerg\].*Access to /the/file failed for 127.0.0.1. reason: the reason')" 1
check "nothing else in the error log" \
      "$(newlog | grep -v -E ':notice\]|respond.rex: line 22: Error 41|Access to /the/file' | wc -l | tr -d ' ')" 0

rm -rf "$TMP"
exit $FAILS
