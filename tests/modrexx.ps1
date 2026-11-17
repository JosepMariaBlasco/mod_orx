# The mod_rexx layer of mod_orx on Windows: tests/modrexx.sh and
# tests/modrexx-phases.sh for Windows. Programs written for mod_rexx, with
# mod_rexx's own configuration, under an Apache for Windows with mod_orx and
# the layer; the same programs, the same cases and the same expected answers
# as on Linux (tests/modrexx/), run once with the layer loaded after mod_orx
# and once with it loaded before.
#
#   tests\modrexx.ps1 -Apache C:\Apache24 -OoRexx C:\oorexx
#
# Built first with "build.cmd OOREXX APACHE modrexx". The checks themselves
# are the Rexx programs the Linux tests run (tests/modrexx/run-checks.rex
# and run-cases.rex), which need curl. Listens on -Port and on 8091. Prints
# one line per check, PASS or FAIL, and exits with the number of failures.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

param([Parameter(Mandatory)][string]$Apache, [Parameter(Mandatory)][string]$OoRexx,
      [int]$Port = 8097)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$fails = 0
function check($name, $ok, $detail = '') {
    if ($ok) { "PASS $name" } else { "FAIL $name $detail"; $script:fails++ }
}
function fwd($path) { $path -replace '\\', '/' }
function stop_apache {
    Get-Process httpd -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 1
    Get-Process oorexx-host -ErrorAction SilentlyContinue | Stop-Process -Force
}

# --- Deploy ---------------------------------------------------------------
Copy-Item "$repo\build\oorexx-host.exe" "$Apache\bin\oorexx-host.exe" -Force
Copy-Item "$repo\build\mod_orx.so" "$Apache\modules\mod_orx.so" -Force
Copy-Item "$repo\build\mod_orx_rexx.so" "$Apache\modules\mod_orx_rexx.so" -Force
Copy-Item "$repo\build\orx_rexx_plugin.dll" "$Apache\modules\orx_rexx_plugin.dll" -Force
$root = fwd $Apache
$here = fwd "$repo\tests\modrexx"

# What tests/modrexx.sh serves: the programs, at /modrexx.
$lab = "$Apache\htdocs\modrexx"
New-Item -ItemType Directory -Force $lab | Out-Null
foreach ($n in 'probe', 'respond', 'checks') { Copy-Item "$repo\tests\modrexx\$n.rex" $lab -Force }
[IO.File]::WriteAllText("$lab\static.txt", "hello`n")

# What tests/modrexx-phases.sh serves on 8091: phase.rex once per phase.
$phases = 'PostReadRequest', 'Trans', 'HeaderParser', 'Access', 'Authen', 'Authz', 'Type', 'Fixup', 'Log'
$ph = "$Apache\orx-phases"
Remove-Item -Recurse -Force $ph -ErrorAction SilentlyContinue
foreach ($d in 'doc\auth', 'doc\cauth', 'doc\forced', 'doc\nofix', 'doc\err', 'doc\err2', 'prog', 'log') {
    New-Item -ItemType Directory -Force "$ph\$d" | Out-Null
}
$P = fwd $ph
foreach ($n in $phases + 'Forced') { Copy-Item "$repo\tests\modrexx\phase.rex" "$ph\prog\$n.rex" }
[IO.File]::WriteAllText("$ph\prog\NativeLog.rex",
    "call lineout '$P/log/8091.log', 'NativeLog'`ncall lineout '$P/log/8091.log'`n")
foreach ($d in 'doc', 'doc\auth', 'doc\forced') { Copy-Item "$repo\tests\modrexx\phase.rex" "$ph\$d\Content.rex" }
$files = @{ 'doc\s.txt' = 'static'; 'doc\alt.txt' = 'alt'; 'doc\u.zzz' = 'x'; 'doc\auth\s.txt' = 'private'
            'doc\cauth\s.txt' = 'cprivate'; 'doc\forced\s.txt' = 'f'; 'doc\nofix\s.txt' = 'nofix' }
foreach ($f in $files.Keys) { [IO.File]::WriteAllText("$ph\$f", $files[$f] + "`n") }
& "$Apache\bin\htpasswd.exe" -bc "$ph\prog\users" alice secret 2>&1 | Out-Null

