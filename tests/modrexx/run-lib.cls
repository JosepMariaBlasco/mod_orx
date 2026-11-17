/* run-lib.cls -- what run-checks.rex and run-cases.rex share: a request  */
/* with curl, and files as lines.                                         */
/*                                                                        */
/* Copyright 2026 Josep Maria Blasco -- Apache License 2.0                */

/* curl(options, url): the request, with curl's options as they would be  */
/* typed (arguments in double quotes: they go through sh or cmd). Gives   */
/* a directory: rc (curl's), status (the number, "" without a response),  */
/* statusLine, headers (an array of lines), contentType ("" if none) and  */
/* body (the bytes). .tmp is the stem of the two files it uses.           */
::routine curl public
  use strict arg options, url, seconds = 30
  h = .tmp'.h'; b = .tmp'.b'
  call writeFile h, ''
  call writeFile b, ''
  q = '"'                           -- (a quote before h or b would be a hex or binary string)
  address system 'curl -s -m' seconds '-D' q || h || q '-o' q || b || q options q || url || q
  r = .directory~new
  r~rc = rc
  r~headers = linesOf(readFile(h))
  r~body = readFile(b)
  r~statusLine = ''; r~status = ''; r~contentType = ''
  if r~headers~items > 0 then do
    r~statusLine = r~headers[1]
    r~status = r~statusLine~word(2)
  end
  do l over r~headers
    if l~caselessStartsWith('Content-Type:') then r~contentType = l~substr(14)~strip
  end
  return r

/* The whole file, as it is ("" if it is not there). Shared: Apache has   */
/* its error log open.                                                    */
::routine readFile public
  use strict arg name
  s = .stream~new(name)
  if s~open('read shared') \== 'READY:' then return ''
  data = s~charIn(1, s~chars)
  s~close
  return data

::routine writeFile public
  use strict arg name, data
  s = .stream~new(name)
  s~open('write replace')
  if data \== '' then s~charOut(data)
  s~close
  return

/* Text as lines, without their ends (LF or CRLF). A last line with no    */
/* end counts; an empty text has no lines.                                */
::routine linesOf public
  use strict arg text
  if text == '' then return .array~new
  if text~right(1) == '0a'x then text = text~left(text~length - 1)
  lines = .array~new
  do while text \== ''
    p = text~pos('0a'x)
    if p == 0 then do
      lines~append(text~changeStr('0d'x, ''))
      leave
    end
    lines~append(text~left(p - 1)~changeStr('0d'x, ''))
    text = text~substr(p + 1)
    if text == '' then lines~append('')
  end
  if lines~items == 0 then lines~append('')
  return lines

/* An expected file, for this server.                                     */
::routine expected public
  use strict arg name, url, doc, port, hp
  return readFile(name)~changeStr('@URL@', url)~changeStr('@DOC@', doc) -
                       ~changeStr('@PORT@', port)~changeStr('@HP@', hp)

/* How many lines have the text: anywhere, at their start ("start") or as */
/* the whole line ("exact").                                              */
::routine count public
  use strict arg lines, text, how = 'in'
  n = 0
  do l over lines
    select case how
      when 'exact' then if l == text then n += 1
      when 'start' then if l~startsWith(text) then n += 1
      otherwise if l~pos(text) > 0 then n += 1
    end
  end
  return n

/* The first lines (all of them by default), each followed by sep.        */
::routine joined public
  use strict arg lines, first = (lines~items), sep = '|'
  text = ''
  do n = 1 to min(first, lines~items)
    text ||= lines[n] || sep
  end
  return text

/* The error log's lines after its first "before" ones.                   */
::routine newLog public
  use strict arg log, before
  all = linesOf(readFile(log))
  if all~items <= before then return .array~new
  return all~section(before + 1)

/* The stem of a temporary file of this process.                          */
::routine tempName public
  use strict arg prefix
  dir = value('TEMP', , 'ENVIRONMENT')
  if dir == '' then dir = value('TMPDIR', , 'ENVIRONMENT')
  if dir == '' then dir = '/tmp'
  return dir~strip('T', '/')~strip('T', '\') || .File~separator || prefix'-'SysQueryProcess('PID')
