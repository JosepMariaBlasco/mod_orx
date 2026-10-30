#!/bin/sh
# Check OoRexxPreload: what a preloaded package shares between phases, what a
# program's own package does not, and how a bad preload is reported.
#
# Run after tests/rexxhttp-suite.sh, which leaves Apache configured with
# mod_orx (any MPM). Runs as root. Restores the configuration it found.
#
#   tests/preload.sh
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

LAB=/var/www/html/oorexx-preload
BIN=/var/www/oorexx-preload-bin
LOG=/var/log/apache2/error.log
CONF=/etc/apache2/mods-available/orx.conf
URL=http://127.0.0.1/oorexx-preload
FAILS=0

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected '$3', got '$2')"; fi; }
body() { curl -s -m 20 "$URL/$1"; }
three() { echo "$(body "$1") $(body "$1") $(body "$1")"; }
restart() { apachectl -k stop; sleep 2; : > "$LOG"; apachectl -k start; sleep 3; }

echo "== Setup"
mkdir -p "$LAB" "$BIN"
cp "$CONF" "$BIN/orx.conf.saved"
# A class with state of its own, and nothing that keeps it reachable (no
# .environment entry): only the host can keep it loaded.
for c in Shared Own; do
    cat > "$BIN/$c.cls" <<EOF
::class $c public
::method count class
  expose n
  if \\var('N') then n = 0
  n = n + 1
  return n
EOF
done
for c in Shared Own; do
    cat > "$LAB/$c.rex" <<EOF
say 'Content-Type: text/plain'
say ''
call charout , .$c~count
::requires '$BIN/$c.cls'
EOF
done
chmod -R a+rX "$LAB" "$BIN"
cat > /etc/apache2/conf-available/oorexx-preload.conf <<EOF
<Directory "$LAB">
    Options +ExecCGI
    AddHandler oorexx-cgi .rex
    Require all granted
</Directory>
EOF
a2enconf oorexx-preload >/dev/null
# One process, so that every request reaches the same one.
grep -v -E '^(OoRexxProcesses|OoRexxPreload)' "$BIN/orx.conf.saved" > "$CONF"
echo "OoRexxProcesses 1" >> "$CONF"
echo "OoRexxPreload $BIN/Shared.cls" >> "$CONF"
restart

echo "== Sharing"
check "a preloaded class keeps its state from one phase to the next" "$(three Shared.rex)" "1 2 3"
check "a program's own class starts afresh in every phase"          "$(three Own.rex)" "1 1 1"
check "nothing in the error log" "$(grep -v -c ':notice\]' "$LOG")" 0

echo "== A missing file: refused when the configuration is read"
echo "OoRexxPreload $BIN/Missing.cls" >> "$CONF"
check "apachectl configtest names it" \
      "$(apachectl configtest 2>&1 | grep -c "OoRexxPreload: not a file: $BIN/Missing.cls")" 1
sed -i '/Missing.cls/d' "$CONF"

echo "== A file that does not load: the host stops, and says why"
printf '::class Bad public\n::method x\n  if then\n' > "$BIN/Bad.cls"; chmod a+r "$BIN/Bad.cls"
echo "OoRexxPreload $BIN/Bad.cls" >> "$CONF"
restart
sleep 2
check "the error, with file and line" \
      "$(grep -c "OoRexxPreload failed: Error 35 in $BIN/Bad.cls at line 3" "$LOG")" 1
check "not restarted"                  "$(grep -c 'could not start (status 2); not restarting it' "$LOG")" 1
check "no host process left"           "$(pgrep -c -x oorexx-host)" 0

echo "== Cleanup"
cp "$BIN/orx.conf.saved" "$CONF"
a2disconf oorexx-preload >/dev/null; rm -rf "$LAB" "$BIN"
restart
check "the configuration found is back" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1/test/hola)" 200
echo "== $FAILS failure(s)"
[ $FAILS = 0 ]