# The configuration: the modules Debian loads, in its order (the order of
# their names), so that the layer's hooks are where they are there; then
# what the two tests configure.
function conf($first) {
    $mods = 'log_config', 'access_compat', 'alias', 'auth_basic', 'authn_core', 'authn_file',
            'authz_core', 'authz_host', 'authz_user', 'autoindex', 'dir', 'env', 'mime', 'negotiation'
    $load = $mods | ForEach-Object { "LoadModule ${_}_module modules/mod_$_.so" }
    $orx = 'LoadModule orx_module modules/mod_orx.so'
    $layer = 'LoadModule orx_rexx_module modules/mod_orx_rexx.so'
    if ($first) { $load += $layer, $orx } else { $load += $orx, $layer }
    $load += 'reqtimeout', 'setenvif' | ForEach-Object { "LoadModule ${_}_module modules/mod_$_.so" }
    $handlers = $phases | ForEach-Object { "    Rexx${_}Handler $P/prog/$_.rex" }
@"
ServerRoot "$root"
ServerName 127.0.0.1:$Port
Listen 127.0.0.1:$Port
$($load -join "`n")
TypesConfig conf/mime.types
DirectoryIndex index.html index.cgi index.pl index.php index.xhtml index.htm
ErrorLog logs/orx-modrexx-error.log
LogLevel warn
PidFile logs/orx-modrexx.pid
DocumentRoot "$root/htdocs"
<Directory />
    AllowOverride None
    Require all denied
</Directory>
<Directory "$root/htdocs">
    Require all granted
</Directory>
<Directory "$root/htdocs/modrexx">
    AddHandler rexx-handler .rex
    RexxSetVar ExampleVar "Hello world"
    RexxSetVar EXAMPLEVAR2 "Second one"
    Require all granted
</Directory>
Listen 127.0.0.1:8091
<VirtualHost *:8091>
    DocumentRoot $P/doc
    RexxSetVar PHLOG $P/log/8091.log
$($handlers -join "`n")
    OoRexxLogHook $P/prog/NativeLog.rex
    <Directory $P/doc>
        AddHandler rexx-handler .rex
        Require all granted
    </Directory>
    <Directory $P/doc/auth>
        AuthType Basic
        AuthName ph
        AuthUserFile $P/prog/users
        Require valid-user
    </Directory>
    <Directory $P/doc/cauth>
        AuthType Rexx
        AuthName ph
        Require valid-user
    </Directory>
    <Directory $P/doc/forced>
        RexxHandler $P/prog/Forced.rex
    </Directory>
    <Directory $P/doc/err>
        ErrorDocument 404 /Content.rex
    </Directory>
    <Directory $P/doc/err2>
        ErrorDocument 404 /s.txt
    </Directory>
    <Directory $P/doc/nofix>
        RexxFixupHandler none
    </Directory>
</VirtualHost>
"@
}

$log = "$Apache\logs\orx-modrexx-error.log"
$env:PATH = "$OoRexx;$env:PATH"      # the host finds rexx.dll there
$env:ORX_ERROR_LOG = fwd $log
# The expected answers have Linux's paths.
$env:ORX_PATH_AS = "$P=/var/www/oorexx-phases"

foreach ($first in $false, $true) {
    $tag = if ($first) { 'layer first' } else { 'layer after' }
    stop_apache
    Remove-Item $log -ErrorAction SilentlyContinue
    conf $first | Out-File -Encoding ascii "$Apache\conf\orx-modrexx.conf"
    & "$Apache\bin\httpd.exe" -t -f "$Apache\conf\orx-modrexx.conf" 2>&1 | ForEach-Object { "  $_" }
    check "${tag}: configuration" ($LASTEXITCODE -eq 0)
    $httpd = Start-Process "$Apache\bin\httpd.exe" -ArgumentList '-f', "$Apache\conf\orx-modrexx.conf" `
               -NoNewWindow -PassThru
    Start-Sleep 4
    $hosts = @(Get-CimInstance Win32_Process -Filter "Name = 'oorexx-host.exe'")
    check "${tag}: Apache is up, with the host and its plugin" ((-not $httpd.HasExited) -and $hosts.Count -gt 0) `
          "($($hosts.Count) host processes)"

    # tests/modrexx.sh's checks.
    $out = & "$OoRexx\rexx.exe" "$here/run-checks.rex" "http://127.0.0.1:$Port/modrexx" /modrexx (fwd $log) "$root/htdocs" $Port 2>&1
    $rc = $LASTEXITCODE
    foreach ($l in $out) {
        $l = "$l"
        if ($l -match '^\s+ok\s+(.*)$') { check "${tag}: $($Matches[1])" $true }
        elseif ($l -match '^\s+FAIL\s+(.*)$') { check "${tag}: $($Matches[1])" $false }
        else { "  $l" }
    }
    check "${tag}: run-checks.rex ran to its end" (@($out | Where-Object { "$_" -like '== checks.rex*' }).Count -gt 0) "(rc $rc)"

    # tests/modrexx-phases.sh's cases. Two of them answer differently on
    # Windows: a program gives the request another file after Apache has
    # looked at the first one, and Apache sends the new file with the old
    # one's size. On Linux the client gets it (padded); on Windows, an empty
    # reply. Of those two, only what the phase programs saw is compared.
    $out = & "$OoRexx\rexx.exe" "$here/run-cases.rex" 8091 "$here/phases.cases" "$P/log/8091.log" rexx-handler `
             "$P/doc" "$here/phases-rexx.expected" 'hp-file fixup-file' 2>&1
    $rc = $LASTEXITCODE
    $said = $false
    foreach ($l in $out) {
        $l = "$l"
        if ($l -match '^\s+ok\s+(.*)$') { check "${tag}: $($Matches[1]), as on Linux" $true; $said = $true }
        elseif ($l -match '^\s+FAIL\s+(.*)$') { check "${tag}: $($Matches[1])" $false; $said = $true }
        else { "  diff: $l" }
    }
    check "${tag}: run-cases.rex ran to its end" $said "(rc $rc)"
}

# --- Cleanup --------------------------------------------------------------
stop_apache
Remove-Item -Recurse -Force $ph, $lab -ErrorAction SilentlyContinue
Remove-Item "$Apache\conf\orx-modrexx.conf" -ErrorAction SilentlyContinue

"RESULT $fails failure(s)"
exit $fails
