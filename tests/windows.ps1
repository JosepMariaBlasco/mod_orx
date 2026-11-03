# mod_orx on Windows: Apache (mpm_winnt) with mod_orx and its host, serving
# Rexx CGI programs. Checks what the port changed: the transport (AF_UNIX,
# accept from several processes), the program's output (STDOUT on a pipe that
# lasts the child's life: exact bytes, the end of each phase, flushes), the
# body, the environment and the directory, errors and timeouts, the pool
# (several processes), the host's restart by the module, its draining, and
# the job that ends it with Apache.
#
#   tests\windows.ps1 -Apache C:\Apache24 -OoRexx C:\oorexx
#
# Built first with build.cmd. Prints one line per check, PASS or FAIL, and
# exits with the number of failures.
#
# Copyright 2026 Josep Maria Blasco -- Apache License 2.0

param([Parameter(Mandatory)][string]$Apache, [Parameter(Mandatory)][string]$OoRexx,
      [int]$Port = 8096)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$fails = 0
$trace = @()
function check($name, $ok, $detail = '') {
    if ($ok) { "PASS $name" } else { "FAIL $name $detail"; $script:fails++ }
}
function hosts { @(Get-CimInstance Win32_Process -Filter "Name = 'oorexx-host.exe'") }
function pool { (hosts | ForEach-Object { "$($_.ProcessId)<$($_.ParentProcessId)" }) -join ' ' }
# Every request leaves a line: status, time, and the host processes after it.
function get($path, $method = 'GET', $body = $null) {
    $a = @('-s', '-m', '30', '-o', '-', '-w', '\n%{http_code}', "http://127.0.0.1:$Port$path")
    if ($body -ne $null) { $a += @('--data-binary', $body) }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $r = (& curl.exe @a) -join "`n"
    $i = $r.LastIndexOf("`n")
    $res = @{ code = $r.Substring($i + 1); body = $r.Substring(0, [Math]::Max($i, 0)).TrimEnd("`r", "`n") }
    $script:trace += "  $($res.code) $path $([Math]::Round($sw.Elapsed.TotalSeconds, 2)) s; hosts: $(pool)"
    return $res
}

# --- Deploy ---------------------------------------------------------------
Copy-Item "$repo\build\oorexx-host.exe" "$Apache\bin\oorexx-host.exe" -Force
Copy-Item "$repo\build\mod_orx.so" "$Apache\modules\mod_orx.so" -Force
$root = $Apache -replace '\\', '/'
$cgi = "$Apache\htdocs\rexx"
New-Item -ItemType Directory -Force $cgi | Out-Null

# The programs. Bytes that a text-mode stream would change: NUL, ^Z, CR, FF.
Set-Content -Encoding ascii "$cgi\hello.rex" @'
say 'Content-Type: text/plain'
say ''
say 'hello' value('QUERY_STRING',,'ENVIRONMENT') value('REQUEST_METHOD',,'ENVIRONMENT')
'@
Set-Content -Encoding ascii "$cgi\bytes.rex" @'
call charout , 'Content-Type: application/octet-stream' || '0d0a0d0a'x
call charout , '00'x || '1a'x || '0d'x || 'ff'x || 'x' || '0a'x || '0d0a'x
'@
Set-Content -Encoding ascii "$cgi\big.rex" @'
say 'Content-Type: text/plain'
say ''
line = copies('0123456789', 9) || 'abcdefghi'
do i = 1 to 3000
  call charout , line || '0a'x
end
'@
Set-Content -Encoding ascii "$cgi\post.rex" @'
say 'Content-Type: text/plain'
say ''
n = value('CONTENT_LENGTH',,'ENVIRONMENT')
data = charin(, , n)
say 'got' length(data) c2x(left(data, 4))
'@
Set-Content -Encoding ascii "$cgi\where.rex" @'
say 'Content-Type: text/plain'
say ''
say directory()
'@
Set-Content -Encoding ascii "$cgi\pid.rex" @'
call SysSleep 0.3
say 'Content-Type: text/plain'
say ''
say SysQueryProcess('PID')
'@
Set-Content -Encoding ascii "$cgi\flush.rex" @'
say 'Content-Type: text/plain'
say ''
say 'first'
.output~flush
call SysSleep 1.5
say 'second'
'@
Set-Content -Encoding ascii "$cgi\broken.rex" @'
say 'Content-Type: text/plain'
say ''
say 'before'
x = 1 +
'@
Set-Content -Encoding ascii "$cgi\loop.rex" @'
say 'Content-Type: text/plain'
say ''
do forever; nop; end
'@

