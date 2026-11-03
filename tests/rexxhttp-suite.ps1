# The RexxHTTP HTTP test suite on Windows, through mod_cgi or mod_orx.
#
# RexxHTTP's own runner (tests/http/RunHTTPTests.rex) targets Linux. This
# script deploys the same montage into an Apache for Windows (Apache Lounge's):
# the five sources, the fixtures with their whitelist .htaccess, and the
# CGI-contract matrix, at the same URL paths and, on the current drive, the
# same disk paths (/var/www/html, /opt/rexxhttp-matrix). Then it runs the
# suite unchanged.
#
#   tests\rexxhttp-suite.ps1 -Apache C:\Apache24 -OoRexx C:\oorexx `
#                            -RexxHTTP C:\rexxhttp [-Handler orx|cgi]
#
# -Handler cgi runs RexxHTTP.rex through mod_cgi (rexx.exe per request), the
# reference; orx (the default) through mod_orx, built first with build.cmd.
# Prints the suite's counts and the error log's lines; exits non-zero unless
# the suite ran with 0 failures, 0 errors and an empty error log.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

param([Parameter(Mandatory)][string]$Apache, [Parameter(Mandatory)][string]$OoRexx,
      [Parameter(Mandatory)][string]$RexxHTTP, [ValidateSet('orx', 'cgi')][string]$Handler = 'orx',
      [string]$Out = "$env:TEMP\rexxhttp-suite-$Handler.txt")

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$root = $Apache -replace '\\', '/'
$src = "$RexxHTTP\src"
$fx = "$RexxHTTP\tests\http\fixtures"

# --- The montage, as RunHTTPTests.rex lays it out ----------------------------
$deploy = '\var\www\html\RexxHTTP'
$test = '\var\www\html\test'
foreach ($d in $deploy, $test, '\var\www\html\mtx\sub', '\opt\rexxhttp-matrix\sub') {
    New-Item -ItemType Directory -Force $d | Out-Null
}
foreach ($f in 'HTTP.Request.cls', 'HTTP.Response.cls', 'HTTP.OutputStream.cls', 'HTTP.Cookie.cls',
               'RexxHTTP.rex') {
    Copy-Item "$src\$f" "$deploy\$f" -Force
}
if ($Handler -eq 'cgi') {
    # mod_cgi runs the processor through its shebang: the interpreter's path.
    $p = "$deploy\RexxHTTP.rex"
    $t = [IO.File]::ReadAllText($p)
    $t = $t -replace '^#![^\n]*', ('#!' + ($OoRexx -replace '\\', '/') + '/rexx.exe')
    [IO.File]::WriteAllText($p, $t)
}

# The fixtures, in the runner's whitelist order, with its SetEnv lines.
$fixtures = ('hola multi qs check setck readck setckopts badck ' +
    'late partial boom htmltype polstrict polbad badnm bsnm arrout ' +
    'commitfail flushcrash addckfail ' +
    'setpair readpair readname dupck ' +
    'err404 errgen errescape errreset rdr rdr301 rdr303 ' +
    'envpool enc ' +
    'urlprobe urlnobar urlremap ' +
    'crlfhdr crlfrdr crlfck ' +
    'errthenfail chunkecho methodbody ctnorm ctnormsp ctnormqm ctnormws argopt arroutopt headwrap nrdy'
    ) -split ' '
$setenv = @{
    polstrict = @('REXXHTTP_ARGPOLICY Strict')
    polbad    = @('REXXHTTP_ARGPOLICY disparate')
    envpool   = @('MYPLAIN plainvalue', '_MYVAR underscored', 'REDIRECT_HTTP_FAKE fallbackvalue')
    urlprobe  = @('REXXHTTP_BASE /var/www/html/test', 'REXXHTTP_URLBASE /test')
    urlremap  = @('REXXHTTP_BASE /var/www/html/test', 'REXXHTTP_URLBASE /otheria')
}
$ht = New-Object Text.StringBuilder
foreach ($f in $fixtures) {
    Copy-Item "$fx\$f" "$test\$f" -Force
    [void]$ht.Append("<Files $f>`n  SetHandler RexxHTTPCGI`n")
    if ($setenv.ContainsKey($f)) { foreach ($l in $setenv[$f]) { [void]$ht.Append("  SetEnv $l`n") } }
    [void]$ht.Append("</Files>`n")
}
[IO.File]::WriteAllText("$test\.htaccess", $ht.ToString())
# The 63 bytes the HEAD/GET wrapper tests expect, LF and all.
[IO.File]::WriteAllText("$test\headwrap.data",
    "FIXED PAYLOAD LINE 1`nFIXED PAYLOAD LINE 2`nFIXED PAYLOAD LINE 3`n")

