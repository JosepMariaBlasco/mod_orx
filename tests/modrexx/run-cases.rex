#!/usr/bin/env rexx
/* run-cases.rex PORT CASES PHLOG [HANDLER [DOCROOT [EXPECTED [LOOSE]]]]  */
/*                                                                        */
/* Run every case of a cases file against 127.0.0.1:PORT and print, for   */
/* each one, what came back: status, the headers that matter, the start   */
/* of the body, the lines the phase programs wrote to PHLOG, and the new  */
/* lines of the error log (without their date, process and client). A     */
/* Date or Server header in the body (a HEAD shown with curl -I) is left  */
/* out. The DOCTYPE of Apache's own error pages is cut short (it is not   */
/* the same in every 2.4), and a request curl could not finish says so.   */
/* HANDLER (rexx-handler by default) is printed as HANDLER. The same      */
/* output from two servers means the same behaviour. In the cases, @H@ is */
/* HANDLER and @D@ is DOCROOT.                                            */
/*                                                                        */
/* With EXPECTED, a file with that output, nothing of it is printed: the  */
/* cases are compared with the file's, the ones that differ are shown,    */
/* and the exit status is how many did. LOOSE is a list of cases, in one  */
/* argument, of which only the phase programs' lines are compared.        */
/*                                                                        */
/* The error log is ORX_ERROR_LOG (by default, Debian's). ORX_PATH_AS,    */
/* "FROM=TO", writes TO for FROM (whatever its case) in what is printed:  */
/* for a server whose files are not where the expected ones were          */
/* measured. Needs curl. Used by tests/modrexx-phases.sh and, on Windows, */
/* by tests/modrexx.ps1.                                                  */
/*                                                                        */
/* Copyright 2026 Josep Maria Blasco -- Apache License 2.0                */

args = .sysCArgs
if args~items < 3 then do
  say 'usage: run-cases.rex PORT CASES PHLOG [HANDLER [DOCROOT [EXPECTED [LOOSE]]]]'
  exit 99
end
port = args[1]; cases = args[2]; phlog = args[3]
handler = 'rexx-handler';  if args~items >= 4 then handler = args[4]
docroot = '/var/www/html'; if args~items >= 5 then docroot = args[5]
expected = '';             if args~items >= 6 then expected = args[6]
loose = '';                if args~items >= 7 then loose = args[7]
log = value('ORX_ERROR_LOG', , 'ENVIRONMENT')
if log == '' then log = '/var/log/apache2/error.log'
parse value value('ORX_PATH_AS', , 'ENVIRONMENT') with pathFrom '=' pathTo
.local~tmp = tempName('orx-cases')

out = .array~new
do c over linesOf(readFile(cases))
  if c~strip == '' | c~left(1) == '#' then iterate
  parse var c name path opts
  call writeFile phlog, ''
  before = linesOf(readFile(log))~items
  opts = opts~strip~changeStr('@H@', handler)~changeStr('@D@', docroot)
  r = curl(opts, 'http://127.0.0.1:'port || path, 10)
  call SysSleep 0.3                 -- the logging phase runs after the response
  out~append('==' name)
  if r~rc \== 0 then out~append('curl: error' r~rc)
  if r~headers~items > 0 then out~append(r~headers[1])
  shown = .array~new
  do h over r~headers
    parse lower var h key ':'
    if 'content-type location www-authenticate'~wordPos(key) == 0, -
       \(key~startsWith('x-out-') & key~substr(7)~verify('abcdefghijklmnopqrstuvwxyz') == 0) then iterate
    p = h~pos(':'port'/')
    if p > 0 then h = h~left(p - 1)':PORT/'h~substr(p + port~length + 2)
    shown~append(h)
  end
  out~appendAll(shown~sort)
  n = 0
  do l over linesOf(r~body)
    if l~startsWith('Date:') | l~startsWith('Server:') then iterate
    n += 1; if n > 3 then leave
    l = norm(l)~left(min(100, norm(l)~length))
    if l~startsWith('<!DOCTYPE HTML PUBLIC ') then l = '<!DOCTYPE HTML PUBLIC ...>'
    out~append('  body:' l)
  end
  do l over linesOf(readFile(phlog))
    out~append('  phase:' norm(l))
  end
  do l over newLog(log, before)
    if l~pos('AH00094') > 0 | l~pos('AH00489') > 0 | l~pos('AH00493') > 0 then iterate
    -- "[date] [module:level] [pid] rest" -> "[module:level] rest"
    parse var l '[' date '] [' level '] [' pid '] ' rest
    if l~left(1) == '[', date~pos(']') == 0, level~pos(']') == 0, pid~pos(']') == 0, -
       l~pos('] ['level'] ['pid'] ') > 0 then l = '['level']' rest
    p = l~pos('[client ')
    if p > 0 then do
      q = l~pos('] ', p)
      if q > 0 then l = l~left(p - 1) || l~substr(q + 2)
    end
    out~append('  log:' norm(l))
  end
end
call SysFileDelete .tmp'.h'
call SysFileDelete .tmp'.b'
do i = 1 to out~items
  out[i] = out[i]~changeStr(handler, 'HANDLER')
end

if expected == '' then do
  do l over out
    say l
  end
  exit 0
end

/* Compare, case by case.                                                 */
got = byCase(out, loose)
want = byCase(linesOf(readFile(expected)), loose)
differ = 0
names = .set~new
do name over want~allIndexes
  names~put(name)
end
do name over got~allIndexes
  names~put(name)
end
do name over names~makeArray~sort
  g = got[name];  if g == .nil then g = .array~of('(no such case)')
  w = want[name]; if w == .nil then w = .array~of('(no such case)')
  if g~items == w~items then do
    same = 1
    do i = 1 to g~items while same
      same = g[i] == w[i]
    end
    if same then iterate
  end
  differ += 1
  if differ > 8 then iterate
  say '  case' name 'differs:'
  do l over w
    say '    expected:' l~left(min(300, l~length))
  end
  do l over g
    say '    got:     ' l~left(min(300, l~length))
  end
end
if differ == 0 then say '  ok    every case as expected ('got~items')'
else say '  FAIL  the cases:' differ 'of' got~items 'differ'
exit differ

norm: procedure expose pathFrom pathTo
  if pathFrom == '' then return arg(1)
  text = arg(1)
  p = text~caselessPos(pathFrom)
  do while p > 0
    text = text~left(p - 1) || pathTo || text~substr(p + pathFrom~length)
    p = text~caselessPos(pathFrom, p + pathTo~length)
  end
  return text

/* The output as a table: the lines of each case, by its name. Of a loose */
/* case, only the phase programs' lines.                                  */
::routine byCase
  use strict arg lines, loose
  t = .table~new
  name = ''
  do l over lines
    if l~startsWith('== ') then do
      name = l~substr(4)
      t[name] = .array~new
      iterate
    end
    if name == '' then iterate
    if loose~wordPos(name) > 0, \l~startsWith('  phase: ') then iterate
    t[name]~append(l)
  end
  return t

::requires 'run-lib.cls'