@"
ServerRoot "$root"
ServerName localhost
Listen 127.0.0.1:$Port
LoadModule authn_core_module modules/mod_authn_core.so
LoadModule authz_core_module modules/mod_authz_core.so
LoadModule mime_module modules/mod_mime.so
LoadModule log_config_module modules/mod_log_config.so
LoadModule orx_module modules/mod_orx.so
ErrorLog logs/orx-error.log
LogLevel warn orx:info
PidFile logs/orx.pid
DocumentRoot "$root/htdocs"
OoRexxProcesses 2 4
<Directory "$root/htdocs/rexx">
    Options +ExecCGI
    AddHandler oorexx-cgi .rex
    Require all granted
</Directory>
<Location /rexx/loop.rex>
    OoRexxTimeout 3
</Location>
"@ | Out-File -Encoding ascii "$Apache\conf\orx.conf"

$log = "$Apache\logs\orx-error.log"
Remove-Item $log -ErrorAction SilentlyContinue
# The host finds rexx.dll on the PATH it inherits from Apache.
$env:PATH = "$OoRexx;$env:PATH"
& "$Apache\bin\httpd.exe" -t -f "$Apache\conf\orx.conf"
check 'configuration' ($LASTEXITCODE -eq 0)
$httpd = Start-Process "$Apache\bin\httpd.exe" -ArgumentList '-f', "$Apache\conf\orx.conf" `
           -NoNewWindow -PassThru
Start-Sleep 4

# --- The pool ---------------------------------------------------------------
$trace += "  started: httpd $($httpd.Id); hosts: $(pool)"
$h = hosts
$master = $h | Where-Object { $_.ParentProcessId -eq $httpd.Id }
$children = $h | Where-Object { $master -and $_.ParentProcessId -eq $master.ProcessId }
check 'one master, started by Apache''s parent' (@($master).Count -eq 1) "($($h.Count) host processes)"
check 'the pool has its minimum, 2 children' (@($children).Count -eq 2) "(found $(@($children).Count))"
check 'the socket is there' (Test-Path "$Apache\logs\oorexx.sock")
$acl = (icacls "$Apache\logs\oorexx.sock") -join ' '
check 'the socket''s ACL is restricted' ($acl -notmatch 'Authenticated Users|BUILTIN\\Users|Everyone') $acl

# --- Output, environment, body, directory ------------------------------------
$r = get '/rexx/hello.rex?a=1'
check 'a CGI program runs' ($r.code -eq '200' -and $r.body -eq 'hello a=1 GET') "[$($r.code)] [$($r.body)]"

$tmp = "$env:TEMP\orx-bytes.bin"
& curl.exe -s -m 30 -o $tmp "http://127.0.0.1:$Port/rexx/bytes.rex"
$b = [IO.File]::ReadAllBytes($tmp)
$want = [byte[]](0, 0x1a, 0x0d, 0xff, 0x78, 0x0a, 0x0d, 0x0a)
check 'output bytes arrive exactly' ($b.Length -eq 8 -and -not (Compare-Object $b $want -SyncWindow 0)) `
      "($(($b | ForEach-Object { $_.ToString('x2') }) -join ' '))"

& curl.exe -s -m 30 -o $tmp "http://127.0.0.1:$Port/rexx/big.rex"
$len = (Get-Item $tmp).Length
check 'a 300 KB response, whole (end of output)' ($len -eq 300000) "($len bytes)"
$r = get '/rexx/hello.rex?after=big'
check 'the next phase on that process starts clean' ($r.body -eq 'hello after=big GET') "[$($r.body)]"

$payload = 'ABCD' + ('z' * 70000)
Set-Content -NoNewline -Encoding ascii "$env:TEMP\orx-post.txt" $payload
$r = get '/rexx/post.rex' 'POST' "@$env:TEMP\orx-post.txt"
check 'the request body (70 KB)' ($r.body -eq 'got 70004 41424344') "[$($r.body)]"

