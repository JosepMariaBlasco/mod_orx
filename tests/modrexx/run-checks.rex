#!/usr/bin/env rexx
/* run-checks.rex BASE URL LOG [DOC [PORT]] -- the checks of the modrexx  */
/* class (tests/modrexx.sh says what they are), against an Apache that    */
/* already serves probe.rex, respond.rex and checks.rex and a static.txt  */
/* ("hello") at BASE, with rexx-handler and the two RexxSetVar of         */
/* tests/modrexx.sh. URL is BASE's path, LOG Apache's error log, DOC its  */
/* DocumentRoot as Apache names it (/var/www/html), PORT the port of BASE */
/* (80). Needs curl. Exits with the number of failures. Run by            */
/* tests/modrexx.sh and, on Windows, by tests/modrexx.ps1.                */
/*                                                                        */
/* Copyright 2026 Josep Maria Blasco -- Apache License 2.0                */

args = .sysCArgs
if args~items < 3 then do
  say 'usage: run-checks.rex BASE URL LOG [DOC [PORT]]'
  exit 99
end
base = args[1]; url = args[2]; log = args[3]
doc = '/var/www/html'; if args~items >= 4 then doc = args[4]
port = 80;             if args~items >= 5 then port = args[5]
hp = ''; if port \== 80 then hp = ':'port       -- as ap_construct_url writes it
here = filespec('location', .context~package~name)
.local~fails = 0
.local~tmp = tempName('orx-checks')
-- Only what the error log gets from here on.
log0 = linesOf(readFile(log))~items

say '== probe.rex: what a program sees, line by line, as under mod_rexx'
r = curl('-A probe/1.0 -H "Cookie: c1=v1; c2=a&b; c3=%41+"', -
         base'/probe.rex/p?a=1&name=nm&zz=Z&e=%C3%A9+x%2By&f')
got = linesOf(r~body)
want = linesOf(expected(here'probe.expected', url, doc, port, hp))
call check 'as many lines', got~items, want~items
bad = 0
do n = 1 to want~items
  g = ''; if n <= got~items then g = got[n]
  w = want[n]
  -- On Windows the time zone of WWWHTTP_time is the C library's whole
  -- name for it, of several words.
  if w~startsWith('WWWHTTP_time words='), .File~separator == '\' then w = 'WWWHTTP_time words=*'
  -- name=*: a value that depends on the machine; only the name counts.
  if w~endsWith('=*') then same = g~startsWith(w~left(w~length - 1))
  else same = g == w
  if \same then do
    call fail 'line' n": '"g"', expected '"w"'"
    bad = 1
  end
end
if bad == 0 then call ok 'every line as under mod_rexx ('want~items')'
r = curl('', base'/probe.rex')
call check 'the writes reach the response: the status', r~status, 201
do h over .array~of('X-Out: out1', 'X-Set: set1', 'X-Err: err1', 'Set-Cookie: k=v', -
                    'Content-Type: text/plain; charset=x-probe', -
                    'Content-Encoding: identity', 'Content-Language: ca,es')
  call check 'the writes reach the response:' h, count(r~headers, h, 'exact'), 1
end

say '== respond.rex: what the return value and the output make of the response'
call check 'nothing returned, nothing said', headOf(''), '200 '
call check 'WWWSendHTTPHeader and Say', -
     headOf('ct=text/plain&say=2') joined(bodyOf('ct=text/plain&say=2')), -
     '200 text/plain line 1|line 2|'
call check 'no content type set: none sent', headOf('say=1'), '200 '
call check '404 replaces what was said', -
     headOf('say=2&ret=404') count(bodyOf('say=2&ret=404'), '404 Not Found'), -
     '404 text/html; charset=iso-8859-1 1'
call check '-1 (DECLINED): the next handler adds the file itself', -
     joined(bodyOf('ct=text/plain&say=2&ret=-1'), 3), -
     'line 1|line 2|/* respond.rex -- how a mod_rexx program''s response comes out. The query  */|'
