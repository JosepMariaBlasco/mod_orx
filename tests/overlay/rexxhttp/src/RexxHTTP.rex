#!/usr/bin/env rexx
/******************************************************************************/
/*                                                                            */
/* RexxHTTP.rex - A ooRexx rexxlet processor                                  */
/* =========================================                                  */
/*                                                                            */
/* This file is part of the RexxHTTP package                                  */
/* [See https://rexx.epbcn.com/rexxhttp/]                                     */
/*                                                                            */
/* Copyright (c) 2006-2026 Josep Maria Blasco <josep.maria.blasco@epbcn.com>  */
/*                                                                            */
/* License: Apache License 2.0 (https://www.apache.org/licenses/LICENSE-2.0)  */
/*                                                                            */
/* Version history:                                                           */
/*                                                                            */
/* Date     Version Details                                                   */
/* -------- ------- --------------------------------------------------------- */
/* 20061102    0.1  First version                                             */
/* 20170301    0.2  Drop support for mod_rexx, OS/2, IIS, rsp compilers       */
/*                  Change the way request~message works -- see 'unknown'.    */
/*                  By default, it looks in the environment variable pool.    */
/* 20260611    1.0  Global refactor, switch to Apache license                 */
/*                                                                            */
/******************************************************************************/

  Signal on syntax

  -- If RexxHTTP.rex is called directly, simulate a 404 Not Found error
  If Direct() Then Signal NotFound

  REXX_PATH = Env("REDIRECT_REXX_PATH")
  Call Value "REXX_PATH", REXX_PATH, "ENVIRONMENT"

  -- Create the request and response objects
 .local~request  = .HTTP.Request~new()
 .local~response = .HTTP.Response~new()
  -- The response object creates a buffered output stream
  bufferedOutput       = .response~output

  -- The rexxlet we have to call has been computed by the request object.
  -- SCRIPT_FILENAME is the processor's disk path -- the file to execute --
  -- whereas FILENAME/PATH_TRANSLATED now name the requested document.
  callee   = .request~script_filename

  -- Apply the argument policy BEFORE running the rexxlet. validate parses
  -- the request arguments eagerly (consuming any POST body) and reports
  -- the first violation. We reject here, while output is still untouched,
  -- so the error page is clean. "" means the request is acceptable.
  violation = .request~validate
  If violation \== "" Then Do
    Parse var violation code":"detail
    If code == "badpolicy" Then Signal ConfigError   -- operator error: 500
    Else                        Signal BadRequest    -- client error:   400
  End

  -- Set the buffered output stream as the default output stream.
  -- NOTE: .output~destination is a STACK, not a setter -- with an argument it
  -- PUSHES a new destination, without one it POPS back to the previous. The
  -- return value is NOT the previous destination (it is the one just pushed),
  -- so there is nothing worth keeping.
  --
  -- We keep NO bookkeeping flag for "did we push?". The single source of truth
  -- is the stack itself: .output~current is the destination on top, and it is
  -- our buffer if and only if it isA HTTP.OutputStream. Both this normal path
  -- and the Syntax handler decide whether to pop by asking that question --
  -- so error(), which pops on its own, can never desynchronise a flag from
  -- the real stack state (the v97 double-pop bug). current is a READ, not a
  -- pop, and on an un-redirected stack it returns STDOUT (a .Stream), never
  -- .NIL, so the test is safe to ask in any state.
 .output~destination(bufferedOutput)
    -- Call our rexxlet!
    Call (callee) .request, .response, bufferedOutput
  -- Restore the previous output stream, but only if our buffer is still on
  -- top: a rexxlet that called error()/404() already popped it, and popping
  -- again would empty the stack and leave .output on .NIL.
  If .output~current~isA(.HTTP.OutputStream) Then .output~destination

  -- Flush the response. This will write the headers if necessary
 .response~flush

  -- We're done!
  Exit

--------------------------------------------------------------------------------

-- Direct: is RexxHTTP.rex being invoked DIRECTLY by URL, rather than reached
-- through the Action that dispatches a rexxlet to us? A direct hit has no
-- document to run, so it is a 404.
--
-- The discriminator is REDIRECT_HANDLER: Apache sets it (to the Action's
-- handler name) on every Action dispatch -- one hop or two -- and leaves it
-- empty on a direct URL hit. Measured across the direct, one-hop and two-hop
-- topologies. The old test searched REQUEST_URI for the substring "RexxHTTP.rex"
-- unanchored, which false-positived on any request whose QUERY STRING merely
-- mentioned the name (e.g. ?doc=RexxHTTP.rex returned a spurious 404 over a
-- valid rexxlet). We do not care about the handler's NAME, only whether ANY
-- Action redirect brought us here, so an emptiness test is exactly right and
-- is immune to the query string. See ref-decisiones.md "v99 -- Direct()".
Direct: Return Env("REDIRECT_HANDLER") == ""

