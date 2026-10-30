#!/bin/sh
# Run the RexxHTTP HTTP test suite through mod_oorexx instead of mod_cgid.
#
# RexxHTTP's own runner deploys its sources, fixtures and Apache config with
# the standard CGI setup (mod_cgid). This script then swaps the CGI handler
# for oorexx-cgi, disables mod_cgid so that nothing can fall back to it,
# starts oorexx-host, and runs the suite unchanged. Green means the same
# counts as under mod_cgid, with 0 failures, 0 errors and an empty
# error.log.
#
# Requirements: Debian/Ubuntu Apache (a2enmod & co.), ooRexx, mod_oorexx and
# oorexx-host installed (make install), a RexxHTTP checkout, run as root.
#
#   tests/rexxhttp-suite.sh /path/to/rexxhttp
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

set -e
REXXHTTP=${1:?usage: $0 /path/to/rexxhttp}
SOCK_DIR=/run/oorexx
SOCK=$SOCK_DIR/host.sock
CONF=/etc/apache2/conf-available/rexx-cgi.conf

# The RexxHTTP runner calls sudo; inside a root-only container there is none.
command -v sudo >/dev/null || {
    printf '#!/bin/sh\nexec "$@"\n' > /usr/local/bin/sudo
    chmod 755 /usr/local/bin/sudo
}

echo "== Deploying the RexxHTTP suite (standard CGI setup)"
# The runner insists on cgid and actions (it asks interactively otherwise);
# cgid is disabled again below, once the setup is done.
a2enmod cgid actions >/dev/null
(cd "$REXXHTTP" && rexx tests/http/RunHTTPTests.rex --setup-only </dev/null)

echo "== Swapping mod_cgid for mod_oorexx"
sed -i 's/AddHandler cgi-script \.rex/AddHandler oorexx-cgi .rex/; s/^ScriptAlias /Alias /' "$CONF"
cat > /etc/apache2/mods-available/oorexx.load <<EOF
LoadModule oorexx_module $(apxs -q LIBEXECDIR)/mod_oorexx.so
EOF
cat > /etc/apache2/mods-available/oorexx.conf <<EOF
OoRexxSocket $SOCK
OoRexxTimeout 30
EOF
a2enmod oorexx >/dev/null
a2dismod -f cgid >/dev/null 2>&1 || true

echo "== Starting oorexx-host"
mkdir -p "$SOCK_DIR"
chown www-data:www-data "$SOCK_DIR"
chmod 700 "$SOCK_DIR"
M=$(pgrep -x -o oorexx-host || true); [ -n "$M" ] && kill "$M" && sleep 1
runuser -u www-data -- oorexx-host --socket "$SOCK" --processes 4 &
sleep 2

apachectl -k restart
sleep 1
: > /var/log/apache2/error.log    # after the restart: count only the suite

echo "== Running the suite"
(cd "$REXXHTTP/tests" && PATH="framework:http:$PATH" rexx http/RexxHTTP.testGroup) | tail -8
echo "== error.log: $(wc -l < /var/log/apache2/error.log) line(s)"
