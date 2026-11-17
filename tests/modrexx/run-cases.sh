#!/bin/sh
# run-cases.sh PORT CASES PHLOG [HANDLER [DOCROOT]] -- run every case of a cases file against
# 127.0.0.1:PORT and print, for each one, what came back: status, the
# headers that matter, the start of the body, the lines the phase programs
# wrote to PHLOG, and the new lines of the error log (without their date,
# process and client). A Date or Server header in the body (a HEAD shown
# with curl -I) is left out. The DOCTYPE of Apache's own error pages is cut short
# (it is not the same in every 2.4), and a request curl could not finish
# says so. The same output from two servers means the same
# behaviour. In the cases, @H@ is HANDLER and @D@ is DOCROOT. The error log
# is ORX_ERROR_LOG (by default, Debian's). Used by tests/modrexx-phases.sh
# and, on Windows, under Git's bash, by tests/modrexx.ps1.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

PORT=$1; CASES=$2; PHLOG=$3; H=${4:-rexx-handler}; D=${5:-/var/www/html}
LOG=${ORX_ERROR_LOG:-/var/log/apache2/error.log}
TMP=$(mktemp -d)
# Windows (Git's bash): paths that curl.exe takes too, and arguments as
# they are (a "/x" in a header is not a path to convert).
case "$(uname -s)" in
    MINGW*|MSYS*) TMP=$(cygpath -m "$TMP")
                  export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' ;;
esac
# Paths as the expected files have them: ORX_PATH_AS="FROM=TO" writes TO
# for FROM (whatever its case) in what is printed. For a server whose
# files are not where the expected ones were measured.
norm() {
    if [ -n "$ORX_PATH_AS" ]; then sed "s#${ORX_PATH_AS%%=*}#${ORX_PATH_AS#*=}#gI"; else cat; fi
}
grep -v '^#' "$CASES" | grep -v '^ *$' | while read -r name path opts; do
    : > "$PHLOG"
    before=$(wc -l < "$LOG" | tr -d ' ')
    opts=$(echo "$opts" | sed -e "s/@H@/$H/g" -e "s#@D@#$D#g")
    : > "$TMP/h"; : > "$TMP/b"
    eval "curl -s -m 10 -D '$TMP/h' -o '$TMP/b' $opts 'http://127.0.0.1:$PORT$path'"; rc=$?
    sleep 0.3                      # the logging phase runs after the response
    echo "== $name"
    [ $rc = 0 ] || echo "curl: error $rc"
    head -1 "$TMP/h" | tr -d '\r'
    grep -ai -E '^(content-type|location|www-authenticate|x-out-[a-z]*):' "$TMP/h" \
        | tr -d '\r' | sed "s/:$PORT\//:PORT\//" | sort
    tr -d '\r' < "$TMP/b" | grep -av '^Date:\|^Server:' | head -3 | norm | cut -c1-100 \
        | sed -e 's/^<!DOCTYPE HTML PUBLIC .*/<!DOCTYPE HTML PUBLIC ...>/' -e 's/^/  body: /'
    tr -d '\r' < "$PHLOG" | norm | sed 's/^/  phase: /'
    tail -n +$((before + 1)) "$LOG" | tr -d '\r' | grep -av 'AH00094\|AH00489\|AH00493' \
        | sed -e 's/^\[[^]]*\] \[\([^]]*\)\] \[[^]]*\] /[\1] /' \
              -e 's/\[client [^]]*\] //' | norm | sed 's/^/  log: /'
done
rm -rf "$TMP"