# The CGI-contract matrix: the same probe in two zones.
foreach ($z in '\var\www\html\mtx', '\opt\rexxhttp-matrix') {
    Copy-Item "$fx\cgivars" "$z\probe.rxl" -Force
    Copy-Item "$fx\cgivars" "$z\index.rxl" -Force
    [IO.File]::WriteAllText("$z\target.mtx", "matrix target`n")
    [IO.File]::WriteAllText("$z\sub\index.mtx", "matrix index`n")
    [IO.File]::WriteAllText("$z\spaced doc.mtx", "spaced doc`n")
}

# --- Apache's configuration: its own httpd.conf, plus the montage -----------
# Apache Lounge's httpd.conf as shipped (its modules, its defaults), with the
# server root where it is, port 80 (the suite's default base URL) and the
# document root of the Debian montage.
$conf = [IO.File]::ReadAllText("$Apache\conf\httpd.conf")
$conf = $conf -replace '(?m)^Define SRVROOT .*$', "Define SRVROOT `"$root`""
$conf = $conf -replace '(?m)^Listen .*$', 'Listen 127.0.0.1:80'
$conf = $conf -replace '(?m)^DocumentRoot .*$', 'DocumentRoot "/var/www/html"'
$conf = $conf -replace '(?m)^<Directory "\$\{SRVROOT\}/htdocs">', '<Directory "/var/www/html">'
$conf = $conf -replace '(?m)^#(LoadModule actions_module .*)$', '$1'
if ($Handler -eq 'cgi') { $conf = $conf -replace '(?m)^#(LoadModule cgi_module .*)$', '$1' }
$conf = $conf -replace '(?m)^ErrorLog .*$', 'ErrorLog "logs/suite-error.log"'
$conf = $conf -replace '(?m)^LogLevel .*$', 'LogLevel warn'

if ($Handler -eq 'cgi') {
    $proc = @'
<Directory "/var/www/html/RexxHTTP">
    Options +ExecCGI
    AddHandler cgi-script .rex
    Require all granted
</Directory>
ScriptAlias /RexxHTTP/ /var/www/html/RexxHTTP/
'@
} else {
    $proc = @"
LoadModule orx_module modules/mod_orx.so
OoRexxProcesses 2 8
OoRexxMaxRequests 1000
OoRexxTimeout 30
<Directory "/var/www/html/RexxHTTP">
    Options +ExecCGI
    AddHandler oorexx-cgi .rex
    Require all granted
</Directory>
Alias /RexxHTTP/ /var/www/html/RexxHTTP/
"@
}
$montage = $proc + @'

ServerName localhost
Action RexxHTTPCGI /RexxHTTP/RexxHTTP.rex

<Directory "/var/www/html/test">
    AllowOverride All
    Options +ExecCGI
    Require all granted
</Directory>

<Directory "/var/www/html/mtx">
    AllowOverride None
    Options +ExecCGI
    Require all granted
    SetEnv REXXHTTP_BASE /var/www/html/mtx
    SetEnv REXXHTTP_URLBASE /mtx
    DirectoryIndex index.rxl
    <Files "probe.rxl">
        SetHandler RexxHTTPCGI
    </Files>
    <Files "index.rxl">
        SetHandler RexxHTTPCGI
    </Files>
    AddHandler mtxhandler-doc .mtx
    Action mtxhandler-doc /mtx/probe.rxl
</Directory>
<Directory "/var/www/html/mtx/sub">
    AllowOverride None
    Options +ExecCGI
    Require all granted
    SetEnv REXXHTTP_BASE /var/www/html/mtx
    SetEnv REXXHTTP_URLBASE /mtx
    DirectoryIndex index.mtx
</Directory>

Alias /ali /opt/rexxhttp-matrix
<Directory "/opt/rexxhttp-matrix">
    AllowOverride None
    Options +ExecCGI
    Require all granted
    SetEnv REXXHTTP_BASE /opt/rexxhttp-matrix
    SetEnv REXXHTTP_URLBASE /ali
    DirectoryIndex index.rxl
    <Files "probe.rxl">
        SetHandler RexxHTTPCGI
    </Files>
    <Files "index.rxl">
        SetHandler RexxHTTPCGI
    </Files>
    AddHandler mtxhandler-ali .mtx
    Action mtxhandler-ali /ali/probe.rxl
</Directory>
<Directory "/opt/rexxhttp-matrix/sub">
    AllowOverride None
    Options +ExecCGI
    Require all granted
    SetEnv REXXHTTP_BASE /opt/rexxhttp-matrix
    SetEnv REXXHTTP_URLBASE /ali
    DirectoryIndex index.mtx
</Directory>
'@
[IO.File]::WriteAllText("$Apache\conf\suite.conf", $conf + "`n# The RexxHTTP suite's montage`n" + $montage)

