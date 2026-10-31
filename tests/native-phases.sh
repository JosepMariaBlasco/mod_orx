#!/bin/sh
# Check the hooks of native programs, OoRexx<Phase>Hook, in every phase:
# when each one runs, what its return value does, what it may change, the
# checks of the module, subrequests and internal redirects, LOG, and the
# directives' contexts.
#
# One program, phase.rex, is configured in every phase with the phase's
# name as its argument; it logs that it ran, and does what the request
# header X-Act asks of it ("Phase.key=value ...").
#
# Run after tests/rexxhttp-suite.sh, which leaves Apache configured with
# mod_orx (any MPM). Runs as root; listens on port 8092 for the run. Leaves
# nothing behind.
#
#   tests/native-phases.sh
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

LAB=/var/www/oorexx-native
DOC=$LAB/doc
PROG=$LAB/prog
OUT=$LAB/phases.log
LOG=/var/log/apache2/error.log
CONF=/etc/apache2/conf-available/oorexx-native.conf
URL=http://127.0.0.1:8092
FAILS=0

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected '$3', got '$2')"; fi; }
# req PATH [ACT [curl options]]: the status in $CODE; the body in $BODY, the headers
# in $HDRS, what the phases logged in $RAN (one line), new error log lines in
# $NEWLOG
req() {
    p=$1; act=$2; shift; [ $# -gt 0 ] && shift
    : > "$OUT"; before=$(wc -l < "$LOG")
    code=$(curl -s -m 20 -D /tmp/oorexx-native.h -o /tmp/oorexx-native.b \
           -w '%{http_code}' -H "X-Act: $act" "$@" "$URL$p")
    sleep 0.3
    BODY=$(cat /tmp/oorexx-native.b); HDRS=$(tr -d '\r' < /tmp/oorexx-native.h)
    RAN=$(cat "$OUT" | tr '\n' ' ' | sed 's/ $//')
    NEWLOG=$(tail -n +$((before + 1)) "$LOG")
    CODE=$code
}
header() { echo "$HDRS" | grep -i "^$1:" | cut -d' ' -f2-; }
logged() { echo "$NEWLOG" | grep -c -- "$1"; }

echo "== Setup"
mkdir -p "$DOC/auth" "$DOC/off" "$DOC/dir" "$DOC/args" "$PROG"
echo s > "$DOC/s.txt"; echo alt > "$DOC/alt.txt"; echo err > "$DOC/err.txt"
echo idx > "$DOC/dir/index.html"; echo s > "$DOC/off/s.txt"; echo s > "$DOC/args/s.txt"
echo s > "$DOC/auth/s.txt"
cat > "$DOC/auth/who.rex" <<'EOF'
say 'Content-Type: text/plain'
say ''
say value('REMOTE_USER',, 'ENVIRONMENT') value('AUTH_TYPE',, 'ENVIRONMENT')
EOF
: > "$OUT"; chmod 666 "$OUT"

cat > "$PROG/phase.rex" <<EOF
use arg tag, extra
Address APACHE
"GET HEADER_IN X-Act INTO act"
if rc \\== 0 then act = ''
how = ''
"GET MAIN URI";  if rc == 0 then how = how 'sub'
"GET PREV URI";  if rc == 0 then how = how 'prev'
if arg() > 1 then how = how 'args='arg() || '['extra']'
call lineout '$OUT', tag || how~space(0); call lineout '$OUT'
ret = .nil
do w over act~subWords
  parse var w ph '.' k '=' v
  if ph \\== tag then iterate
  select case k
    when 'ret'    then ret = v
    when 'uri'    then "SET URI" v
    when 'file'   then "SET REQUEST_FILENAME" v
    when 'ct'     then "SET CONTENT_TYPE" v
    when 'hout'   then "SET HEADER_OUT X-Out-"tag v
    when 'errout' then "SET ERR_HEADER_OUT X-Out-"tag v
    when 'loc'    then "SET ERR_HEADER_OUT Location" v
    when 'user'   then "SET USER" v
    when 'hin'    then "SET HEADER_IN X-In" v
    when 'showin' then do
      "GET HEADER_IN X-In INTO x"
      call lineout '$OUT', tag 'X-In=' || x; call lineout '$OUT'
    end
    when 'log'    then "LOG" v 'hello from' tag
    when 'try'    then call try v
    when 'boom'   then x = 1 / 0
  end
end
if ret == .nil then return
return ret

try:
  signal on syntax name refused
  "SET" arg(1)~changeStr(':', ' ') 'x'
  call lineout '$OUT', tag 'allowed' arg(1); call lineout '$OUT'
  return
refused:
  call lineout '$OUT', tag 'refused' arg(1) condition('O')~code; call lineout '$OUT'
  return
EOF
chmod -R a+rX "$LAB"; chmod 666 "$OUT"

H="$PROG/phase.rex"
cat > "$CONF" <<EOF
Listen 8092
<VirtualHost *:8092>
    DocumentRoot $DOC
    LogLevel warn orx:info
    OoRexxPostReadRequestHook $H PostReadRequest
    OoRexxTransHook $H Trans
    ErrorDocument 404 /err.txt
    Alias /aliased $DOC
    <Directory $DOC>
        Options +ExecCGI
        AddHandler oorexx-cgi .rex
        Require all granted
        OoRexxHeaderParserHook $H HeaderParser
        OoRexxAccessHook $H Access
        OoRexxTypeHook $H Type
        OoRexxFixupHook $H Fixup
    </Directory>
    <Directory $DOC/auth>
        AuthType oorexx
        AuthName native
        OoRexxAuthenHook $H Authen
        Require valid-user
    </Directory>
    # A configuration of its own, or Apache would copy the authentication of
    # the main request instead of running the auth phases in the subrequest.
    <Directory $DOC/dir>
        <Files "index.html">
            Require all granted
        </Files>
    </Directory>
    <Directory $DOC/off>
        OoRexxFixupHook none
    </Directory>
    <Directory $DOC/args>
        OoRexxFixupHook $H Fixup "two words"
    </Directory>
</VirtualHost>
EOF
a2enconf oorexx-native >/dev/null
apachectl -k stop; sleep 2; : > "$LOG"; apachectl -k start; sleep 3

echo "== Every phase, in order"
req /s.txt
check "a static file"            "$CODE" 200
check "the phases that ran"      "$RAN" "PostReadRequest Trans HeaderParser Access Type Fixup"
check "the body"                 "$BODY" s

echo "== Return values"
req /s.txt "Trans.file=$DOC/alt.txt Trans.ret=1"
check "1 in Trans, with a file: decided"   "$CODE" 200
check "  served from it"                   "$BODY" alt
req /s.txt "Trans.ret=1"
check "1 in Trans without a file: 500"     "$CODE" 500
check "  and why"                          "$(logged 'returned 1 without an absolute REQUEST_FILENAME')" 1
req /s.txt "Trans.file=rel.txt Trans.ret=1"
check "SET of a relative file: ERROR"      "$CODE" 500
check "  (ERROR, untrapped: the program goes on)" "$(logged 'returned 1 without an absolute REQUEST_FILENAME')" 1
req /s.txt "Trans.uri=/alt.txt"
check "no value: the next module decides"  "$CODE" 200
check "  (the URI it set is translated)"   "$BODY" alt
req /s.txt "HeaderParser.ret=0"
check "0: 403"                             "$CODE" 403
req /s.txt "Access.ret=410"
check "a status"                           "$CODE" 410
req /s.txt "Type.ret=abc"
check "anything else: 500"                 "$CODE" 500
check "  and why"                          "$(logged 'returned "abc"; expected 1, 0, no value')" 1
req /s.txt "Fixup.boom=1"
check "an error in the program: 500"       "$CODE" 500
check "  and the error"                    "$(logged 'Error 42')" 1
req /s.txt "Fixup.ret=302"
check "a 3xx without Location: 500"        "$CODE" 500
check "  and why"                          "$(logged 'returned 302 without a Location')" 1
req /s.txt "Fixup.loc=/x Fixup.ret=302"
check "a 3xx with Location"                "$CODE" 302
check "  the Location, as written"         "$(header Location)" /x
req /s.txt "PostReadRequest.loc=/y PostReadRequest.ret=301"
check "a 3xx from PostReadRequest"         "$CODE" 301
check "  the Location, as written"         "$(header Location)" /y
req /s.txt "Trans.loc=/z Trans.ret=307"
check "a 3xx from Trans"                   "$CODE" 307

echo "== Trans: before mod_alias"
req /aliased/s.txt "Trans.file=$DOC/alt.txt Trans.ret=1"
check "1 in Trans, on an Alias"            "$CODE $BODY" "200 alt"
req /aliased/s.txt
check "no value: mod_alias translates"     "$CODE $BODY" "200 s"

echo "== Headers and a status (D5)"
req /s.txt "Access.hout=a Access.ret=403"
check "HEADER_OUT and a status"            "$CODE" 403
check "  the header is lost"               "$(header X-Out-Access)" ""
check "  and the log says so"              "$(logged 'HEADER_OUT X-Out-Access is lost')" 1
req /s.txt "Access.errout=a Access.ret=403"
check "ERR_HEADER_OUT and a status"        "$CODE" 403
check "  the header survives"              "$(header X-Out-Access)" a
check "  and nothing is said"              "$(logged 'is lost')" 0
req /s.txt "Fixup.hout=b"
check "HEADER_OUT and no status"           "$CODE" 200
check "  the header is sent"               "$(header X-Out-Fixup)" b

echo "== Type: before mod_mime"
req /s.txt "Type.ct=text/x-a Type.ret=1"
check "1 with a content type"              "$CODE" 200
check "  it stays"                         "$(header Content-Type)" text/x-a
req /s.txt "Type.ct=text/x-a"
check "no value: mod_mime decides"         "$CODE" 200
check "  as it would"                      "$(header Content-Type)" text/plain
req /s.txt "Fixup.ct=text/x-b"
check "Fixup, after mod_mime"              "$CODE" 200
check "  has the last word"                "$(header Content-Type)" text/x-b

echo "== What each phase may change"
req /s.txt "PostReadRequest.hin=1 HeaderParser.showin=1 Fixup.showin=1"
check "HEADER_IN set in PostReadRequest is seen later" "$(echo "$RAN" | grep -c 'Fixup X-In=1')" 1
req /s.txt "Access.try=URI Access.try=HEADER_IN:X-A Access.try=HEADER_OUT:X-A Access.try=NOTE:a"
check "Access: URI refused"                "$(echo "$RAN" | grep -c 'Access refused URI 93.900')" 1
check "Access: HEADER_IN refused"          "$(echo "$RAN" | grep -c 'Access refused HEADER_IN:X-A 93.900')" 1
check "Access: HEADER_OUT allowed"         "$(echo "$RAN" | grep -c 'Access allowed HEADER_OUT:X-A')" 1
check "Access: NOTE allowed"               "$(echo "$RAN" | grep -c 'Access allowed NOTE:a')" 1
req /s.txt "Fixup.try=URI Fixup.try=REQUEST_FILENAME Fixup.try=CONTENT_TYPE Type.try=PATH_INFO Trans.try=CONTENT_TYPE"
check "Fixup: URI refused"                 "$(echo "$RAN" | grep -c 'Fixup refused URI 93.900')" 1
check "Type: PATH_INFO refused"            "$(echo "$RAN" | grep -c 'Type refused PATH_INFO 93.900')" 1
check "Trans: CONTENT_TYPE refused"        "$(echo "$RAN" | grep -c 'Trans refused CONTENT_TYPE 93.900')" 1
check "Fixup: CONTENT_TYPE allowed"        "$(echo "$RAN" | grep -c 'Fixup allowed CONTENT_TYPE')" 1
req /s.txt "Fixup.try=USER"
check "USER, outside Authen, refused"      "$(echo "$RAN" | grep -c 'Fixup refused USER 93.900')" 1

echo "== Authen (AuthType oorexx)"
req /auth/who.rex "Authen.user=alice Authen.ret=1"
check "1 with USER"                        "$CODE" 200
check "  REMOTE_USER and AUTH_TYPE reach the content" "$BODY" "alice oorexx"
check "  Authen runs after Access"         "$RAN" "PostReadRequest Trans HeaderParser Access Authen Type Fixup"
req /auth/s.txt "Authen.ret=0"
check "0: 401"                             "$CODE" 401
req /auth/s.txt "Authen.ret=1"
check "1 without USER: 500"                "$CODE" 500
check "  and why"                          "$(logged 'returned 1 without setting USER')" 1
req /auth/s.txt
check "no value: no one else, 500"         "$CODE" 500
req /s.txt "Authen.ret=0"
check "not where AuthType is not oorexx"   "$CODE" 200

echo "== Subrequests and internal redirects"
req /dir/
check "DirectoryIndex"                     "$CODE" 200
check "  in the subrequest only Access ran" "$(echo "$RAN" | tr ' ' '\n' | grep -c sub)" 1
check "  and it was Access"                "$(echo "$RAN" | tr ' ' '\n' | grep sub)" Accesssub
req /missing.txt
check "an ErrorDocument"                   "$CODE" 404
check "  the hooks ran again, with PREV"   "$(echo "$RAN" | tr ' ' '\n' | grep -c prev)" 5
# Not Access: Apache skips the auth phases of a redirect whose configuration
# is that of the request before (server/request.c, 2.4.58).
check "  all but Access"                   "$(echo "$RAN" | tr ' ' '\n' | grep prev | tr '\n' ' ')" \
      "PostReadRequestprev Transprev HeaderParserprev Typeprev Fixupprev "

echo "== Directives"
req /off/s.txt
check "none turns a hook off"              "$CODE" 200
check "  Fixup did not run"                "$RAN" "PostReadRequest Trans HeaderParser Access Type"
req /args/s.txt
check "arguments"                          "$CODE" 200
check "  by Use Arg"                       "$(echo "$RAN" | tr ' ' '\n' | grep Fixup)" "Fixupargs=2[twowords]"
check "Trans in a section: refused"        "$(printf '<Directory /tmp>\nOoRexxTransHook /x.rex\n</Directory>\n' > /etc/apache2/conf-available/oorexx-bad.conf; a2enconf oorexx-bad >/dev/null; apachectl configtest 2>&1 | grep -c 'OoRexxTransHook.*not allowed'; a2disconf oorexx-bad >/dev/null; rm -f /etc/apache2/conf-available/oorexx-bad.conf)" 1

echo "== LOG"
req /s.txt "Fixup.log=info Access.log=debug Type.log=TRACE3 HeaderParser.log=warn"
check "at a level LogLevel lets through"   "$(echo "$NEWLOG" | grep -c '\[orx:info\].*hello from Fixup')" 1
check "and at another"                     "$(echo "$NEWLOG" | grep -c '\[orx:warn\].*hello from HeaderParser')" 1
check "not below LogLevel orx:info"        "$(echo "$NEWLOG" | grep -c 'hello from Access\|hello from Type')" 0
req /s.txt "Fixup.log="
check "LOG without a level: SYNTAX, 500"   "$CODE $(logged 'LOG needs a level')" "500 1"

echo "== The error log"
apachectl -k stop; sleep 2
check "no segfault, nothing unexpected" \
      "$(grep -c -i 'segfault\|AH00052' "$LOG")" 0

echo "== Cleanup"
a2disconf oorexx-native >/dev/null; rm -f "$CONF"; rm -rf "$LAB" /tmp/oorexx-native.h /tmp/oorexx-native.b
: > "$LOG"; apachectl -k start; sleep 3
check "the configuration found is back" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1/test/hola)" 200
echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
