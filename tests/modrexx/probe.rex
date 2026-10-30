/* probe.rex -- prints, one per line, what a mod_rexx program sees: the   */
/* initial variables, the WWW* functions' answers, and what each write    */
/* reads back. tests/modrexx.sh runs it under mod_orx and compares the    */
/* lines with the ones measured under mod_rexx 2.4.0.                     */
r = arg(1)
call WWWSendHTTPHeader r, 'text/plain'
call show 'arg()', arg()
call show 'datatype(arg(1))', datatype(r)
vars = 'AUTH_TYPE CONTENT_LENGTH CONTENT_TYPE GATEWAY_INTERFACE',
       'HTTP_USER_ACCEPT HTTP_USER_AGENT PATH_INFO PATH_TRANSLATED',
       'QUERY_STRING REMOTE_ADDR REMOTE_HOST REMOTE_IDENT REMOTE_USER',
       'REQUEST_METHOD SCRIPT_NAME SERVER_NAME SERVER_PORT SERVER_PROTOCOL',
       'SERVER_SOFTWARE DEFAULT_TYPE IS_MAIN_REQUEST SERVER_ROOT',
       'UNPARSEDURI URI FILENAME POST_STRING RSPCOMPILER FNAMETEMPLATE'
do v over vars~makeArray(' ')
  call show 'WWW'v, value('WWW'v)
