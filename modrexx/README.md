# The mod_rexx layer of mod_orx

[mod_rexx](https://sourceforge.net/projects/modrexx/) programs run
unchanged under mod_orx, in every phase, with mod_rexx's own configuration.
This layer is **optional**: a site without programs written for mod_rexx
does not load it, and sees nothing of mod_rexx in mod_orx.

It is two pieces, built only on the core's interfaces (see the main
[README](../README.md)):

- `mod_orx_rexx.so`, an Apache module: mod_rexx's directives, its handler,
  and its hooks in every phase;
- `orx_rexx_plugin.so` (on Windows, `orx_rexx_plugin.dll`), a plugin of
  `oorexx-host`: the `WWW*` variables and functions. The module tells
  mod_orx to have the host load it.

## Building and installing

After the core (`make && sudo make install`):

```sh
make modrexx
sudo make install-modrexx    # the plugin goes to PLUGINDIR, by default
                             # /usr/local/lib/mod_orx
```

On Windows, `build.cmd` builds the layer too when its third argument is
`modrexx`:

```bat
build.cmd C:\path\to\ooRexx C:\Apache24 modrexx
```

It leaves `mod_orx_rexx.so` and `orx_rexx_plugin.dll` in `build\`. Copy both
to Apache's `modules\`, where the module tells the host to look for the
plugin (`modules/orx_rexx_plugin.dll`, relative to `ServerRoot`).

## Configuring

A configuration written for mod_rexx works as it is. Only its
`LoadModule` line changes, for these two:

```apache
LoadModule orx_module      modules/mod_orx.so        # instead of mod_rexx.so
LoadModule orx_rexx_module modules/mod_orx_rexx.so

RexxSetVar SITE "My site"
RexxFixupHandler /srv/rexx/fixup.rex
<Directory "/var/www/html/rexx">
    AddHandler rexx-handler .rex
