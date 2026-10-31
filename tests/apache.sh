#!/bin/sh
# Check Address APACHE, the command environment of native programs, in the
# phases that run them: the providers (Require oorexx, AuthBasicProvider
# oorexx) and the logging hook (OoRexxLogHook).
#
# Run after tests/rexxhttp-suite.sh, which leaves Apache configured with
# mod_orx (any MPM). Runs as root.
#
#   tests/apache.sh
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

LAB=/var/www/html/oorexx-apache
BIN=/var/www/oorexx-apache-bin
LOG=/var/log/apache2/error.log
OUT=/tmp/oorexx-apache.out
URL=http://127.0.0.1/oorexx-apache
FAILS=0

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected '$3', got '$2')"; fi; }
get() { p=$1; shift; curl -s -m 20 -o /dev/null -w '%{http_code}' "$@" "$URL/$p"; }
# line TAG [N]: the Nth line (default: the last) the programs wrote with
# that tag, without the tag
line() { grep "^$1 " "$OUT" | if [ -n "$2" ]; then sed -n "$2p"; else tail -1; fi | cut -d' ' -f2-; }
restart() { apachectl -k stop; sleep 2; : > "$LOG"; apachectl -k start; sleep 3; }

echo "== Setup"
mkdir -p "$LAB/authz" "$LAB/authn" "$LAB/logged" "$BIN"
: > "$OUT"; chmod 666 "$OUT"
echo hello > "$LAB/authz/hello.txt"
echo hello > "$LAB/authn/hello.txt"
echo hello > "$LAB/logged/hello.txt"
htpasswd -bc "$BIN/users" alice pw >/dev/null 2>&1

# The programs write "TAG value" lines to $OUT.
cat > "$BIN/lib.rex" <<EOF
::routine put public
  call lineout '$OUT', arg(1) arg(2); call lineout '$OUT'
EOF

cat > "$BIN/authz.rex" <<'EOF'
Address APACHE
"GET PHASE";                       call put 'phase', phase
"GET URI";                         call put 'uri', uri
"GET REQUEST_METHOD";              call put 'method', request_method
"GET QUERY_STRING";                call put 'query', '['query_string']'
"GET HEADER_IN X-Test";            call put 'last', header_in.['X-TEST'] rc
call put 'plural', headers_in.['X-TEST'] (header_in. == headers_in.)
"GET HEADERS_IN user-agent INTO ua"; call put 'into', ua~word(1)~left(4) rc
"GET HEADER_IN X-None";            call put 'none', rc header_in.["X-NONE"]
x = 'old'; "GET HEADER_IN X-None INTO x"; call put 'nonevar', rc x
a = .array~new
Address APACHE "LIST HEADERS_IN" With Output Using (a)
call put 'listall', (a~items > 3) (a~hasItem('X-Test: one, two'))
"GET PREV URI";                    call put 'prev', rc
"SET NOTE seen  by authz";         "GET NOTE seen"
call put 'note', '['note.seen']' rc
"ADD NOTES seen again";            "GET NOTES seen"
call put 'notelast', notes.seen
Address APACHE "LIST NOTE seen" With Output Stem h.
call put 'list', h.0 '['h.1']' h.2
"UNSET NOTE seen";                 "GET NOTE seen"; call put 'unset', rc
"SET ENV ORX_SEEN" phase
call put 'syntax', self_test('SET URI /x')
call put 'syntax2', self_test('SET HEADER_OUT X-A 1')
call put 'syntax3', self_test('FROB URI')
call put 'syntax4', self_test('GET NOWHERE')
call put 'syntax5', self_test('SET PREV NOTE a b')
call put 'syntax6', self_test('GET URI EXTRA')
call put 'syntax7', self_test('ADD URI x')
return 1

self_test:
  signal on syntax name caught
  Address APACHE arg(1)
  return 'no condition'
caught:
  c = condition('O')
  return c~code c~message~word(1) || '|' || (c~message~pos('APACHE:') > 0)

::requires 'lib.rex'
EOF

cat > "$BIN/authn.rex" <<'EOF'
use arg user, password
Address APACHE
"GET PHASE"; "GET USER"
call put 'authn', phase '['user']' (user == '')
"SET NOTE from-authn" user
return password == 'pw'
::requires 'lib.rex'
EOF

cat > "$LAB/show.rex" <<'EOF'
say 'Content-Type: text/plain'
say ''
say value('ORX_SEEN',, 'ENVIRONMENT')
EOF

cat > "$LAB/jump.rex" <<'EOF'
say 'Location: /oorexx-apache/logged/hello.txt'
say ''
EOF

cat > "$BIN/log.rex" <<'EOF'
Address APACHE
"GET PHASE";  call put 'logphase', phase
"GET STATUS"; "GET BYTES_SENT"; call put 'final', status bytes_sent
"GET URI";    call put 'loguri', uri
"GET NEXT URI"; call put 'next', rc next_uri
"GET NEXT PREV URI"; call put 'nextprev', rc next_prev_uri
"GET NOTE from-authn"; call put 'lognote', rc note.['FROM-AUTHN']
signal on syntax name caught
"SET NOTE x y"
call put 'logset', 'no condition'
return
caught:
  call put 'logset', condition('O')~code
  return
