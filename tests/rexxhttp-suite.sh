#!/bin/sh
# Run the RexxHTTP HTTP test suite through mod_orx instead of mod_cgid.
#
# RexxHTTP's own runner deploys its sources, fixtures and Apache config with
# the standard CGI setup (mod_cgid). This script then swaps the CGI handler
# for oorexx-cgi, disables mod_cgid so that nothing can fall back to it,
# and runs the suite unchanged. Apache itself starts oorexx-host (the module
# manages it); nothing is started by hand. Green means the same
# counts as under mod_cgid, with 0 failures, 0 errors and an empty
# error.log.
#
# OOREXX_PROCESSES ("min max") and OOREXX_MAX_REQUESTS override the pool's
# configuration.
#
# Requirements: Debian/Ubuntu Apache (a2enmod & co.), ooRexx, mod_orx and
# oorexx-host installed (make install), a RexxHTTP checkout, run as root.
#
#   tests/rexxhttp-suite.sh /path/to/rexxhttp
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

set -e
REXXHTTP=${1:?usage: $0 /path/to/rexxhttp}
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

echo "== Swapping mod_cgid for mod_orx"
sed -i 's/AddHandler cgi-script \.rex/AddHandler oorexx-cgi .rex/; s/^ScriptAlias /Alias /' "$CONF"
cat > /etc/apache2/mods-available/orx.load <<EOF
LoadModule orx_module $(apxs -q LIBEXECDIR)/mod_orx.so
EOF
cat > /etc/apache2/mods-available/orx.conf <<EOF
OoRexxHost $(command -v oorexx-host)
OoRexxProcesses ${OOREXX_PROCESSES:-2 8}
OoRexxMaxRequests ${OOREXX_MAX_REQUESTS:-1000}
OoRexxTimeout 30
EOF
a2enmod orx >/dev/null
a2dismod -f cgid >/dev/null 2>&1 || true

apachectl -k restart
sleep 1
: > /var/log/apache2/error.log    # after the restart: count only the suite

echo "== Running the suite"
(cd "$REXXHTTP/tests" && PATH="framework:http:$PATH" rexx http/RexxHTTP.testGroup) | tail -8
echo "== error.log: $(wc -l < /var/log/apache2/error.log) line(s)"