end
/* RexxSetVar ExampleVar and EXAMPLEVAR2: a name is set as it is written. */
call show 'EXAMPLEVAR', examplevar
call show 'EXAMPLEVAR2', examplevar2
call show 'WWWGetVersion', WWWGetVersion()~word(1)
call show 'WWWHTTP_time words', WWWHTTP_time(r)~words
call show 'WWWEscape_Path 0', WWWEscape_Path(r, '/a b/c?d#e%f&g', 0)
call show 'WWWEscape_Path 1', WWWEscape_Path(r, '/a b/c?d#e%f&g', 1)
call show 'WWWConstruct_URL', WWWConstruct_URL(r, '/x/y z')
call show 'IsMain', WWWReqRecIsMain(r)
call show 'The_request', WWWReqRecThe_request(r)
call show 'Proxyreq', WWWReqRecProxyreq(r)
call show 'Header_only', WWWReqRecHeader_only(r)
call show 'Protocol', WWWReqRecProtocol(r)
call show 'Status_line', WWWReqRecStatus_line(r)
call show 'Status', WWWReqRecStatus(r)
call show 'Method', WWWReqRecMethod(r)
call show 'Method_number', WWWReqRecMethod_number(r)
call show 'Allowed', WWWReqRecAllowed(r)
call show 'Bytes_sent', WWWReqRecBytes_sent(r)
call show 'Header_in User-Agent', WWWReqRecHeader_in(r, 'user-agent')
call show 'Header_in X-None', WWWReqRecHeader_in(r, 'X-None')
call show 'Err_header_out X-None', WWWReqRecErr_header_out(r, 'X-None')
call show 'Subprocess_env X-None', WWWReqRecSubprocess_env(r, 'X-None')
call show 'Notes X-None', WWWReqRecNotes(r, 'X-None')
call show 'Content_type', WWWReqRecContent_type(r)
call show 'Content_encoding', WWWReqRecContent_encoding(r)
call show 'Handler', WWWReqRecHandler(r)
call show 'Content_languages', WWWReqRecContent_languages(r)
call show 'No_cache', WWWReqRecNo_cache(r)
call show 'Uri', WWWReqRecUri(r)
call show 'Filename', WWWReqRecFilename(r)
call show 'Path_info', WWWReqRecPath_info(r)
call show 'Args', WWWReqRecArgs(r)
call show 'Finfo_stmode', WWWReqRecFinfo_stmode(r)
call show 'User', WWWReqRecUser(r)
call show 'Auth_type', WWWReqRecAuth_type(r)
s = WWWReqRecServer(r)
c = WWWReqRecConnection(r)
call show 'Server_admin', WWWSrvRecServer_admin(s)
call show 'Server_hostname', WWWSrvRecServer_hostname(s)
call show 'Port', WWWSrvRecPort(s)
call show 'Is_virtual', WWWSrvRecIs_virtual(s)
call show 'Aborted', WWWCnxRecAborted(c)
/* Writes, and what a read sees afterwards. */
call show 'set Status_line', WWWReqRecStatus_line(r, '299 Probed')
call show '  Status_line', WWWReqRecStatus_line(r)
call show 'set Status', WWWReqRecStatus(r, '201')
call show '  Status', WWWReqRecStatus(r)
call show 'set Method', WWWReqRecMethod(r, 'PROBE')
call show '  Method', WWWReqRecMethod(r)
call show 'set Method_number', WWWReqRecMethod_number(r, 7)
call show '  Method_number', WWWReqRecMethod_number(r)
call show 'set Allowed', WWWReqRecAllowed(r, 5)
call show '  Allowed', WWWReqRecAllowed(r)
call show 'set Header_in', WWWReqRecHeader_in(r, 'X-In', 'in1')
call show '  Header_in', WWWReqRecHeader_in(r, 'x-in')
call show 'set Header_out', WWWReqRecHeader_out(r, 'X-Out', 'out1')
call show 'set Err_header_out', WWWReqRecErr_header_out(r, 'X-Err', 'err1')
call show '  Err_header_out', WWWReqRecErr_header_out(r, 'x-err')
call show 'set Subprocess_env', WWWReqRecSubprocess_env(r, 'X_ENV', 'env1')
call show '  Subprocess_env', WWWReqRecSubprocess_env(r, 'X_ENV')
call show 'set Notes', WWWReqRecNotes(r, 'x-note', 'note1')
call show '  Notes', WWWReqRecNotes(r, 'X-NOTE')
call show 'set Content_encoding', WWWReqRecContent_encoding(r, 'identity')
call show '  Content_encoding', WWWReqRecContent_encoding(r)
call show 'set Handler', WWWReqRecHandler(r, 'probe-handler')
call show '  Handler', WWWReqRecHandler(r)
call show 'set Content_languages', WWWReqRecContent_languages(r, 'ca')
call show 'set Content_languages', WWWReqRecContent_languages(r, 'es')
call show '  Content_languages', WWWReqRecContent_languages(r)
call show 'set No_cache', WWWReqRecNo_cache(r, 1)
call show '  No_cache', WWWReqRecNo_cache(r)
call show 'set Uri', WWWReqRecUri(r, '/probed')
call show '  Uri', WWWReqRecUri(r)
call show 'set Filename', WWWReqRecFilename(r, '/tmp/probed')
call show '  Filename', WWWReqRecFilename(r)
call show 'set Path_info', WWWReqRecPath_info(r, '/pi')
call show '  Path_info', WWWReqRecPath_info(r)
call show 'set Content_type', WWWReqRecContent_type(r, 'text/plain; charset=x-probe')
call show '  Content_type', WWWReqRecContent_type(r)
call WWWReqRecMethod_number r, 0     /* M_GET again, for WWWGetArgs */
call show 'WWWSetHeaderValue', WWWSetHeaderValue(r, 'X-Set', 'set1')
call show 'WWWAddCookie', WWWAddCookie(r, 'k=v')
call show 'WWWLogInfo', WWWLogInfo(r, 'probe info')
/* Arguments the functions refuse. */
call try 'WWWGetVersion(1)'
call try 'WWWReqRecUri(r, 1, 2)'
call try 'WWWReqRecHeader_in(r)'
call try 'WWWReqRecHeader_out(r, 1)'
call try 'WWWAddCookie(r, 1, 2)'
call try 'WWWGetArgs()'
call try 'WWWReqRecNo_cache()'
/* Stems in the caller, also inside a procedure, with a symbolic tail. */
name = 'zz'
call WWWGetArgs r
call show 'WWWARGS.0', wwwargs.0
do i = 1 to wwwargs.0
  call show 'WWWARGS.'i'.!NAME', wwwargs.i.!name
  call show 'WWWARGS.'i'.!VALUE', wwwargs.i.!value
end
call show 'WWWARGS.zz', wwwargs.zz
k = 'zz'
call show 'WWWARGS.k (k = zz)', wwwargs.k
call show 'WWWPOST_STRING', wwwpost_string
call WWWGetCookies r
call show 'WWWCOOKIES.0', wwwcookies.0
do i = 1 to wwwcookies.0
  call show 'WWWCOOKIES.'i, wwwcookies.i.!name'='wwwcookies.i.!value
end
call inproc
return 0

inproc: procedure expose r
  call WWWGetCookies r
  call show 'in procedure WWWCOOKIES.0', wwwcookies.0
  return

show: say arg(1)'=['arg(2)']'; return

try:
  signal on syntax
  interpret 'x =' arg(1)
  call show arg(1), 'returned' x
  return
syntax:
  call show arg(1), 'Error' condition('O')~code
  return
