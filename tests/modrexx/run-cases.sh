#!/bin/sh
# run-cases.sh PORT CASES PHLOG [HANDLER [DOCROOT]] -- run every case of a cases file against
# 127.0.0.1:PORT and print, for each one, what came back: status, the
# headers that matter, the start of the body, the lines the phase programs
# wrote to PHLOG, and the new lines of the error log (without their date,
# process and client). The same output from two servers means the same
# behaviour. In the cases, @H@ is HANDLER and @D@ is DOCROOT. Used by
# tests/modrexx-phases.sh.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

PORT=$1; CASES=$2; PHLOG=$3; H=${4:-oorexx-modrexx}; D=${5:-/var/www/html}
LOG=/var/log/apache2/error.log
TMP=$(mktemp -d)
grep -v '^#' "$CASES" | grep -v '^ *$' | while read -r name path opts; do
    : > "$PHLOG"
    before=$(wc -l < "$LOG")
    opts=$(echo "$opts" | sed -e "s/@H@/$H/g" -e "s#@D@#$D#g")
    eval "curl -s -m 10 -D '$TMP/h' -o '$TMP/b' $opts 'http://127.0.0.1:$PORT$path'"
    sleep 0.3                      # the logging phase runs after the response
    echo "== $name"
    head -1 "$TMP/h" | tr -d '\r'
    grep -ai -E '^(content-type|location|www-authenticate|x-out-[a-z]*):' "$TMP/h" \
        | tr -d '\r' | sed "s/:$PORT\//:PORT\//" | sort
    tr -d '\r' < "$TMP/b" | grep -av '^Date:' | head -3 | cut -c1-100 | sed 's/^/  body: /'
    sed 's/^/  phase: /' "$PHLOG"
    tail -n +$((before + 1)) "$LOG" | grep -av 'AH00094\|AH00489\|AH00493' \
        | sed -e 's/^\[[^]]*\] \[\([^]]*\)\] \[[^]]*\] /[\1] /' \
              -e 's/\[client [^]]*\] //' | sed 's/^/  log: /'
done
rm -rf "$TMP"