</Directory>
```

The two lines may come in either order. The layer's hooks run before the
core's own in every phase, whichever is loaded first. Against the other
modules, whose order the layer does not set, the order of `LoadModule`
decides, as it did for mod_rexx. In the type checking phase, for one, a
program sees only what `mod_mime` did not type if the layer is loaded after
`mod_mime`, as on Debian, where modules load in the order of their names.

On Windows the order of `LoadModule` does not decide: with the same
modules loaded in the same order as on Debian, `RexxAuthzHandler` ran before
`mod_auth_basic` there, and after it on Linux (measured). So on Windows the
layer names its predecessors in the three phases where the place matters:
`mod_alias` in the translation phase, `mod_mime` in the type checking phase,
and Apache's authentication modules (`mod_auth_basic`, `mod_auth_digest`,
`mod_auth_form`) in the authentication phase. That is mod_rexx's place on
Debian, and what the rest of this page describes.

mod_rexx itself cannot be loaded in the same Apache: the two define the
same directives.

| Directive | Meaning |
|---|---|
| `AddHandler rexx-handler` (or `rexx_handler`, `application/x-httpd-rexx-script`) | Run the requested file as a mod_rexx program. |
| `RexxHandler path` | The program the handler runs instead of the requested file. |
| `RexxPostReadRequestHandler`, `RexxTransHandler`, `RexxHeaderParserHandler`, `RexxAccessHandler`, `RexxAuthenHandler`, `RexxAuthzHandler`, `RexxTypeHandler`, `RexxFixupHandler`, `RexxLogHandler` | A program for that phase of the request. |
| `RexxSetVar name value` | A variable more for every program. |

They keep mod_rexx's meaning, oddities included:

- **Authentication and authorization are crossed.** `RexxAuthenHandler` runs
  in the authorization phase (`auth_checker`), and `RexxAuthzHandler` in
  the authentication phase (`check_user_id`).
- **Nothing is inherited.** A section with any `Rexx*` directive replaces
  all the `Rexx*` ones of the sections around it.
- **Program paths are taken as written.**

Under Apache 2.4 the crossing matters. With `AuthType Basic`, Apache's own
module takes the authentication phase, so `RexxAuthzHandler` never runs
there; and since no `WWW*` function sets the user, a program in that phase
can only refuse. The program that can decide on a known user is
`RexxAuthenHandler`. Correcting the crossing would have let any valid user
through where such a program refuses today, so it stays.

`RexxRspCompiler` and `RexxTempFileNameTemplate` (RSP pages) are not
supported, and a configuration that has them does not start.

`OoRexxTimeout` (the core's) applies to these programs as to any other.

## What a program gets

A program gets its request in `arg(1)`; the `WWW*` variables exist before
its first line; the `WWW*` functions are there without being declared; and
what it returns is the phase's status (`0` OK, `-1` declined, `-2` done, or
an HTTP status). Its own `Apache.cls` keeps working, since it only calls
those functions.

Outside the content phase, as under mod_rexx, nothing runs in a
subrequest, what the program says goes nowhere, and an error in it is a 500
with the error in the error log. The request body is read only if the
program asks for it (`WWWGetArgs` on a POST, say), and is then gone for the
content handler, as under mod_rexx.

Every variable and function behaves as under mod_rexx 2.4.0, its last
version, measured on the same programs and the same configuration under
both (`tests/modrexx.sh`, `tests/modrexx-phases.sh`), down to its
oddities:

- A variable is set with its name exactly as written. So a form field
  `user` is `WWWARGS.user` (found with `k = 'user'; say wwwargs.k`), and a
  `RexxSetVar` name in lowercase is not visible to the program.
- Apache holds a small response until the end, so a status returned after
  some output still replaces it.
- A `HEAD` request is answered without running the program.
- The program runs in `/` (on Windows, the root of Apache's drive), and a
  command goes to the environment `WWW`, which does not exist (RC -3), not
  to a shell.

The differences:

- The request is an opaque handle (`orx:` and 16 hex digits), not a
  pointer. A value that is not this request's handle (or its server's, or
  its connection's, where those are expected) is Error 40.1 in the program;
  under mod_rexx it crashed Apache's process.
- `RexxPostReadRequestHandler` and `RexxTransHandler` work. Under mod_rexx
  2.4.0 and Apache 2.4 they crashed Apache's process on every request,
  whatever the program: mod_rexx looked up a path info that does not exist
  yet in those phases. There, `WWWPATH_TRANSLATED` is the request's
  directory, and `WWWFILENAME` is empty until the translation.
- `WWWInternal_Redirect` works only in the content phase; anywhere else it
  is Error 40.1, with a line in the error log saying why. Under mod_rexx it
  crashed Apache's process. It is done when the program ends, and what the
  program wrote is dropped, as mod_rexx's output was.
- All the program's output reaches the client (`Say`, `.output`,
  `lineout`, `charout`...), not only `Say`. The request body can also be
  read as input, and `WWWGetArgs` then gets what is left of it. A chunked
  body is accepted.
- `WWWGetArgs` sets `WWWARGS.0` to 0 when it has nothing to parse (a POST
  that is not urlencoded, a PUT...). mod_rexx left it unset, and the usual
  `do i = 1 to wwwargs.0` failed.
- An error in the program is Apache's usual 500 page, and the error, with
  its line, goes to the error log in one line. mod_rexx sent a page of its
  own with the program's path and the error.
- `WWWSERVER_SOFTWARE` does not include `Mod_Rexx/2.4.0`, which mod_rexx
  added to Apache's own version string. `WWWGetVersion` and
  `WWWGATEWAY_INTERFACE` say `Mod_Rexx/2.4.0`, in case a program checks.
- The program's process environment is the CGI environment.
- An internal redirect to a program (`ErrorDocument 404 /error.rex`, an
  `Action`) works. Under mod_rexx it crashed Apache's process, for the same
  reason as its `PostReadRequest` and `Trans` phases.
- `WWWReqRecNext`, `WWWReqRecPrev` and `WWWReqRecMain` give a handle to the
  related request, or `(nil)` when there is none, as mod_rexx did. The
  related request can be read with the same functions, but not changed
  (Error 40.1). So a page for an `ErrorDocument` finds the request that
  failed with `WWWReqRecPrev`, and a `RexxLogHandler` finds where a request
  was redirected to with `WWWReqRecNext`.
- The subrequest functions (`WWWSub_Req_Lookup_URI`,
  `WWWSub_Req_Lookup_File`, `WWWRun_Sub_Req`) are not there: a call to one
  is Error 43, routine not found. Under mod_rexx no program could use them:
  they returned the request's pointer in binary, which no other function
  took (and their documentation gives the arguments in the wrong order).

On Windows, two things are not as on Linux, and neither comes from the
layer:

- `WWWHTTP_time` ends with the time zone as the C library names it: an
  abbreviation on Unix (`CET`), usually the whole name on Windows.
- A program that gives the request another file after Apache has looked at
  the first one (`WWWReqRecFilename` in the header parser or the fixup
  phase, say) gets, on Linux, the new file sent with the old one's size;
  on Windows, Apache sends an empty reply (measured).

## Testing

Both tests run after `tests/rexxhttp-suite.sh` (see the main README) and
after `make install-modrexx`. They load the layer for their run and take it
out again; with `ORX_REXX_FIRST=1`, they load it just before mod_orx instead
of after it.

`tests/modrexx.sh` checks the content handler against the answers of
mod_rexx 2.4.0 itself, measured on the same programs: every initial
variable, every `WWW*` function, every write and what a later read sees,
and what each kind of return value makes of the response.

`tests/modrexx-phases.sh` checks programs in every phase, with one
configuration:

- what each phase sees;
- what its return values and its writes make of the request;
- authentication with `AuthType Basic` and with an `AuthType` of its own;
- sections that replace the outer directives;
- the body, and the error log.

The answers are those of mod_rexx 2.4.0 itself, except where it crashed. A
logging hook of the core (`OoRexxLogHook`) runs next to `RexxLogHandler` in
every case, and its line must come after the mod_rexx program's.

On Windows, `tests\modrexx.ps1 -Apache C:\Apache24 -OoRexx C:\ooRexx` is
both tests: it deploys what `build.cmd ... modrexx` built into an Apache
for Windows, with the modules Debian loads, and runs the same programs and
the same cases against the same expected answers, once with the layer
loaded after mod_orx and once before. The checks are the shell scripts the
Linux tests use (`tests/modrexx/checks.sh`, `tests/modrexx/run-cases.sh`),
run under Git for Windows' bash. Of the two cases Apache for Windows
answers with an empty reply (above), only what the phase programs saw is
compared. `.github/workflows/windows.yml` runs it on Windows Server 2022.
