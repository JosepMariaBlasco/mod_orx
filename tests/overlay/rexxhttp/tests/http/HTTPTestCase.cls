/******************************************************************************/
/*                                                                            */
/* HTTPTestCase.cls - ooTestCase subclass for RexxHTTP integration tests      */
/* ====================================================================       */
/*                                                                            */
/* This file is part of the RexxHTTP package                                  */
/* [See https://rexx.epbcn.com/rexxhttp/]                                     */
/*                                                                            */
/* Copyright (c) 2006-2026 Josep Maria Blasco <josep.maria.blasco@epbcn.com>  */
/*                                                                            */
/* License: Apache License 2.0 (https://www.apache.org/licenses/LICENSE-2.0)  */
/*                                                                            */
/* Provides HTTP helpers (curl-backed) plus body/header assertions for the    */
/* RexxHTTP rexxlet-processor integration tests. Modelled on the Rexx Parser  */
/* CGITestCase, extended to capture response headers (Set-Cookie, lazy        */
/* headers, Content-Type), to issue POST requests, and to carry a cookie jar  */
/* for round-trip tests.                                                      */
/*                                                                            */
/* Version history:                                                           */
/*                                                                            */
/* Date     Version Details                                                   */
/* -------- ------- --------------------------------------------------------- */
/* 20260611    1.0  First version                                             */
/*                                                                            */
/******************************************************************************/

::Requires "ooTest.frm"

::Class HTTPTestCase Subclass ooTestCase Public

/******************************************************************************/
/* httpGet: perform an HTTP GET via curl. Returns a Directory with:           */
/*   status  - the HTTP status code as a string ("200", "404", ...)           */
/*   body    - the response body (text), lines joined with LF                 */
/*   headers - the raw response header block (text), as returned by curl -D   */
/*   raw     - the response body exactly as received, every byte (body joins  */
/*             its lines with LF and drops the last terminator; raw does not, */
/*             whatever framing Apache chose: Content-Length or chunked)      */
/*                                                                            */
/* An optional cookie jar file may be supplied. If given, curl reads it with  */
/* -b (sends stored cookies) and writes it with -c (stores Set-Cookie), so    */
/* successive calls sharing one jar perform a real browser-style round trip.  */
/******************************************************************************/

::Method httpGet
  Use Strict Arg url, jar = ""
  Return self~curl("GET", url, "", jar)

/******************************************************************************/
/* httpPost: perform an HTTP POST via curl with a urlencoded body.            */
/* The data argument is sent verbatim as the request body with               */
/* Content-Type application/x-www-form-urlencoded (curl's --data default).    */
/******************************************************************************/

::Method httpPost
  Use Strict Arg url, data, jar = ""
  Return self~curl("POST", url, data, jar)

/******************************************************************************/
/* httpHead: perform an HTTP HEAD via curl (--head). The response carries     */
/* headers but no body, by definition; the returned Directory's "body" entry  */
/* is therefore expected to be empty. Used to assert the HEAD contract: same  */
/* headers a GET would set, no body, query string passed through.             */
/******************************************************************************/

::Method httpHead
  Use Strict Arg url, jar = ""
  Return self~curl("HEAD", url, "", jar)

/******************************************************************************/
/* curl (private): the workhorse for GET, POST and HEAD. Assembles the        */
/* options and hands them to exchange.                                        */
/******************************************************************************/

::Method curl Private
  Use Strict Arg verb, url, data, jar

  -- -s silent, --noproxy to dodge any local proxy.
  opts = "-s --noproxy 127.0.0.1"
  -- The jar is a file object (see newJar) or a path.
  If jar~isA(.File) Then jar = jar~absolutePath
  If jar \== "" Then opts = opts "-b" '"'jar'"' "-c" '"'jar'"'
  If verb == "POST" Then opts = opts "--data" '"'data'"'
  -- --head issues a real HEAD request (see exchange for where its output goes).
  If verb == "HEAD" Then opts = opts "--head"

  Return self~exchange(opts, url, verb == "HEAD")