if ($Handler -eq 'orx') {
    Copy-Item "$repo\build\oorexx-host.exe" "$Apache\bin\oorexx-host.exe" -Force
    Copy-Item "$repo\build\mod_orx.so" "$Apache\modules\mod_orx.so" -Force
}

# --- Run ------------------------------------------------------------------------
$env:PATH = "$OoRexx;$env:PATH"
$log = "$Apache\logs\suite-error.log"
& "$Apache\bin\httpd.exe" -t -f "$Apache\conf\suite.conf"
if ($LASTEXITCODE -ne 0) { "RESULT $Handler configuration rejected"; exit 1 }
$httpd = Start-Process "$Apache\bin\httpd.exe" -ArgumentList '-f', "$Apache\conf\suite.conf" `
           -NoNewWindow -PassThru
Start-Sleep 4
$smoke = & curl.exe -s -m 30 -o NUL -w '%{http_code}' http://127.0.0.1/test/hola
"SMOKE $Handler /test/hola: $smoke"
Clear-Content $log -ErrorAction SilentlyContinue      # count only the suite

# PROBE (for now): the suite's client keeps its files in /tmp, which on
# Windows is \tmp on the current drive, and deletes them with rm.
New-Item -ItemType Directory -Force '\tmp' | Out-Null
"PROBE rm: $((Get-Command rm.exe -ErrorAction SilentlyContinue).Source)"
Push-Location "$RexxHTTP\tests"
$env:PATH = "$RexxHTTP\tests\framework;$RexxHTTP\tests\http;$env:PATH"
$sw = [Diagnostics.Stopwatch]::StartNew()
& "$OoRexx\rexx.exe" http\RexxHTTP.testGroup *> $Out
Pop-Location
"SUITE $Handler ran in $([Math]::Round($sw.Elapsed.TotalSeconds)) s"

& taskkill.exe /F /T /PID $httpd.Id 2>&1 | Out-Null
Get-Process httpd, oorexx-host -ErrorAction SilentlyContinue | Stop-Process -Force

$text = Get-Content -Raw $Out
$counts = ($text -split "`r?`n" | Where-Object { $_ -match '^(Tests ran|Assertions|Failures|Errors|Skipped files|Messages|Logged messages):' }) -join '; '
"COUNTS $Handler $counts"
$lines = @(Get-Content $log -ErrorAction SilentlyContinue)
"ERRORLOG $Handler $($lines.Count) line(s)"
$lines | Select-Object -First 15 | ForEach-Object { "  log: $_" }
$ran = $text -match 'Tests ran:'
$bad = $text -match '(?m)^(Failures|Errors): +[1-9]'
if ($ran -and -not $bad -and $lines.Count -eq 0) { "RESULT $Handler green"; exit 0 }
"RESULT $Handler not green"
exit 1
