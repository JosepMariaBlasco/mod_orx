# mod_orx

An Apache HTTP Server module for [ooRexx](https://sourceforge.net/projects/oorexx/):
Rexx programs as CGI scripts without a fork per request, and Rexx code
attached to the other phases of Apache's request cycle (authentication,
authorization, logging...), which a CGI cannot reach.

**Status: milestone 1.** The content handler `oorexx-cgi` works and passes
the full [RexxHTTP](https://rexx.epbcn.com/rexxhttp/) HTTP test suite
unchanged, in place of mod_cgid, under both the event and the prefork MPM.
Apache starts, watches and restarts the host itself. Rexx programs can also
authenticate users, authorize requests and log them, through Apache's own
provider mechanisms. Large packages such as RexxHTTP's classes can be
preloaded once per host process. Programs written for mod_rexx run
unchanged, in every phase, with mod_rexx's own configuration (see below).
The native program class in the other phases, and Windows, are still to
come. Not for production yet.

## Design in one page

ooRexx does not run Rexx code in parallel inside one process, not even in
separate interpreter instances: with two threads, throughput is about four
times worse than with two processes (measured). `.environment` is shared by
every instance of a process, and a native fault inside a process takes the
whole process down. Apache's default MPMs are threaded (event on Unix; one
single child process on Windows). Running Rexx inside Apache's workers
would therefore serialise it, mix the state of concurrent requests, and let
one failing program take a worker down with all its connections.

So mod_orx is two pieces:

- **`mod_orx.so`**, a thin module inside Apache. It registers the hooks
  and talks to the host over a local socket. It does not link ooRexx.
- **`oorexx-host`**, a pool of processes outside Apache. Each process
  serves one phase of one request at a time, in a **fresh interpreter
  instance** (about 0.2–0.5 ms), with all of the program's I/O redirected
  to the connection. A watchdog halts a program that exceeds its deadline.
  A process is recycled after a number of requests. If one dies, the
  others, and Apache, are not affected. The pool grows when no process is
  idle, up to a maximum, and shrinks back to its minimum when it has had
  idle processes to spare for a while.