/******************************************************************************/
/* exchange (private): run curl with the given options against url, and       */
/* return a Directory with status, headers and body (see httpGet). Status,    */
/* headers and body go to temporary files, created by the framework in the   */
/* system's temporary folder, so this runs unchanged on every platform; they  */
/* are deleted once read. Each exchange names its files afresh (see         */
/* tmpName): the framework also deletes a temporary file when its object is  */
/* collected, at no particular moment, and an earlier exchange's object must */
/* never take the current exchange's files with it.                          */
/*                                                                            */
/* In --head mode curl writes the header block to -o (the headers ARE the     */
/* output there). The response has no body by definition, so that output     */
/* goes to a file of its own, never read: the headers come from -D alone and  */
/* the body file stays unwritten, so the body assertions see emptiness, not   */
/* the header block.                                                          */
/******************************************************************************/

::Method exchange Private
  Use Strict Arg opts, url, head = .false

  id = self~class~tmpName
  tmpBody = .TemporaryTestFile~new(, "rexxhttp-test-"id"-body.tmp")
  tmpHdr  = .TemporaryTestFile~new(, "rexxhttp-test-"id"-hdr.tmp")
  tmpStat = .TemporaryTestFile~new(, "rexxhttp-test-"id"-stat.tmp")
  tmpHead = .TemporaryTestFile~new(, "rexxhttp-test-"id"-head.tmp")
  Do f Over .Array~of(tmpBody, tmpHdr, tmpStat, tmpHead)
    f~delete
  End

  bodySink = tmpBody
  If head Then bodySink = tmpHead

  -- -D dumps response headers, -o writes the body, -w prints the status code.
  Address System "curl" opts "-D" tmpHdr~quotedName "-o" bodySink~quotedName ,
    "-w %{http_code}" '"'url'"' ">" tmpStat~quotedName

  response = .Directory~new
  response~setEntry("status",  self~slurp(tmpStat~absolutePath)~strip)
  response~setEntry("headers", self~slurp(tmpHdr~absolutePath))
  response~setEntry("body",    self~slurp(tmpBody~absolutePath))
  response~setEntry("raw",     self~slurpRaw(tmpBody~absolutePath))

  Do f Over .Array~of(tmpBody, tmpHdr, tmpStat, tmpHead)
    f~delete
  End

  Return response

/******************************************************************************/
/* tmpName (class): a name no earlier exchange of this run has used: the     */
/* process id and a counter.                                                 */
/******************************************************************************/

::Method tmpName Class
  Expose serial
  If \Datatype(serial, "W") Then serial = 0
  serial += 1
  Return SysQueryProcess("PID")"-"serial

/******************************************************************************/
/* newJar: a fresh, empty cookie jar, a temporary file in the system's        */
/* temporary folder. Pass it to httpGet/httpPost and keep it in a variable    */
/* for as long as it is in use (the framework deletes it when the object      */
/* goes); dropJar deletes it at once.                                         */
/******************************************************************************/

::Method newJar
  Use Strict Arg name
  jar = .TemporaryTestFile~new(, name)
  jar~delete
  Return jar

::Method dropJar
  Use Strict Arg jar
  jar~delete

/******************************************************************************/
/* onDisk, asConfigured: an expected disk path of the montage, written as     */
/* on Linux ("/var/www/html/mtx/probe.rxl"), in the form RexxHTTP reports it  */
/* on this machine. Where the montage's paths name a drive (Windows), the     */
/* runner says which in REXXHTTP_TEST_DRIVE ("C:"); empty or unset, nothing   */
/* changes. onDisk is a path RexxHTTP resolved on disk (filename,             */
/* path_translated, script_filename): the drive, and the native separator.    */
/* asConfigured is a path as the operator wrote it in Apache's configuration  */
/* (REXXHTTP_BASE, hence RexxHTTP_DIR): the drive, and "/" as written.        */
/******************************************************************************/

::Method onDisk
  Use Strict Arg path
  Return self~asConfigured(path)~translate(.File~separator, "/")

::Method asConfigured
  Use Strict Arg path
  Return Value("REXXHTTP_TEST_DRIVE", , "ENVIRONMENT") || path

/******************************************************************************/
/* slurp (private): read an entire file into a string, lines joined with LF.  */
/* Returns "" if the file is empty or unreadable.                             */
/******************************************************************************/

