/* phase.rex -- a mod_rexx program for any phase. It is installed under  */
/* several names (Trans.rex, Fixup.rex...) and its name is its tag. It   */
/* appends one line to the file in the variable PHLOG with what it sees  */
/* on entry, then does what the request header X-Act asks of its tag:    */
/* words "Tag.op=value", for example "Trans.file=/x/y Fixup.ret=0".      */
/*                                                                        */
/*   ret=n       return n (otherwise -1, DECLINED: the phase goes on)    */
/*   uri= file= handler= ct= status= pi=   change that field             */
/*   note= env=  set the note or subprocess_env entry "k"               */
/*   hout= xout= set the header X-Out-Tag (headers_out, err_headers_out) */
/*   hin=        set the request header X-In                             */
/*   say=1       Say a line                                              */
/*   log=1       WWWLogError a line                                      */
/*   syntax=1    a syntax error                                          */
/*   redirect=u  WWWInternal_Redirect                                    */
/*   args=1      WWWGetArgs, and log WWWARGS.0 and WWWPOST_STRING       */
r = arg(1)
parse source . . me
tag = filespec('name', me)
tag = left(tag, lastpos('.', tag) - 1)
line = tag
call add 'main', WWWIS_MAIN_REQUEST
call add 'uri', WWWReqRecUri(r)
call add 'file', WWWReqRecFilename(r)
call add 'handler', WWWReqRecHandler(r)
call add 'ct', WWWReqRecContent_type(r)
call add 'user', WWWReqRecUser(r)
call add 'status', WWWReqRecStatus(r)
call add 'pi', WWWReqRecPath_info(r)
call add 'pt', WWWPATH_TRANSLATED
call add 'note', WWWReqRecNotes(r, 'k')
call add 'env', WWWReqRecSubprocess_env(r, 'k')
call add 'in', WWWReqRecHeader_in(r, 'X-In')
call add 'stm', WWWReqRecFinfo_stmode(r)
call add 'wfile', WWWFILENAME
ret = -1
if tag == 'Content' | tag == 'Forced' then do
  say line
  ret = 0
end
do w over WWWReqRecHeader_in(r, 'X-Act')~makeArray(' ')
  parse var w t '.' op '=' v
  if t \== tag then iterate
  select case op
    when 'ret'      then ret = v
    when 'uri'      then call WWWReqRecUri r, v
    when 'file'     then call WWWReqRecFilename r, v
    when 'handler'  then call WWWReqRecHandler r, v
    when 'ct'       then call WWWReqRecContent_type r, v
    when 'status'   then call WWWReqRecStatus r, v
    when 'pi'       then call WWWReqRecPath_info r, v
    when 'note'     then call WWWReqRecNotes r, 'k', v
    when 'env'      then call WWWReqRecSubprocess_env r, 'k', v
    when 'hout'     then call WWWReqRecHeader_out r, 'X-Out-'tag, v
    when 'xout'     then call WWWReqRecErr_header_out r, 'X-Out-'tag, v
    when 'hin'      then call WWWReqRecHeader_in r, 'X-In', v
    when 'say'      then say 'said in' tag
    when 'log'      then call WWWLogError r, 'logged in' tag
    when 'syntax'   then x = 1 + 'a'
    when 'redirect' then call WWWInternal_Redirect r, v
    when 'args'     then do
      call WWWGetArgs r
      call add 'args', wwwargs.0
      call add 'post', WWWPOST_STRING
    end
    otherwise nop
  end
end
call lineout PHLOG, line
call lineout PHLOG
return ret

add:
  line = line'|'arg(1)'='arg(2)
  return