Apache manages the host as mod_cgid manages its daemon. The module binds
the socket as root, in Apache's run-time directory, so that only Apache's
`User` can connect to it. It starts `oorexx-host` on that socket, as that
user and in a process group of its own, and restarts it if it dies. The
host's first process is a small *keeper*, Apache's only child; the pool's
master is the keeper's child. Apache's parent
keeps the socket open the whole time, so requests queue on it while a dead
host is being replaced. On every restart, graceful or not, and on every
stop, the module unlinks the socket and tells the host to **drain**: it
finishes what it holds and what is already queued, then exits. The new
generation starts at once on a new socket, so requests that are already
running finish on the old host while new ones go to the new. The keeper
passes the signal on to the master and exits at once, and Apache reaps it
there and then. Apache unloads and reloads its modules on every restart, so
nothing of the old host may call into the module afterwards, not even to be
reaped (an upgraded module would crash Apache's parent).

The protocol is a small binary framing (type, flags, length, payload), one
connection per phase, with the protocol version in the first frame.

For `oorexx-cgi`, the module behaves like mod_cgi:

- the same preconditions (`Options ExecCGI`, `AcceptPathInfo`...);
- the same environment (`ap_add_common_vars` + `ap_add_cgi_vars`);
- the same header parsing (Apache's own CGI header scanner);
- the same handling of `Location`.

The request body is read on demand. The program's output is a real
`.Stream`, streamed to the client and flushed when the program goes quiet,
so an explicit flush reaches the client early, and a response written in
one go gets a `Content-Length`.

## History

- **mod_rexx** (W. David Ashley, 2004–2007; Apache 2.4 port in 2020) ran
  classic Rexx inside Apache's processes.
- In 2009–2010 a rewrite for ooRexx 4.0 named **mod_ooRexx** lived in the
  ooRexx SVN incubator. It was never released.
- This is an independent reimplementation, written from scratch under the
  Apache License 2.0. It keeps the idea that made mod_rexx valuable (Rexx
  at every phase of the request), not its code.

The name keeps clear of that unreleased mod_ooRexx. `orx` was the original
file type for Oryx, the internal product name of what became ooRexx.

## Building

Requirements:

- Apache 2.4 with `apxs` (Debian/Ubuntu: `apache2-dev`);
- ooRexx 5.2.0 or later with its API headers;
- a C and a C++ compiler.

```sh
make
sudo make install            # APXS=... OOREXX_PREFIX=... if needed
```

## Configuring

```apache
LoadModule orx_module modules/mod_orx.so
OoRexxTimeout 60

<Directory "/var/www/html/rexx">
    Options +ExecCGI
    AddHandler oorexx-cgi .rex
    Require all granted
</Directory>
```

| Directive | Default | Meaning |
|---|---|---|
| `OoRexxHost path` \| `none` | `$(BINDIR)/oorexx-host` | The host executable. `none`: the module does not start a host; one is started by hand (see below). |
| `OoRexxSocket path` | `oorexx.sock` in `DefaultRuntimeDir` | The socket between the module and the host. |
| `OoRexxProcesses min [max]` | `2 8` | Size of the host's pool. With a single number, the pool has a fixed size. |
| `OoRexxMaxRequests n` | `1000` | Phases a host process serves before it is replaced (`0`: no limit). |
| `OoRexxTimeout seconds` | `60` | Deadline of a phase; also valid per directory. A drained host gets the largest one, plus 10 seconds, before what is left is killed. |
| `OoRexxAuthnProgram path` | none | The program behind `AuthBasicProvider oorexx`. Per directory. |
| `OoRexxLogHook path` \| `none` | none | A program to run in the logging phase. Per directory; `none` switches off an inherited one. |
| `OoRexxPreload file...` | none | Rexx packages every host process loads once, when it starts (see below). May be repeated. |
| `OoRexxSetVar name value` | none | A variable more for every program of the `OoRexx*` set (see *Programs written for mod_rexx*), like mod_rexx's `RexxSetVar`. Per directory, also in `.htaccess`; the outer ones are inherited. |
| `OoRexxHandler path` \| `none` | none | The program `oorexx-modrexx` runs instead of the requested file, like mod_rexx's `RexxHandler`. Per directory, also in `.htaccess`. |
| `OoRexx<Phase>Handler path` \| `none` | none | A program written for mod_rexx for that phase: `PostReadRequest`, `Trans`, `HeaderParser`, `Access`, `Authen`, `Authz`, `Type`, `Fixup`, `Log` (see below). Per directory, also in `.htaccess`; `none` switches off an inherited one. |
| `Rexx*` | | mod_rexx's own directives, with its meaning (see below). |

`OoRexxHost`, `OoRexxSocket`, `OoRexxProcesses`, `OoRexxMaxRequests` and
`OoRexxPreload` are global: they cannot appear inside `<VirtualHost>`.
Program paths are relative to `ServerRoot`, as in `ScriptAlias`.

### Preloading packages

Every phase runs in a fresh interpreter instance, and a program that
`::Requires` a large package loads and parses it again every time. With
`OoRexxPreload`, each host process loads the given packages once, when it
starts, in an instance it keeps alive; a program that requires one of them
finds it already loaded.

```apache
OoRexxPreload /srv/rexxhttp/HTTP.Request.cls /srv/rexxhttp/HTTP.Response.cls \
              /srv/rexxhttp/HTTP.OutputStream.cls /srv/rexxhttp/HTTP.Cookie.cls
```

For RexxHTTP this is most of the cost of a request: loading its four classes
takes about 4.6 ms per instance, and 0.34 ms when they are preloaded. A
RexxHTTP page served by `oorexx-cgi` went from about 190–270 to about 870
requests per second (two cores, `ab -c 4`).

What is shared, and what is not:

- A preloaded package is loaded once per host process, and every phase that
  process serves uses that same copy: if its classes keep state of their own
  (class variables), that state carries over from one phase to the next.
  RexxHTTP's classes keep none.
- A package that is not preloaded (a program's own) is still loaded afresh in
  every phase, with its state starting from scratch, as in a CGI.
- Code in a preloaded package sees the `.local`, `.output` and other streams
  of the phase that calls it, not those of the instance that loaded it.

A preloaded file that changes on disk is seen only by new host processes:
after deploying a new version, restart Apache (`apachectl graceful` is
enough). A file that does not exist is refused when the configuration is
read; one that fails to load (a syntax error, say) stops the host, with the
error, its file and its line in the error log, and the host is not
restarted.

### Authentication, authorization and logging

These phases use the mechanisms Apache already has, so they combine with
everything else in the usual way:

```apache
<Location "/private/">
    AuthType Basic
    AuthName "Private"
    AuthBasicProvider oorexx file          # Rexx first, then a password file
    AuthUserFile /etc/apache2/users
    OoRexxAuthnProgram /srv/rexx/authn.rex
    Require oorexx /srv/rexx/authz.rex admins editors
</Location>

OoRexxLogHook /srv/rexx/log.rex
```

Such a program gets the CGI environment, as a CGI would (without the
request body; the `Authorization` header only with `CGIPassAuth On`, as for
a CGI), plus its arguments; what it returns is its verdict:

| Phase | `Use Arg` | Returns `1` | Returns `0` | Returns nothing |
|---|---|---|---|---|
| authentication | user, password | granted | denied | user not found: the next provider decides |
| authorization | user (`""` if none), the rest of the `Require` line | granted | denied | neutral: the other `Require` lines decide |
| logging | status, bytes sent, microseconds since the request arrived | (ignored) | (ignored) | (ignored) |

Any other value, an error in the program, or a program that runs past
`OoRexxTimeout` is a general error: a line in the error log and, in
authentication and authorization, a 500.
What such a program says with `Say` goes to the error log too, since there
is no client to send it to.

Two things work as they do in Apache's own providers. Apache asks
authorization first without a user: where authentication is configured, a
`0` then makes it authenticate and ask again (so the program runs twice);
where it is not, a `0` is a plain 403. An authenticated user who is not
authorized gets a 401, as with `Require user`, unless
`AuthzSendForbiddenOnFailure` is on.

Each phase runs in a fresh interpreter instance, like a CGI: about 1.4 ms
per hook, measured on a static file with a hook that only returns `1`.

### Programs written for mod_rexx

[mod_rexx](https://sourceforge.net/projects/modrexx/) programs run
unchanged: they get their request in `arg(1)`, the `WWW*` variables exist
before their first line, the `WWW*` functions are there without being
declared, and what they return is the phase's status (`0` OK, `-1`
declined, `-2` done, or an HTTP status). Their own `Apache.cls` keeps
working, since it only calls those functions.

There are two sets of directives for them. The first is **mod_rexx's
own**, with its meaning: a configuration written for mod_rexx works as it
is, and the only line to change is the `LoadModule`. (So mod_rexx and
mod_orx cannot be loaded in the same Apache.)

```apache
LoadModule orx_module modules/mod_orx.so     # instead of mod_rexx.so
RexxSetVar SITE "My site"
RexxFixupHandler /srv/rexx/fixup.rex
<Directory "/var/www/html/rexx">
    AddHandler rexx-handler .rex
</Directory>
```

The second, **mod_orx's**, has the same names with `Oo` in front, and the
handler `oorexx-modrexx`. Its directives mean what their names say, and
behave as Apache's do; mod_rexx's had three oddities, which its set keeps:

| | `Rexx*` (mod_rexx's) | `OoRexx*` (mod_orx's) |
|---|---|---|
| Content | `AddHandler rexx-handler` (or `rexx_handler`, `application/x-httpd-rexx-script`), `RexxHandler` | `AddHandler oorexx-modrexx`, `OoRexxHandler` |
| Phases | `RexxPostReadRequestHandler`, `RexxTransHandler`, `RexxHeaderParserHandler`, `RexxAccessHandler`, `RexxAuthenHandler`, `RexxAuthzHandler`, `RexxTypeHandler`, `RexxFixupHandler`, `RexxLogHandler` | the same, with `OoRexx` |
| Variables | `RexxSetVar` | `OoRexxSetVar` |
| Authentication and authorization | crossed: `RexxAuthenHandler` runs in authorization (`auth_checker`), `RexxAuthzHandler` in authentication (`check_user_id`) | `OoRexxAuthenHandler` in authentication, `OoRexxAuthzHandler` in authorization |
| Inner sections | a section with any `Rexx*` directive replaces all the `Rexx*` ones around it (nothing is inherited) | inherit what they do not set; `none` switches a program off |
| Program paths | as written | relative to `ServerRoot` |

A program of each set sees only the variables of its own set. Both sets
can be used side by side; in the same hook, the `Rexx*` program runs first.
`RexxRspCompiler` and `RexxTempFileNameTemplate` (RSP pages) are not
supported, and a configuration that has them does not start.

Under Apache 2.4 the crossing matters. With `AuthType Basic`, Apache's
own module takes the authentication phase, so a program there never runs
(`RexxAuthzHandler`, `OoRexxAuthenHandler`); and since no `WWW*` function
sets the user, a program in that phase can only refuse. The program that
can decide on a known user is the one in the authorization phase
(`RexxAuthenHandler`, `OoRexxAuthzHandler`).

Outside the content phase, as under mod_rexx, nothing runs in a
subrequest, what the program says goes nowhere, and an error in it is a
500 with the error in the error log. The request body is read only if the
program asks for it (`WWWGetArgs` on a POST, say), and is then gone for the
content handler, as under mod_rexx.

Every variable and function behaves as under mod_rexx 2.4.0, its last
version, measured on the same programs and the same configuration under
both (`tests/modrexx.sh`, `tests/modrexx-phases.sh`), down to its
oddities: a variable is set with its name exactly as written, so that a
form field `user` is `WWWARGS.user` (found with `k = 'user'; say
wwwargs.k`) and a `SetVar` name in lowercase is not visible to the
program; Apache holds a small response until the end, so that a status
returned after some output still replaces it; a `HEAD` request is answered
without running the program. The program runs in `/`, and a command goes
to the environment `WWW`, which does not exist (RC -3), not to a shell, as
under mod_rexx. The differences:

- The request is an opaque handle (`orx:` and 16 hex digits), not a
  pointer. A value that is not this request's handle (or its server's, or
  its connection's, where those are expected) is Error 40.1 in the program;
  under mod_rexx it crashed Apache's process.
- `RexxPostReadRequestHandler` and `RexxTransHandler` work. Under mod_rexx
  2.4.0 and Apache 2.4 they crashed Apache's process on every request,
  whatever the program (mod_rexx looked up a path info that does not exist
  yet in those phases). There, `WWWPATH_TRANSLATED` is the request's
  directory, and `WWWFILENAME` is empty until the translation.
- `WWWInternal_Redirect` works only in the content phase; anywhere else it
  is Error 40.1, with a line in the error log saying why. Under mod_rexx it
  crashed Apache's process.
- All the program's output reaches the client (`Say`, `.output`,
  `lineout`, `charout`...), not only `Say`; the request body can also be
  read as input, and `WWWGetArgs` then gets what is left of it. A chunked
  body is accepted.
- `WWWGetArgs` sets `WWWARGS.0` to 0 when it has nothing to parse (a POST
  that is not urlencoded, a PUT...); mod_rexx left it unset, and the usual
  `do i = 1 to wwwargs.0` failed.
- An error in the program is Apache's usual 500 page, and the error, with
  its line, goes to the error log in one line; mod_rexx sent a page of its
  own with the program's path and the error.
- `WWWSERVER_SOFTWARE` does not include `Mod_Rexx/2.4.0`, which mod_rexx
  added to Apache's own version string. `WWWGetVersion` and
  `WWWGATEWAY_INTERFACE` say `Mod_Rexx/2.4.0`, in case a program checks.
- The program's process environment is the CGI environment.
- An internal redirect to a program (`ErrorDocument 404 /error.rex`, an
  `Action`) works; under mod_rexx it crashed Apache's process, for the same
  reason as its `PostReadRequest` and `Trans` phases.
- `WWWReqRecNext`, `WWWReqRecPrev` and `WWWReqRecMain` give a handle to the
  related request, or `(nil)` when there is none, as mod_rexx did. The
  related request can be read with the same functions, but not changed
  (Error 40.1). So a page for an `ErrorDocument` finds the request that
  failed with `WWWReqRecPrev`, and a `LogHandler` finds where a request
  was redirected to with `WWWReqRecNext`.
- The subrequest functions (`WWWSub_Req_Lookup_URI`,
  `WWWSub_Req_Lookup_File`, `WWWRun_Sub_Req`) are not there: a call to one
  is Error 43, routine not found. Under mod_rexx no program could use them:
  they returned the request's pointer in binary, which no other function
  took (and their documentation gives the arguments in the wrong order).

`WWWInternal_Redirect` is done when the program ends, and what the program
wrote is dropped, as mod_rexx's output was.

To debug the host (under a debugger, say), set `OoRexxHost none`, choose an
`OoRexxSocket` in a directory that Apache's user owns, and start the host
there yourself, as that user:

```sh
sudo -u www-data oorexx-host --socket /run/oorexx/host.sock --processes 2
```

## Testing

`tests/rexxhttp-suite.sh /path/to/rexxhttp` deploys the RexxHTTP suite,
swaps mod_cgid for mod_orx and runs the suite unchanged. Result, under
both event and prefork: 182 tests, 313 assertions, 0 failures, 0 errors,
empty error.log, the same counts as under mod_cgid. `OOREXX_PROCESSES` and
`OOREXX_MAX_REQUESTS` change the pool for the run (the suite also passes
with a single process replaced after every phase), and `OOREXX_PRELOAD=1`
preloads RexxHTTP's classes (the suite passes either way).

`tests/lifecycle.sh`, run after the suite, checks the host's life cycle:
graceful restarts with requests in flight, an upgraded module followed by a
graceful and a hard restart, the death of each kind of host process, growth
of the pool up to its maximum, and a stop with a program still running.

`tests/phases.sh`, run after the suite, checks authentication,
authorization and logging, alone and combined with Apache's own providers
(`AuthBasicProvider ... file`, `RequireAny`, `RequireAll`, `Require ip`),
including errors, timeouts and the error log.

`tests/preload.sh`, run after the suite, checks `OoRexxPreload`: what a
preloaded package shares between phases and what a program's own package
does not, a missing file, and a file that fails to load.

`tests/modrexx.sh`, run after the suite, checks `oorexx-modrexx` against
the answers of mod_rexx 2.4.0 itself, measured on the same programs: every
initial variable, every `WWW*` function, every write and what a later read
sees, and what each kind of return value makes of the response.

`tests/modrexx-phases.sh`, run after the suite, checks programs written for
mod_rexx in every phase, with both sets of directives and one same
configuration: what each phase sees, what its return values and its
writes make of the request, authentication with `AuthType Basic` and with
an `AuthType` of its own, inheritance, the body, and the error log. For
the `Rexx*` set, the answers are those of mod_rexx 2.4.0 itself, except
where it crashed.

## License

Apache License 2.0. See [LICENSE](LICENSE).