::Method slurp Private
  Use Strict Arg path

  If Stream(path, "C", "Query Exists") == "" Then Return ""

  lines = .Array~new
  Call Stream path, "C", "Open Read"
  Loop While Lines(path) > 0
    lines~append(LineIn(path))
  End
  Call Stream path, "C", "Close"

  Return lines~makeString("L", "0A"x)

/******************************************************************************/
/* slurpRaw (private): read an entire file, byte for byte. Returns "" if the  */
/* file does not exist.                                                       */
/******************************************************************************/

::Method slurpRaw Private
  Use Strict Arg path

  If Stream(path, "C", "Query Exists") == "" Then Return ""

  s = .Stream~new(path)
  s~open("Read")
  data = s~charIn(1, s~chars)
  s~close

  Return data

/******************************************************************************/
/* Assertions on response text                                                */
/******************************************************************************/

::Method assertContains
  Use Strict Arg text, substring, label
  self~assertTrue(text~caselessPos(substring) > 0, label)

::Method assertNotContains
  Use Strict Arg text, substring, label
  self~assertTrue(text~caselessPos(substring) == 0, label)

/******************************************************************************/
/* assertHeader: assert that the response header block contains a header      */
/* whose line matches "name: ...value...". Both name and the value substring  */
/* are matched caselessly. Useful for Content-Type, Set-Cookie, lazy headers. */
/******************************************************************************/

::Method assertHeader
  Use Strict Arg headers, name, valueSubstring, label
  found = .False
  Do line Over headers~makeArray("0A"x)
    Parse Var line hname ":" hvalue
    If hname~strip~caselessCompare(name) == 0 Then Do
      If hvalue~caselessPos(valueSubstring) > 0 Then found = .True
    End
  End
  self~assertTrue(found, label)

/******************************************************************************/
/* assertNoHeader: assert that NO header line has the given name.             */
/******************************************************************************/

::Method assertNoHeader
  Use Strict Arg headers, name, label
  found = .False
  Do line Over headers~makeArray("0A"x)
    Parse Var line hname ":"
    If hname~strip~caselessCompare(name) == 0 Then found = .True
  End
  self~assertTrue(\found, label)

/******************************************************************************/
/* httpGetCookie: GET with a raw "Cookie:" request header (v11). A cookie jar */
/* cannot carry two cookies of the same name, but a browser sends them as a   */
/* single header "name=A; name=B"; this helper reproduces that exactly, for   */
/* the first-wins duplicate test.                                             */
/******************************************************************************/

::Method httpGetCookie
  Use Strict Arg url, cookieHeader

  opts = "-s --noproxy 127.0.0.1 -H" '"'"Cookie: "cookieHeader'"'

  Return self~exchange(opts, url)

/******************************************************************************/
/* httpPostChunked: POST with Transfer-Encoding: chunked and NO               */
/* Content-Length (v99). curl chunks the body when the explicit header is     */
/* present; Apache unwraps it and hands the reassembled body to the CGI on    */
/* stdin with CONTENT_LENGTH empty -- the case readBodyToEOF exists for.       */
/* Verbatim body, Content-Type application/x-www-form-urlencoded.             */
/******************************************************************************/

::Method httpPostChunked
  Use Strict Arg url, data

  -- --data sends the body; the explicit Transfer-Encoding header makes curl
  -- frame it as chunked and drop Content-Length. -H Expect: strips curl's
  -- automatic 100-continue so the exchange is a single round trip.
  opts = "-s --noproxy 127.0.0.1"
  opts = opts "-H" '"'"Transfer-Encoding: chunked"'"'
  opts = opts "-H" '"'"Expect:"'"'
  opts = opts "--data" '"'data'"'

  Return self~exchange(opts, url)


/******************************************************************************/
/* httpVerb: perform an arbitrary-method request (PUT, DELETE, ...) with an    */
/* optional urlencoded body (v99). -X sets the method; --data sends the body   */
/* with a fixed Content-Length, exactly like httpPost. An empty data string    */
/* sends no body. Used to exercise the PUT/DELETE argument/body contract.      */
/******************************************************************************/

::Method httpVerb
  Use Strict Arg verb, url, data = ""

  opts = "-s --noproxy 127.0.0.1 -X" verb
  If data \== "" Then opts = opts "--data" '"'data'"'

  Return self~exchange(opts, url)

