# mod_orx

An Apache HTTP Server module for [ooRexx](https://sourceforge.net/projects/oorexx/):
Rexx programs as CGI scripts without a fork per request, and Rexx code
attached to the other phases of Apache's request cycle (authentication,
authorization, logging...), which a CGI cannot reach.

**Status: beta.** The content handler `oorexx-cgi` works and passes
the full [RexxHTTP](https://rexx.epbcn.com/rexxhttp/) HTTP test suite
unchanged, in place of mod_cgid, under both the event and the prefork MPM.
Apache starts, watches and restarts the host itself. Rexx programs can also
authenticate users, authorize requests and log them, through Apache's own
provider mechanisms, and run in every other phase of the request
(`OoRexx<Phase>Hook`), where they talk to Apache with commands
(`Address APACHE`). Large packages such as RexxHTTP's classes can be
preloaded once per host process. Programs written for mod_rexx run
unchanged, in every phase, with mod_rexx's own configuration, through an
optional layer ([modrexx/README.md](modrexx/README.md)). mod_orx needs
ooRexx 5.3.0, and stays in beta until 5.3.0 is released; Windows is still
to come. Not for production yet.

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
  to the connection. Besides, each process keeps one instance alive, its
  *anchor*, so that the interpreter does not start from scratch in every
  phase; what it loads (the `APACHE` environment, preloaded packages) is
  loaded once per process. A watchdog halts a program that exceeds its deadline.
  A process is recycled after a number of requests. If one dies, the
  others, and Apache, are not affected. The pool grows when no process is
  idle, up to a maximum, and shrinks back to its minimum when it has had
  idle processes to spare for a while.

Apache manages the host as mod_cgid manages its daemon. The module binds
the socket as root, in Apache's run-time directory, so that only Apache's
`User` can connect to it. It starts `oorexx-host` on that socket, as that
user and in a process group of its own, and restarts it if it dies: at
once if it had run for a minute; if not, after a wait that doubles every
time, from 1 s up to 60 s, so that a host that cannot stay up is not
restarted, and logged, every second. The
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
connection per phase, with the protocol version in the first frame
(`include/oorexx_protocol.h`).

Everything else is built on the core, through two interfaces internal to
mod_orx (they are versioned, and must match exactly; nothing is promised to
third parties):

- `include/mod_orx.h`: what the core offers to the other Apache modules of
  mod_orx, as APR optional functions: build what is sent to the host (the
  CGI environment, and a snapshot of the request), run a phase, send its
  output to the client or nowhere, allow some changes to the request and
  not others.
- `include/orx_host_plugin.h`: what `oorexx-host` offers to a plugin that
  serves a class of program (`--plugin`): read the snapshot, change the
  request, and set the policy of the interpreter instance.

The mod_rexx layer is the first such client: the module `mod_orx_rexx.so`
and the plugin `orx_rexx_plugin.so`, in `modrexx/`. A site that does not
load it sees nothing of mod_rexx.

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
- ooRexx 5.3.0 (r13344 or later) with its API headers: the `APACHE`
  environment of native programs is a `.CommandHandler`, new in 5.3.0;
- a C and a C++ compiler.

```sh
make
sudo make install            # APXS=... OOREXX_PREFIX=... if needed
```

Only for programs written for mod_rexx, the optional layer
([modrexx/README.md](modrexx/README.md)):

```sh
make modrexx
sudo make install-modrexx    # PLUGINDIR=... too, if needed
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
| `OoRexxQueueTimeout seconds` | `10` | How long a phase waits for a free host process when they are all busy. After that, a 503 (500 in an authentication or authorization provider), and a line in the error log. Also valid per directory. |
| `OoRexxLogQueueTimeout seconds` | `2` | The same, in the logging phase, where the hook is skipped after that. Shorter, because the client already has its response: waiting there only holds an Apache worker, and a place in the host's queue that content may need. Also valid per directory. |
| `OoRexxAuthnProgram path` | none | The program behind `AuthBasicProvider oorexx`. Per directory. |
| `OoRexxLogHook path` \| `none` | none | A program to run in the logging phase. Per directory; `none` switches off an inherited one. |
| `OoRexx<Phase>Hook path [args...]` \| `none` | none | A native program in that phase (see below): `PostReadRequest` and `Trans` in the server or a `<VirtualHost>`; `HeaderParser`, `Access`, `Authen`, `Type` and `Fixup` per directory too. |
| `OoRexxPreload file...` | none | Rexx packages every host process loads once, when it starts (see below). May be repeated. |

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
takes about 4.6 ms per instance, and 0.34 ms when they are preloaded. (The
anchor itself, which every host process now keeps even without
`OoRexxPreload`, already makes a RexxHTTP page cheaper: from about 7.3 to
about 4.0 ms on the suite's test page, sequential requests.) A
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

### What a host process keeps between phases: `.environment`

Every phase runs in a new interpreter instance, so its `.local`, its
streams and the packages it loads itself start from scratch. `.environment`
does not: in ooRexx it belongs to the process, and every instance shares it
(measured). Whatever a program leaves there, any later phase served by that
process sees, whichever program it runs, of whatever class, in whatever
phase. mod_orx does not clean it, and does not try to: a check of the
entries would miss everything below them (a collection that was already
there and has been filled, a class's state, a stream left open), and a
cleaning that cannot be complete would only promise what it does not keep.
So:

- **Do not leave anything sensitive in `.environment`**, nor in any object
  that a later program can reach from it.
- **It is not a store.** Each process of the pool has its own
  `.environment`, and each request goes to any of them: what one request
  leaves there, the next may not see. At most it is a cache that is
  sometimes right, and it dies, without notice, when its process is
  recycled (every `OoRexxMaxRequests` phases), when the pool shrinks, when
  the process or the host dies, and on every restart of Apache, graceful
  ones too. In a closed application, where nothing can leak, it may serve
  as such a cache; state that must be shared between requests belongs
  outside the process (a file, a database).
- State that is meant to be shared between the phases a process serves has
  a deliberate place: the class state of a preloaded package (see above),
  with the same lifetime and the same visibility, but in plain sight.

RexxHTTP itself keeps the request and the response in `.local`; it puts
only its four classes in `.environment`.

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

Each phase runs in a fresh interpreter instance, like a CGI: about 2.3 ms
per hook, measured on a static file with a hook that only returns `1`.
These programs can also use `Address APACHE` (next section): the providers
may change `ENV` and `NOTE`; the logging hook only reads, and its `STATUS`
and `BYTES_SENT` are the final ones.

### Native programs in every phase: `Address APACHE`

```apache
OoRexxTransHook /srv/rexx/trans.rex
<Directory "/srv/www">
    OoRexxFixupHook /srv/rexx/fixup.rex strict
</Directory>
<Location "/app/">
    AuthType oorexx                    # authentication without a password:
    AuthName app                       # a session cookie, a token...
    OoRexxAuthenHook /srv/rexx/session.rex
    Require valid-user
</Location>
```

A native program gets the arguments of its directive (`Use Arg`) and the
CGI environment, and talks to Apache with commands:

```rexx
-- OoRexxTransHook: what was under /old/ is served from /var/www/new/
Address APACHE
"GET URI"
If uri~startsWith("/old/") Then Do
  "SET REQUEST_FILENAME" "/var/www/new/"uri~substr(6)
  "SET NOTE origin" uri
  Return 1                          -- this translation is mine
End
Return                              -- someone else decides
```

| Command | Does |
|---|---|
| `GET field [INTO var]` | Sets a variable named after the field (`URI`), or `var`. |
| `GET table key [INTO var]` | Sets `TABLE.KEY` (the key in uppercase), the same stem under the singular and the plural name (`HEADER_IN.` and `HEADERS_IN.`). With repeated entries, the last one. |
| `SET field value` | Changes a field. The value is the rest of the line, as it is. |
| `SET` \| `ADD` \| `UNSET table key [value]` | Replaces, adds or removes entries. |
| `LIST table [key]` | `Name: value` lines (or the values of `key`), in order: to `Address APACHE "..." With Output Stem s.` (or `Using (array)`), or else to `.output`, which in a hook is the error log. |
| `LOG level message` | A line in the error log at a `LogLevel` level (`emerg`...`debug`, `trace1`...`trace8`), filtered by `LogLevel orx:...`. |

Fields take `mod_rewrite`'s names: `THE_REQUEST`, `REQUEST_METHOD`,
`SERVER_PROTOCOL`, `HOSTNAME`, `UNPARSED_URI`, `URI`, `QUERY_STRING`,
`REQUEST_FILENAME`, `PATH_INFO`, `HANDLER`, `CONTENT_TYPE`,
`CONTENT_ENCODING`, `CONTENT_LANGUAGES`, `USER`, `AUTH_TYPE`, `STATUS`,
`BYTES_SENT`, `REMOTE_ADDR`, `SERVER_NAME`, `SERVER_PORT`, and `PHASE`.
(Not `REQUEST_URI`, which means two things in Apache.) Tables:
`HEADER_IN`, `HEADER_OUT`, `ERR_HEADER_OUT`, `ENV`, `NOTE`. `PREV`, `NEXT`
or `MAIN` before the thing go to a related request, read-only, and prefix the
variable: `"GET PREV URI"` sets `PREV_URI`; `"GET PREV PREV URI"`,
`PREV_PREV_URI`.

`RC` is 0, or 1 when there is no such entry or related request (the
variable is left without a value). A value Apache would not take (a control
character in a header, a relative `REQUEST_FILENAME`) raises ERROR with
`RC` 2. A misuse (an unknown command, a change the phase does not allow) is
a SYNTAX error, 93.900, at the command itself.

| Changes allowed in | `URI` | `QUERY_STRING` | `REQUEST_FILENAME`, `PATH_INFO` | `HANDLER`, `CONTENT_*` | `USER` | `HEADER_IN` | `HEADER_OUT`, `ERR_HEADER_OUT` | `ENV`, `NOTE` |
|---|---|---|---|---|---|---|---|---|
| PostReadRequest | yes | yes | | | | yes | yes | yes |
| Trans | yes | yes | yes | | | | yes | yes |
| HeaderParser | | | | | | yes | yes | yes |
| Access | | | | | | | yes | yes |
| Authen | | | | | yes | | yes | yes |
| Type | | | | yes | | | yes | yes |
| Fixup | | yes | yes | yes | | | yes | yes |
| providers | | | | | | | | yes |
| Log | | | | | | | | |

What the program returns is the hook's answer:

| Returns | Trans, Type, Authen (the first that decides) | PostReadRequest, HeaderParser, Access, Fixup (all run) |
|---|---|---|
| `1` | decided: the next modules do not run | go on |
| nothing | someone else decides | go on |
| `0` | 403 (Authen: 401) | 403 |
| 300 to 599 | that status | that status |
| anything else, an error, the timeout | 500, and a line in the error log | the same |

The module checks that a `Trans` that returns `1` has set an absolute
`REQUEST_FILENAME`, an `Authen` that returns `1` has set `USER`, and a 3xx
has a `Location`. A status drops `HEADER_OUT` (Apache keeps only
`ERR_HEADER_OUT`, and `Location`): what must survive it, a `Set-Cookie` in a
302 say, goes in `ERR_HEADER_OUT`, and the module warns in the error log of
each header lost. There is no internal redirect from a hook: in `Trans`,
change `URI` or `REQUEST_FILENAME`; elsewhere, return a status and let an
`ErrorDocument` take it.

Where they run:

- `Trans` runs early, as `mod_rewrite` does, right after it, and so before
  `mod_alias`; `Type`, before `mod_mime`, so that it can decide every type.
  (In these two phases the mod_rexx layer keeps mod_rexx's place, after
  them; in the others, its handlers run before the native hooks.)
- `OoRexxAuthenHook` runs only where `AuthType oorexx`. On `1`, `REMOTE_USER`
  and `AUTH_TYPE` reach the content and the log, and `Require valid-user`,
  `Require user` and `Require oorexx` work on top of it.
- The hooks do not run in subrequests (a `DirectoryIndex` lookup, an
  include, negotiation), except `Access` and `Authen`, which guard what a
  subrequest reaches. They do run in internal redirects (an `ErrorDocument`),
  where `PREV` is the request before; Apache itself skips the auth phases of
  a redirect or a subrequest whose configuration is that of the request
  before it, and takes its user.
- They do not read the request body: `.input` is empty.

### Debugging the host

To debug the host (under a debugger, say), set `OoRexxHost none`, choose an
`OoRexxSocket` in a directory that Apache's user owns, and start the host
there yourself, as that user:

```sh
sudo -u www-data oorexx-host --socket /run/oorexx/host.sock --processes 2
```

With the mod_rexx layer loaded, give the host its plugin too:
`--plugin 2:/usr/local/lib/mod_orx/orx_rexx_plugin.so`.

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

`tests/queue.sh` checks `OoRexxQueueTimeout` and `OoRexxLogQueueTimeout`
with every host process busy. `tests/restart.sh` checks the wait between
restarts: a host that dies as it starts, a stop during a wait, and a host
that had run for a minute (it takes about two minutes).

`tests/standalone.sh [event|prefork]` needs nothing of Debian's: it starts
an Apache of its own, from a minimal configuration in a temporary
directory, checks that a Rexx CGI is served, kills Apache's parent with a
program still running, and checks that every process of the host ends on
its own once that program has finished. On Linux the host learns of the
death from the kernel (`PR_SET_PDEATHSIG`); elsewhere it watches for it
itself, and `make CXXFLAGS='-O2 -Wall -DORX_PORTABLE' APXS='apxs
-Wc,-DORX_PORTABLE'` builds that way on Linux too, to test it there.

Continuous integration (`.github/workflows/ci.yml`) runs every test on
Ubuntu under both MPMs, and builds mod_orx and runs `tests/standalone.sh`
on macOS. Each test reports through annotations (`tests/ci-run.sh`).
`tests/apache.sh` checks `Address APACHE` in the providers and the logging
hook; `tests/native-phases.sh`, the `OoRexx<Phase>Hook` hooks: the order of
the phases, every kind of return value, what each phase may change, the
module's checks, `Authen`, subrequests and internal redirects, the
directives and `LOG`.

The suite and these tests run without the mod_rexx layer. Its own
tests, `tests/modrexx.sh` and `tests/modrexx-phases.sh`, load it for their
run: see [modrexx/README.md](modrexx/README.md).

## License

Apache License 2.0. See [LICENSE](LICENSE).