--------------------------------------------------------------------------------
-- Env: Return the value of an environment variable                           --
--------------------------------------------------------------------------------

Env: Return Value(Arg(1),,"ENVIRONMENT")

--------------------------------------------------------------------------------
-- NotFound: Return 404 Not found when RexxHTTP.rex is called directly        --
--------------------------------------------------------------------------------

NotFound:
  Say "Content-Type: text/html"
  Say "Status: 404 Not Found"
  Say ""
  Say '<!DOCTYPE HTML PUBLIC "-//IETF//DTD HTML 2.0//EN">'
  Say '<HTML><HEAD>'
  Say '<TITLE>404 Not Found</TITLE>'
  Say '</HEAD><BODY>'
  Say '<H1>Not Found</H1>'
  Say '<P>The requested URL' Env(request_uri) 'was not found on this server.</P>'
  Say '<HR>'
  Say Env(server_signature)
  Say '</BODY></HTML>'
Exit

--------------------------------------------------------------------------------
-- BadRequest: Return 400 when the request arguments violate the policy       --
--   (a parameter without "=", an empty name, a repeated name, or a name      --
--   beginning with a digit). "detail" and "code" are set by the Parse of     --
--   the validate result.                                                     --
--------------------------------------------------------------------------------

BadRequest:
  Say "Content-Type: text/html"
  Say "Status: 400 Bad Request"
  Say ""
  Say '<!DOCTYPE HTML PUBLIC "-//IETF//DTD HTML 2.0//EN">'
  Say '<HTML><HEAD>'
  Say '<TITLE>400 Bad Request</TITLE>'
  Say '</HEAD><BODY>'
  Say '<H1>Bad Request</H1>'
  Say '<P>The query string is malformed:' Reason(code, detail)'.</P>'
  Say '<HR>'
  Say Env(server_signature)
  Say '</BODY></HTML>'
Exit

--------------------------------------------------------------------------------
-- ConfigError: Return 500 when REXXHTTP_ARGPOLICY is set to an unknown value --
--   This is an operator configuration error, not a client error.            --
--------------------------------------------------------------------------------

ConfigError:
  Say "Content-Type: text/html"
  Say "Status: 500 Internal Server Error"
  Say ""
  Say '<!DOCTYPE HTML PUBLIC "-//IETF//DTD HTML 2.0//EN">'
  Say '<HTML><HEAD>'
  Say '<TITLE>500 Internal Server Error</TITLE>'
  Say '</HEAD><BODY>'
  Say '<H1>Internal Server Error</H1>'
  Say '<P>Server misconfiguration: unknown REXXHTTP_ARGPOLICY value' -
      '"'detail'".</P>'
  Say '<HR>'
  Say Env(server_signature)
  Say '</BODY></HTML>'
Exit

--------------------------------------------------------------------------------
-- Reason: Turn a validate violation code into a human-readable phrase        --
--------------------------------------------------------------------------------

Reason: Procedure
  Use Strict Arg code, detail
  Select
    When code == "novalue"   Then Return "parameter without a value:" detail
    When code == "noname"    Then Return "parameter with an empty name:" detail
    When code == "duplicate" Then Return "duplicate parameter name:" detail
    When code == "digitname" Then Return "parameter name begins with a digit:" detail
    Otherwise                     Return "invalid parameter" detail
  End

--------------------------------------------------------------------------------
-- Syntax error handler                                                       --
--------------------------------------------------------------------------------