call check '-2 (DONE)', joined(bodyOf('say=1&ret=-2'), , ''), 'line 1'
do v over .array~of('abc', '1.5', '99999')
  call check v 'counts as 0', headOf('say=1&ret='v) joined(bodyOf('say=1&ret='v), , ''), -
       '200  line 1'
end
call check '301', headOf('say=1&ret=301'), '301 text/html; charset=iso-8859-1'
call check '200 returned is Apache''s error page for a 200', -
     count(bodyOf('ret=200&say=1'), '<title>200 OK</title>'), 1
call check '204', headOf('say=1&ret=204'), '204 '
call check '304', headOf('say=1&ret=304'), '304 '
call check 'a syntax error: 500, and what was said is dropped', -
     headOf('say=2&err=1') count(bodyOf('say=2&err=1'), 'line', 'start'), -
     '500 text/html; charset=iso-8859-1 0'
call check '... and the error, with its line, in the error log', -
     count(newLog(log, log0), 'respond.rex: line 22: Error 41'), 2
call check 'WWWReqRecStatus 404, with a body', -
     headOf('ct=text/plain&say=1&status=404') joined(bodyOf('ct=text/plain&say=1&status=404'), , ''), -
     '404 text/plain line 1'
call check 'WWWInternal_Redirect: the other resource, without the program''s output', -
     headOf('say=1&redirect='url'/static.txt') joined(bodyOf('say=1&redirect='url'/static.txt'), , ''), -
     '200 text/plain hello'
call check 'a long output goes as it comes: a later 500 cannot replace it', -
     headOf('say=3000&ret=500') joined(bodyOf('say=3000&ret=500'), 1, ''), '200  line 1'
r = curl('-I', base'/respond.rex?say=1&ct=text/plain')
shown = ''
do l over r~headers
  if l~startsWith('HTTP') | l~startsWith('Content-Type') then shown ||= l'|'
end
call check 'HEAD: answered without running the program, as text/html', shown, -
     'HTTP/1.1 200 OK|Content-Type: text/html|'

say '== checks.rex: what only mod_orx answers'
r = curl('-X POST -H "Content-Type: text/plain" -d raw', base'/checks.rex')
got = linesOf(r~body)
want = linesOf(expected(here'checks.expected', url, doc, port, hp))
bad = 0
do n = 1 to max(got~items, want~items)
  g = '(nothing)'; if n <= got~items then g = got[n]
  w = '(nothing)'; if n <= want~items then w = want[n]
  if g \== w then do
    call fail 'checks.rex, line' n": '"g"', expected '"w"'"
    bad = 1
  end
end
if bad == 0 then call ok 'every line as expected'
new = newLog(log, log0)
n = 0
do l over new
  p = l~pos('[orx:emerg]')
  if p > 0, l~pos('Access to /the/file failed for 127.0.0.1. reason: the reason', p) > 0 then n += 1
end
call check 'WWWLogReason: mod_rexx''s line, at its level (emerg)', n, 1
n = 0
do l over new
  if l~pos(':notice]') == 0, l~pos('respond.rex: line 22: Error 41') == 0, -
     l~pos('Access to /the/file') == 0 then n += 1
end
call check 'nothing else in the error log', n, 0

call SysFileDelete .tmp'.h'
call SysFileDelete .tmp'.b'
exit .fails

ok:
  say '  ok   ' arg(1)
  return

fail:
  say '  FAIL ' arg(1)
  .local~fails = .fails + 1
  return

check:
  if arg(2) == arg(3) then call ok arg(1)
  else call fail arg(1) "(expected '"arg(3)"', got '"arg(2)"')"
  return

-- "status content-type" of a response of respond.rex, and its body's lines.
headOf:
  hr = curl('', base'/respond.rex?'arg(1))
  return hr~status hr~contentType

bodyOf:
  return linesOf(curl('', base'/respond.rex?'arg(1))~body)

::requires 'run-lib.cls'
