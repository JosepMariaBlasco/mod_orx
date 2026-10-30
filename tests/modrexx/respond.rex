/* respond.rex -- how a mod_rexx program's response comes out. The query  */
/* says what to do: ct=type (WWWSendHTTPHeader), say=n (n lines of        */
/* output), ret=value (what the program returns; nothing if absent),    */
/* err=1 (a syntax error after the output), status=n (WWWReqRecStatus),  */
/* redirect=uri (WWWInternal_Redirect).                                   */
r = arg(1)
call WWWGetArgs r
q. = ''
do i = 1 to wwwargs.0
  k = translate(wwwargs.i.!name); q.k = wwwargs.i.!value
end
if q.ct \== '' then call WWWSendHTTPHeader r, q.ct
if q.status \== '' then call WWWReqRecStatus r, q.status
n = q.say; if n == '' then n = 0
do i = 1 to n
  say 'line' i
end
if q.redirect \== '' then do
  call WWWInternal_Redirect r, q.redirect
  say 'after the redirect'
end
if q.err == 1 then x = 1 + 'a'
if q.ret == '' then return
return q.ret