Syntax:

  error     = Condition("O")

  Signal off syntax                               -- Avoid loops

  -- Pop the buffered stream off .output's destination stack, but only if it
  -- is actually on top. Popping an empty stack leaves .output on .NIL, and
  -- .NIL does not understand SAY -- the error page below would die with a
  -- 97.1 instead of reaching the client. We ask the stack itself
  -- (.output~current isA HTTP.OutputStream) rather than trusting a flag, so
  -- a rexxlet that already popped -- e.g. via error() -- is handled correctly:
  -- current is then STDOUT, the test is false, and we do NOT pop again. This
  -- is the fix for the v97 double-pop (error() + this handler both popping).
  -- The pop call takes NO argument on purpose: that is what asks for a pop
  -- (an argument would push again). current is a read, never a pop.
  If .output~current~isA(.HTTP.OutputStream) Then .output~destination

  If error~code == 98.900 Then Do
    -- 98.900 is our internal signal: a configuration condition a method
    -- deliberately raised (see rawResourcePath) to surface as a clean 500
    -- page rather than a stack trace. Honour the response's state the same
    -- three ways the generic path below does. Today rawResourcePath raises
    -- this while .request is still being built, so .response does not exist
    -- yet and we take the Else (Say straight to the CGI stream, as the
    -- deployed version already does). The .response arms are hardening for
    -- any future use of the channel from a point where a response is live.
    reason = error~additional[1]
    If .response~isA(.HTTP.Response) Then Do
      -- A live, untouched response: let error() reset the body, set the
      -- 500 status/type and HTML-escape the reason, then commit so the
      -- headers and page actually go out. If it is already committed the
      -- headers are on the wire; don't try to re-emit -- the condition is
      -- still in error~additional for the log via the generic path is not
      -- reached, so we simply stop without corrupting the stream.
      If \.response~committed, \.response~committing Then Do
       .response~error(500, reason)
      End
    End
    Else Do
      Say "Status: 500 Internal Server Error"
      Say "Content-type: text/html; charset=utf-8"
      Say
      Say reason
    End
    Exit
  End

  -- Commit the response. This will emit the page headers.
  --
  -- .response only exists once RexxHTTP.rex has built it (see the main
  -- flow). A syntax error raised BEFORE that -- e.g. inside
  -- HTTP.Request~new() -- reaches this handler with .response still
  -- unset, so .response is the literal string "RESPONSE" (the default
  -- value of an unset .environment entry). Sending it ~committed then
  -- raises here and buries the ORIGINAL error under a misleading
  -- 'RESPONSE does not understand COMMITTED'. Guard on the real type so
  -- an early failure still surfaces its own stack trace to the client
  -- and the error log.
  If .response~isA(.HTTP.Response) Then Do
    -- Only assert the 500 status/type if nothing has gone out yet: once
    -- the response is committed (or mid-commit), the headers are already
    -- on the wire and setting them now would raise (committed) or corrupt
    -- a half-written response (committing). commit itself is idempotent.
    --
    -- KNOWN AND ACCEPTED: if the rexxlet called ~commit itself and THEN
    -- failed, the client gets the status that commit already emitted --
    -- typically "200 OK" -- with this error page as the body, and the
    -- body the rexxlet had buffered before the failure is lost (commit
    -- writes the headers straight to the underlying stream; only ~flush
    -- drains the body buffer, and we Exit before reaching it).
    -- Not worth fixing: the status cannot be recalled once it is on the
    -- wire, so a ~flush here would not make the response truthful -- it
    -- would only append the partial body ahead of the trace, at the cost
    -- of more code on the most delicate path in the processor. The error
    -- and its failing line DO reach the client, so nothing fails silently,
    -- and a rexxlet that commits by hand has already opted out of the
    -- buffered model. See ref-decisiones.md (v95).
    If \.response~committed, \.response~committing Then Do
     .response~content_type = "text/plain; charset=utf-8"
     .response~status       = "500 Internal Server Error"
    End
   .response~commit
  End
  Else Do
    -- No response object: the failure predates its creation. Emit a
    -- minimal CGI 500 by hand. .output was never redirected to the
    -- response buffer (that happens later in the main flow), so Say
    -- writes straight to STDOUT, which is exactly the CGI response.
    Say "Status: 500 Internal Server Error"
    Say "Content-Type: text/plain; charset=utf-8"
    Say ""
  End

  -- Output an error page
  Say "Internal server error"
  Say "---------------------"
  Say

  -- Extract program name and sanitize it to avoid showing
  -- too much details about our internals
  document_root = Env(document_root)
  program_name  = error~program
  -- On Windows the program's name carries the native separator and
  -- DOCUMENT_ROOT Apache's "/", and the file system ignores case: compare
  -- them in one form (otherwise the full path reached the client). Shown
  -- with "/", as the URL that named it.
  If .File~separator == "\" Then Do
    program_name = program_name~translate("/", "\")
    If program_name~caselessStartsWith(document_root~translate("/", "\")) Then
      program_name = SubStr(program_name, Length(document_root) + 1)
  End
  Else If program_name~startsWith(document_root) Then
    program_name = SubStr(program_name, Length(document_root) + 1)

  -- Print an error message that is as similar as possible
  -- to a standard ooRexx error message
  Do frame Over error~stackFrames
    Say frame~makeString
  End
  Say "Error" error~rc "running" program_name "line" -
    error~position":" error~errortext
  Say "Error" error~code":" error~message

Exit

--------------------------------------------------------------------------------
-- Load our dependencies                                                      --
--------------------------------------------------------------------------------

::Requires "HTTP.Request.cls"
::Requires "HTTP.Response.cls"
::Requires "HTTP.OutputStream.cls"
::Requires "HTTP.Cookie.cls"
