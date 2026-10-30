# mod_orx

An Apache HTTP Server module for [ooRexx](https://sourceforge.net/projects/oorexx/):
Rexx programs as CGI scripts without a fork per request, and, later, Rexx
code attached to the other phases of Apache's request cycle
(authentication, authorization, logging...), which a CGI cannot reach.

**Status: milestone 1.** The content handler `oorexx-cgi` works and passes
the full [RexxHTTP](https://rexx.epbcn.com/rexxhttp/) HTTP test suite
unchanged, in place of mod_cgid, under both the event and the prefork MPM.
Apache starts, watches and restarts the host itself. The other phases, the
native and mod_rexx-compatible program classes, and Windows are still to
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

All of them except `OoRexxTimeout` are global: they cannot appear inside
`<VirtualHost>`.

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
with a single process replaced after every phase).

`tests/lifecycle.sh`, run after the suite, checks the host's life cycle:
graceful restarts with requests in flight, an upgraded module followed by a
graceful and a hard restart, the death of each kind of host process, growth
of the pool up to its maximum, and a stop with a program still running.

## License

Apache License 2.0. See [LICENSE](LICENSE).