::requires 'lib.rex'
EOF
chmod -R a+rX "$LAB" "$BIN"

cat > /etc/apache2/conf-available/oorexx-apache.conf <<EOF
<Directory "$LAB">
    Options +ExecCGI
    AddHandler oorexx-cgi .rex
    Require all granted
</Directory>
<Location /oorexx-apache/authz/>
    Require oorexx $BIN/authz.rex
</Location>
<Location /oorexx-apache/show.rex>
    Require oorexx $BIN/authz.rex
</Location>
<Location /oorexx-apache/authn/>
    AuthType Basic
    AuthName apache
    AuthBasicProvider oorexx
    OoRexxAuthnProgram $BIN/authn.rex
    OoRexxLogHook $BIN/log.rex
    Require valid-user
</Location>
<Location /oorexx-apache/jump.rex>
    OoRexxLogHook $BIN/log.rex
</Location>
EOF
a2enconf oorexx-apache >/dev/null
restart

echo "== A provider (Require oorexx): reading"
check "granted"                      "$(get 'authz/hello.txt?a=1' -H 'X-Test: one' -H 'X-Test: two')" 200
check "PHASE"                        "$(line phase 1)"   AUTHZ
check "URI"                          "$(line uri 1)"     /oorexx-apache/authz/hello.txt
check "REQUEST_METHOD"               "$(line method 1)"  GET
check "QUERY_STRING"                 "$(line query 1)"   "[a=1]"
check "a header (Apache merges repeated ones)" "$(line last 1)" "one, two 0"
check "one stem under both names"    "$(line plural 1)"  "one, two 1"
check "INTO"                         "$(line into 1)"    "curl 0"
check "no entry: RC 1, no value"     "$(line none 1)"    "1 HEADER_IN.X-NONE"
check "no entry, INTO: dropped"      "$(line nonevar 1)" "1 X"
check "LIST of a table, to an array" "$(line listall 1)" "1 1"
check "no related request: RC 1"     "$(line prev 1)"    1
get 'authz/hello.txt' >/dev/null
check "QUERY_STRING, none: empty"    "$(line query 2)"   "[]"

echo "== A provider: changing"
check "SET: the value is the rest of the line"  "$(line note 1)"     "[ by authz] 0"
check "ADD, through the plural; GET, the last"  "$(line notelast 1)" "again"
check "LIST of a key, to a stem"                "$(line list 1)"     "2 [ by authz] again"
check "UNSET"                                   "$(line unset 1)"    1
check "ENV reaches the content phase"           "$(curl -s -m 20 "$URL/show.rex")" AUTHZ

echo "== Misuse: SYNTAX 93.900 at the command"
check "a field the phase may not change"        "$(line syntax 1)"  "93.900 APACHE:|1"
check "a table the phase may not change"        "$(line syntax2 1)" "93.900 APACHE:|1"
check "an unknown verb"                         "$(line syntax3 1)" "93.900 APACHE:|1"
check "an unknown thing"                        "$(line syntax4 1)" "93.900 APACHE:|1"
check "a related request is read-only"          "$(line syntax5 1)" "93.900 APACHE:|1"
check "something after GET's thing"             "$(line syntax6 1)" "93.900 APACHE:|1"
check "ADD on a field"                          "$(line syntax7 1)" "93.900 APACHE:|1"

echo "== The authentication provider"
check "granted"                      "$(get authn/hello.txt -u alice:pw)" 200
check "PHASE, and USER (mod_auth_basic sets it)" "$(line authn)" "AUTHN [alice] 0"
check "a NOTE it set, read in LOG"   "$(grep -c '^lognote 0 alice' "$OUT")" 1

echo "== The logging hook"
check "after an internal redirect"   "$(get jump.rex)" 200
sleep 1
check "PHASE"                        "$(line logphase)" LOG
check "STATUS and BYTES_SENT: the final ones" "$(line final)" "200 6"
check "URI: the request's own"       "$(line loguri)" /oorexx-apache/jump.rex
check "NEXT"                         "$(line next)" "0 /oorexx-apache/logged/hello.txt"
check "NEXT PREV: back to itself"    "$(line nextprev)" "0 /oorexx-apache/jump.rex"
check "nothing changes in LOG"       "$(line logset)" 93.900

echo "== The error log"
check "nothing unexpected in it" \
      "$(grep -v -e ':notice\]' "$LOG" | grep -c .)" 0

echo "== Cleanup"; [ $FAILS = 0 ] || cp "$LOG" /tmp/oorexx-apache.errorlog
a2disconf oorexx-apache >/dev/null; rm -rf "$LAB" "$BIN" "$OUT"
restart
echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
