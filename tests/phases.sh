#!/bin/sh
# Check the Rexx hooks outside the content phase: the authentication
# provider (AuthBasicProvider oorexx), the authorization provider (Require
# oorexx) and the logging hook (OoRexxLogHook), alone and combined with
# Apache's own providers.
#
# Run after tests/rexxhttp-suite.sh, which leaves Apache configured with
# mod_orx (any MPM). Runs as root.
#
#   tests/phases.sh
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

LAB=/var/www/html/oorexx-phases
BIN=/var/www/oorexx-phases-bin
LOG=/var/log/apache2/error.log
OUT=/tmp/oorexx-phases.out
URL=http://127.0.0.1/oorexx-phases
FAILS=0

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected '$3', got '$2')"; fi; }
# get PATH [curl options...]: the HTTP status
get() { p=$1; shift; curl -s -m 20 -o /dev/null -w '%{http_code}' "$@" "$URL/$p"; }
logged() { grep -c -- "$1" "$LOG"; }

echo "== Setup"
mkdir -p "$LAB/authn" "$LAB/authz" "$LAB/user" "$LAB/anon" "$LAB/any" "$LAB/neutral" \
         "$LAB/all" "$LAB/broken" "$LAB/logged/quiet" "$BIN"
for d in authn authz user anon any neutral all broken logged logged/quiet; do
    echo hello > "$LAB/$d/hello.txt"
done
htpasswd -bc "$BIN/users" carol pw >/dev/null 2>&1
htpasswd -b  "$BIN/users" alice pw >/dev/null 2>&1
htpasswd -b  "$BIN/users" bob   pw >/dev/null 2>&1
: > "$OUT"; chmod 666 "$OUT"

cat > "$BIN/authn.rex" <<'EOF'
use arg user, password
select
  when user == 'alice' then return password == 'secret'
  when user == 'boom'  then return 1 / 0
  when user == 'weird' then return 'yes'
  when user == 'env'   then return value('REMOTE_ADDR',, 'ENVIRONMENT') == '127.0.0.1'
  when user == 'talk'  then do; say 'hello from authn'; return 1; end
  when user == 'slow'  then do; call SysSleep 5; return 1; end
  otherwise return
end
EOF
cat > "$BIN/authz.rex" <<EOF
use arg user, words
call lineout '$OUT', 'authz ['user'] ['words']'; call lineout '$OUT'
return wordpos(user, words) > 0
EOF
cat > "$BIN/gate.rex" <<'EOF'
return value('HTTP_X_GATE',, 'ENVIRONMENT') == 'open'
EOF
cat > "$BIN/neutral.rex" <<'EOF'
return
EOF
cat > "$BIN/yes.rex" <<'EOF'
return 1
EOF
cat > "$BIN/boom.rex" <<'EOF'
say 1 / 0
EOF
cat > "$BIN/log.rex" <<EOF
use arg status, bytes, micros
call lineout '$OUT', 'log' status bytes datatype(micros, 'W') value('REQUEST_URI',, 'ENVIRONMENT')
call lineout '$OUT'
EOF

cat > /etc/apache2/conf-available/oorexx-phases.conf <<EOF
<Directory "$LAB">
    Require all granted
</Directory>
<Location /oorexx-phases/authn/>
    AuthType Basic
    AuthName phases
    AuthBasicProvider oorexx file
    AuthUserFile $BIN/users
    OoRexxAuthnProgram $BIN/authn.rex
    OoRexxTimeout 2
    Require valid-user
</Location>
<Location /oorexx-phases/authz/>
    AuthType Basic
    AuthName phases
    AuthBasicProvider file
    AuthUserFile $BIN/users
    Require oorexx $BIN/authz.rex admins alice
</Location>
<Location /oorexx-phases/user/>
    AuthType Basic
    AuthName phases
    AuthBasicProvider file
    AuthUserFile $BIN/users
    Require user alice
</Location>
<Location /oorexx-phases/anon/>
    Require oorexx $BIN/gate.rex
</Location>
<Location /oorexx-phases/any/>
    <RequireAny>
        Require oorexx $BIN/neutral.rex
        Require ip 127.0.0.1
    </RequireAny>
</Location>
<Location /oorexx-phases/neutral/>
    Require oorexx $BIN/neutral.rex
