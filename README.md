# mod_ooRexx

An Apache HTTP Server module for [ooRexx](https://sourceforge.net/projects/oorexx/):
Rexx programs as CGI scripts without a fork per request, and, later, Rexx
code attached to the other phases of Apache's request cycle
(authentication, authorization, logging...), which a CGI cannot reach.

**Status: milestone 0.** The content handler `oorexx-cgi` works and passes
the full [RexxHTTP](https://rexx.epbcn.com/rexxhttp/) HTTP test suite
unchanged, in place of mod_cgid. Process management from Apache, the other
phases, the native and mod_rexx-compatible program classes, and Windows are
still to come. Not for production yet.

## Design in one page

ooRexx does not run Rexx code in parallel inside one process, not even in
separate interpreter instances: with two threads, throughput is about four
times worse than with two processes (measured). `.environment` is shared by
every instance of a process, and a native fault inside a process takes the
whole process down. Apache's default MPMs are threaded (event on Unix; one
single child process on Windows). Running Rexx inside Apache's workers
would therefore serialise it, mix the state of concurrent requests, and let
one failing program take a worker down with all its connections.

So mod_ooRexx is two pieces:

- **`mod_oorexx.so`**, a thin module inside Apache. It registers the hooks
  and talks to the host over a local socket. It does not link ooRexx.
- **`oorexx-host`**, a pool of processes outside Apache. Each process
  serves one phase of one request at a time, in a **fresh interpreter
  instance** (about 0.2–0.5 ms), with all of the program's I/O redirected
  to the connection. A watchdog halts a program that exceeds its deadline.
  A process is recycled after a number of requests. If one dies, the
  others, and Apache, are not affected.

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

## Building

Requirements:

- Apache 2.4 with `apxs` (Debian/Ubuntu: `apache2-dev`);
- ooRexx 5.2.0 or later with its API headers;
- a C and a C++ compiler.

```sh
make
sudo make install            # APXS=... OOREXX_PREFIX=... if needed
```

## Configuring (milestone 0)

Start the host as Apache's user. For now it is started by hand; starting it
from Apache comes next.

```sh
sudo mkdir -p /run/oorexx && sudo chown www-data: /run/oorexx && sudo chmod 700 /run/oorexx
sudo -u www-data oorexx-host --socket /run/oorexx/host.sock --processes 4 &
```

```apache
LoadModule oorexx_module modules/mod_oorexx.so
OoRexxSocket  /run/oorexx/host.sock
OoRexxTimeout 60

<Directory "/var/www/html/rexx">
    Options +ExecCGI
    AddHandler oorexx-cgi .rex
    Require all granted
</Directory>
```

## Testing

`tests/rexxhttp-suite.sh /path/to/rexxhttp` deploys the RexxHTTP suite,
swaps mod_cgid for mod_oorexx and runs the suite unchanged. Milestone 0
result: 182 tests, 313 assertions, 0 failures, 0 errors, empty error.log,
the same counts as under mod_cgid.

## License

Apache License 2.0. See [LICENSE](LICENSE).
