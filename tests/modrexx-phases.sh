#!/bin/sh
# Check programs written for mod_rexx in every phase of the request, with
# both sets of directives:
#
#   Rexx*    mod_rexx's own, with its meaning: the configuration a site
#            had for mod_rexx, as it was. Measured against mod_rexx 2.4.0
#            itself (Apache 2.4.58, ooRexx 5.3.0) with this same
#            configuration and these same cases: the answers are the same,
#            except where mod_rexx crashed Apache's process (its
#            PostReadRequest and Trans phases always did, and so did
#            WWWInternal_Redirect outside the content phase), the form of
#            the error log's lines, and a WWWInternal_Redirect from the
#            content phase, which mod_rexx answered with a broken response.
#   OoRexx*  mod_orx's: each phase where its name says, inheritance, and
#            "none".
#
# One program, tests/modrexx/phase.rex, is installed once per phase, with
# the phase as its name; it logs what it sees and does what the request
# header X-Act asks of it. tests/modrexx/phases.cases lists the requests;
# tests/modrexx/run-cases.sh runs them and prints what came back, which is
# compared with phases-rexx.expected and phases-oorexx.expected.
#
# Run after tests/rexxhttp-suite.sh, which leaves Apache configured with
# mod_orx (any MPM). mod_rexx must not be loaded: it defines the same
# Rexx* directives. Runs as root; listens on ports 8091 and 8092 for the
# run. Leaves nothing behind.
#
#   tests/modrexx-phases.sh            check
#   tests/modrexx-phases.sh --update   rewrite the expected files
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

[ "$1" = --update ] && UPDATE=1
HERE=$(cd "$(dirname "$0")" && pwd)
LAB=/var/www/oorexx-phases
DOC=$LAB/doc
PROG=$LAB/prog
LOGS=$LAB/log
CONF=/etc/apache2/conf-available/oorexx-phases.conf
TMP=$(mktemp -d)
FAILS=0
PHASES="PostReadRequest Trans HeaderParser Access Authen Authz Type Fixup Log"

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }

# vhost PREFIX PORT HANDLER: the same configuration for both sets.
vhost() {
    echo "Listen $2"
    echo "<VirtualHost *:$2>"
    echo "    DocumentRoot $DOC"
    echo "    ${1}SetVar PHLOG $LOGS/$2.log"
    for ph in $PHASES; do echo "    ${1}${ph}Handler $PROG/$ph.rex"; done
    echo "    <Directory $DOC>"
    echo "        AddHandler $3 .rex"
    echo "        Require all granted"
    echo "    </Directory>"
    echo "    <Directory $DOC/auth>"
    echo "        AuthType Basic"
    echo "        AuthName ph"
    echo "        AuthUserFile $PROG/users"
    echo "        Require valid-user"
    echo "    </Directory>"
    echo "    <Directory $DOC/cauth>"
    echo "        AuthType Rexx"
    echo "        AuthName ph"
    echo "        Require valid-user"
    echo "    </Directory>"
    echo "    <Directory $DOC/forced>"
    echo "        ${1}Handler $PROG/Forced.rex"
    echo "    </Directory>"
    echo "    <Directory $DOC/nofix>"
    echo "        ${1}FixupHandler none"
    echo "    </Directory>"
    echo "</VirtualHost>"
}

echo "== Setup"
mkdir -p "$DOC/auth" "$DOC/cauth" "$DOC/forced" "$DOC/nofix" "$PROG" "$LOGS"
for ph in $PHASES Forced; do cp "$HERE/modrexx/phase.rex" "$PROG/$ph.rex"; done
for d in "$DOC" "$DOC/auth" "$DOC/forced"; do cp "$HERE/modrexx/phase.rex" "$d/Content.rex"; done
echo static > "$DOC/s.txt";       echo alt > "$DOC/alt.txt"
echo x > "$DOC/u.zzz";            echo private > "$DOC/auth/s.txt"
echo cprivate > "$DOC/cauth/s.txt"; echo f > "$DOC/forced/s.txt"
echo nofix > "$DOC/nofix/s.txt"
htpasswd -bc "$PROG/users" alice secret 2>/dev/null
touch "$LOGS/8091.log" "$LOGS/8092.log"
chmod -R a+rX "$LAB"; chown -R www-data "$LOGS"
{ vhost Rexx 8091 rexx-handler; vhost OoRexx 8092 oorexx-modrexx; } > "$CONF"
a2enconf oorexx-phases >/dev/null
apachectl -k graceful; sleep 2

for run in "rexx 8091 rexx-handler" "oorexx 8092 oorexx-modrexx"; do
    set -- $run
    echo "== The $1 set ($3)"
    "$HERE/modrexx/run-cases.sh" "$2" "$HERE/modrexx/phases.cases" "$LOGS/$2.log" "$3" "$DOC" \
        | sed "s/$3/HANDLER/g" > "$TMP/$1"
    if [ "$UPDATE" = 1 ]; then
        cp "$TMP/$1" "$HERE/modrexx/phases-$1.expected"; ok "phases-$1.expected rewritten"
    elif diff "$HERE/modrexx/phases-$1.expected" "$TMP/$1" > "$TMP/diff"; then
        ok "every case as expected ($(grep -c '^== ' "$TMP/$1"))"
    else
        fail "the $1 set:"; cat "$TMP/diff"
    fi
done

echo "== Cleanup"
a2disconf oorexx-phases >/dev/null
rm -f "$CONF"
rm -rf "$LAB" "$TMP"
apachectl -k graceful; sleep 1

echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