</Location>
<Location /oorexx-phases/all/>
    <RequireAll>
        Require oorexx $BIN/yes.rex
        Require ip 192.0.2.1
    </RequireAll>
</Location>
<Location /oorexx-phases/broken/>
    Require oorexx $BIN/boom.rex
</Location>
<Location /oorexx-phases/logged/>
    OoRexxLogHook $BIN/log.rex
</Location>
<Location /oorexx-phases/logged/quiet/>
    OoRexxLogHook none
</Location>
EOF
a2enconf oorexx-phases >/dev/null
apachectl -k restart; sleep 2
: > "$LOG"

echo "== Authentication: AuthBasicProvider oorexx file"
check "1: granted"                          "$(get authn/hello.txt -u alice:secret)" 200
check "0: denied"                           "$(get authn/hello.txt -u alice:wrong)" 401
check "no value: not found, the next provider decides (yes)" "$(get authn/hello.txt -u carol:pw)" 200
check "no value: not found, the next provider decides (no)"  "$(get authn/hello.txt -u carol:no)" 401
check "no value, and nobody knows the user" "$(get authn/hello.txt -u nobody:x)" 401
check "no credentials"                      "$(get authn/hello.txt)" 401
check "the program sees the CGI environment" "$(get authn/hello.txt -u env:x)" 200
check "an error in the program: 500"        "$(get authn/hello.txt -u boom:x)" 500
check "  ...logged"                         "$(logged 'authn.rex: line 4: Error 42')" 1
check "a value other than 1, 0 or none: 500" "$(get authn/hello.txt -u weird:x)" 500
check "  ...logged"                         "$(logged 'returned "yes"; expected 1, 0 or no value')" 1
check "its output goes to the error log"    "$(get authn/hello.txt -u talk:x)" 200
check "  ...logged"                         "$(logged 'hello from authn')" 1
check "past OoRexxTimeout: halted, 500"     "$(get authn/hello.txt -u slow:x)" 500
check "  ...logged"                         "$(logged 'authn.rex.*(phase timeout)')" 1

echo "== Authorization: Require oorexx program words"
: > "$OUT"
check "a user it accepts"                   "$(get authz/hello.txt -u alice:pw)" 200
check "  ...asked without a user, then with one" "$(cat "$OUT" | tr '\n' '|')" \
      "authz [] [admins alice]|authz [alice] [admins alice]|"
# Apache answers an authenticated user it does not authorize with 401, as
# long as AuthzSendForbiddenOnFailure is off (the default): Require user
# does the same, which is what the second check shows.
check "a user it rejects: 401, as Apache does" "$(get authz/hello.txt -u bob:pw)" 401
check "  ...exactly as Require user"        "$(get user/hello.txt -u bob:pw)" 401
check "no user: Apache asks for one (401)"  "$(get authz/hello.txt)" 401
check "no AuthType, gate open"              "$(get anon/hello.txt -H 'X-Gate: open')" 200
check "no AuthType, gate closed: 403 (nobody to ask for)" "$(get anon/hello.txt)" 403
check "no value is neutral: RequireAny lets Require ip decide" "$(get any/hello.txt)" 200
check "no value alone: denied"              "$(get neutral/hello.txt)" 403
check "RequireAll: 1 here, but Require ip says no" "$(get all/hello.txt)" 403
check "an error in the program: 500"        "$(get broken/hello.txt)" 500

echo "== Logging: OoRexxLogHook"
: > "$OUT"
get logged/hello.txt >/dev/null
get logged/missing.txt >/dev/null
get logged/quiet/hello.txt >/dev/null
sleep 1
check "status, bytes, microseconds and the environment" "$(sed -n 1p "$OUT")" \
      "log 200 6 1 /oorexx-phases/logged/hello.txt"
check "a 404 is logged too"                 "$(sed -n 2p "$OUT" | cut -d' ' -f2,5)" \
      "404 /oorexx-phases/logged/missing.txt"
check "OoRexxLogHook none switches it off"  "$(wc -l < "$OUT")" 2

echo "== error.log"
check "nothing but the expected lines" \
      "$(grep -v -E 'Error 42|returned "yes"|hello from authn|phase timeout|AH01618|AH01617|AH01630|AH01631|boom.rex' "$LOG" | wc -l)" 0

a2disconf oorexx-phases >/dev/null; rm -rf "$LAB" "$BIN" "$OUT"
apachectl -k graceful; sleep 1
echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
