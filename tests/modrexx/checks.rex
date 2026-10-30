/* checks.rex -- what mod_orx does where mod_rexx had no answer: a handle  */
/* that is not one (mod_rexx crashed the worker), a function not yet      */
/* there, a POST that is not urlencoded (mod_rexx left WWWARGS.0 unset),   */
/* and output through .output and lineout (mod_rexx had only Say).        */
r = arg(1)
call WWWSendHTTPHeader r, 'text/plain'
s = WWWReqRecServer(r); c = WWWReqRecConnection(r)
call try 'WWWReqRecUri()'
call try 'WWWReqRecUri("0x1234")'
call try 'WWWReqRecUri(s)'
call try 'WWWSrvRecPort(r)'
call try 'WWWSrvRecPort(s)'
call try 'WWWCnxRecAborted(s)'
call try 'WWWCnxRecAborted(c)'
call try 'WWWReqRecUri(r,)'
call try 'WWWReqRecNext(r)'
call try 'WWWNoSuchThing(r)'
say 'handles:' (r \== s & s \== c) left(r, 4) length(r)
call WWWGetArgs r
say 'WWWARGS.0='wwwargs.0 'POST_STRING=['wwwpost_string']'
call WWWLogReason r, 'the reason', '/the/file'
.output~say('via .output')
call lineout , 'via lineout'
return 0
try:
  signal on syntax
  interpret 'x =' arg(1)
  say arg(1) '-> ['||x||']'
  return
syntax:
  say arg(1) '-> Error' condition('O')~code
  return