$r = get '/rexx/where.rex'
check 'runs in its directory' (($r.body -replace '\\', '/') -eq "$root/htdocs/rexx") "[$($r.body)]"

# An explicit flush reaches the client early: time to the first byte.
$t = & curl.exe -s -m 30 -o NUL -w '%{time_starttransfer} %{time_total}' "http://127.0.0.1:$Port/rexx/flush.rex"
$first, $total = $t -split ' ' | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) }
check 'a flush reaches the client before the end' ($first -lt 1.0 -and $total -ge 1.4) "(first byte $first s, total $total s)"

# --- Errors and timeouts -----------------------------------------------------
$r = get '/rexx/broken.rex'
Start-Sleep 1
$logtext = Get-Content $log -Raw
check 'a syntax error is logged' ($logtext -match 'broken\.rex: line 4: Error 35') "[$($r.code)]"
$sw = [Diagnostics.Stopwatch]::StartNew()
$r = get '/rexx/loop.rex'
$secs = $sw.Elapsed.TotalSeconds
Start-Sleep 1
$logtext = Get-Content $log -Raw
check 'OoRexxTimeout halts a program' ($secs -lt 8 -and $logtext -match 'loop\.rex.*phase timeout') "($([int]$secs) s)"

# --- Several processes at once ------------------------------------------------
$pids = 1..8 | ForEach-Object -Parallel {
    (curl.exe -s -m 30 -w ' [%{http_code}]' "http://127.0.0.1:$using:Port/rexx/pid.rex") -join ' '
} -ThrottleLimit 8
$good = @($pids | Where-Object { $_ -match '^\d+\s+\[200\]$' })
$distinct = @($good | ForEach-Object { ($_ -split ' ')[0] } | Sort-Object -Unique)
$trace += "  8 at once: $(($pids | ForEach-Object { $_.Substring(0, [Math]::Min(20, $_.Length)) }) -join ' | '); hosts: $(pool)"
check '8 requests at once, served by several processes' ($good.Count -eq 8 -and $distinct.Count -ge 2) `
      "($($good.Count) good answers, processes: $($distinct -join ' '))"

# --- The host dies: a child, then the master ---------------------------------
$c = (hosts | Where-Object { $_.ParentProcessId -eq $master.ProcessId } | Select-Object -First 1)
Stop-Process -Id $c.ProcessId -Force
Start-Sleep 2
$r = get '/rexx/hello.rex?child=killed'
check 'a killed child is replaced' ($r.body -eq 'hello child=killed GET' -and
      @(hosts | Where-Object { $_.ParentProcessId -eq $master.ProcessId }).Count -ge 2)
Stop-Process -Id $master.ProcessId -Force
Start-Sleep 4
$m2 = hosts | Where-Object { $_.ParentProcessId -eq $httpd.Id }
$r = get '/rexx/hello.rex?master=killed'
check 'a killed master is restarted by the module' (@($m2).Count -eq 1 -and $m2.ProcessId -ne $master.ProcessId -and
      $r.body -eq 'hello master=killed GET') "[$($r.body)] (old master $($master.ProcessId), now: $(pool))"
Start-Sleep 1
$logtext = Get-Content $log -Raw
check 'the restart is logged' ($logtext -match 'oorexx-host \(pid \d+\) ended')

# --- Apache goes: the job ends the host ---------------------------------------
$before = @(hosts).Count
Stop-Process -Id $httpd.Id -Force
Get-Process httpd -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 3
$after = @(hosts).Count
check 'when Apache dies, its hosts die with it (job)' ($before -ge 3 -and $after -eq 0) "($before before, $after after)"

# --- The error log -------------------------------------------------------------
"--- requests ---"
$trace
"--- error log ---"
Get-Content $log | Select-Object -Last 40
$unexpected = Get-Content $log | Where-Object {
    $_ -match '\[(error|crit|alert|emerg)\]' -and $_ -notmatch 'broken\.rex|loop\.rex|ended \(status|ScriptAlias' }
check 'no unexpected errors in the log' ($unexpected.Count -eq 0) ($unexpected -join ' | ')

"RESULT $fails failure(s)"
exit $fails
