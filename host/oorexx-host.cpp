/*
 * oorexx-host -- runs Rexx programs on behalf of mod_orx.
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * Process model (see the design notes in README.md):
 *
 *   - A master process forks a pool of children on one Unix socket, and
 *     grows and shrinks it between a minimum and a maximum. The master
 *     never touches ooRexx: it only keeps the pool alive. Normally
 *     mod_orx starts the master, with a socket it has already bound
 *     (--listen-fd); for debugging it can be started by hand (--socket).
 *   - Every child accepts on the SAME socket; the kernel hands each
 *     connection to an idle child. A child serves one connection at a time.
 *   - One connection = one phase of one request. For each one the child
 *     creates a FRESH interpreter instance, redirects all of the program's
 *     I/O to the connection, runs the program, and terminates the instance.
 *     ooRexx serialises all Rexx execution inside a process, so parallelism
 *     comes from having several processes, never several threads.
 *   - A child exits after --max-requests connections (the master replaces
 *     it): .environment is process-global and survives instances, so this
 *     bounds what a program can leak to later ones.
 *   - Phases: content (a CGI program), every other phase (a native program),
 *     and whatever a plugin serves (--plugin; see orx_host_plugin.h): the
 *     optional mod_rexx layer runs programs written for mod_rexx in every
 *     phase.
 *     A native program gets the CGI environment, its arguments (ARGS), and
 *     the APACHE command environment (apache.rex) on the snapshot of the
 *     request; it returns a value that the module reads as its verdict. Its
 *     output has no client to go to, so it goes to the error log, like
 *     .error.
 *   - A watchdog thread Halt()s the instance when the phase's deadline
 *     passes. It only ever uses the INSTANCE API: using a thread context
 *     from a thread other than its own aborts the process (measured).
 *
 *   - Every child keeps one instance alive (the ANCHOR). While an instance
 *     lives, the interpreter does not reset between phases (measured:
 *     RexxHTTP's test page, 7.3 -> 4.0 ms). The anchor loads, once, the
 *     APACHE environment (its class is held and used by every native phase)
 *     and, with --preload, the given packages: a program that ::Requires one
 *     of them finds it already loaded (measured: RexxHTTP's four classes,
 *     4.6 ms -> 0.34 ms per phase). Only what the anchor loaded is shared; a
 *     program's own packages are still loaded afresh in every phase. A
 *     changed preloaded file is seen only by new processes (a restart of
 *     Apache, or recycling).
 *
 *   - SIGTERM drains: no child accepts anything new, the children finish
 *     what they hold and what is queued, and the master exits; after
 *     --grace seconds whatever is left is killed.
 *
 *   - On Windows the same model, with CreateProcess, events and a file
 *     mapping in place of fork, signals and a shared page, and an accept
 *     mutex (see "The pool on Windows" below).
 *
 * Usage:
 *   oorexx-host (--socket PATH | --listen-fd N) [--processes MIN]
 *               [--max-processes MAX] [--max-requests N] [--grace SECONDS]
 *               [--preload FILE]... [--plugin CLASS:PATH]...
 */

#ifdef _WIN32
/* Windows (10 1803 or Server 2019 at least: AF_UNIX). Winsock before       */
/* anything that brings in windows.h, oorexxapi.h included.                 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#endif

#include <oorexxapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <strings.h>
#endif
/* Linux kills or warns a process when its parent dies (PR_SET_PDEATHSIG). */
/* Elsewhere (macOS, the BSDs), the keeper and the master watch their       */
/* parent themselves, and the children already end with the stop pipe (see  */
/* child_main). -DORX_PORTABLE builds those paths on Linux too, to test     */
/* them there.                                                              */
#if defined(__linux__) && !defined(ORX_PORTABLE)
#include <sys/prctl.h>
#define ORX_PDEATHSIG 1
#endif

#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

extern "C" {
#include "../include/oorexx_protocol.h"
}
#include "../include/orx_host_plugin.h"

/* The plugin of each class that has one (--plugin).                        */
#define MAX_CLASSES 16
static const orx_host_plugin *g_plugin[MAX_CLASSES];

/* ------------------------------------------------------------------------ */
/* Per-connection state. A child serves one connection at a time, so plain  */
/* globals are the honest representation.                                   */
/* ------------------------------------------------------------------------ */

static oorx_sock_t g_fd = OORX_NO_SOCK; /* the connection                   */
static std::string g_in;               /* body bytes received, not consumed */
static bool        g_in_eof = false;   /* the module said: no more body     */
static volatile bool g_broken = false; /* the connection failed: stop I/O   */

/* Two threads write frames on the connection: the main thread (READ, LOG,
 * RESULT) and the output pump (DATA). Only the main thread ever reads. A
 * frame is written under this lock, so frames never interleave.            */
static std::mutex g_send_lock;

static void send_frame(int type, int flags, const void *p, uint32_t n)
{
    if (g_broken) return;
    std::lock_guard<std::mutex> hold(g_send_lock);
    if (oorx_send(g_fd, type, flags, p, n) < 0) g_broken = true;
}

/* Leave the process at once: no atexit handlers, no flushing. On Windows,  */
/* without detaching DLLs either: rexx.dll, with the anchor instance alive  */
/* and the watchdog thread waiting, must not be asked to clean up.          */
static void quit(int code)
{
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _exit(code);
}

/* A whole message to standard error, in one write: when every child fails  */
/* alike, the lines must not mix.                                           */
static void err_write(const std::string &m)
{
#ifdef _WIN32
    DWORD n;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), m.data(), (DWORD)m.size(), &n, NULL);
#else
    if (write(2, m.data(), m.size()) < 0) { /* nowhere else to say it */ }
#endif
}

#ifdef _WIN32
/* A child that cannot go on says why, on its way out (the error log, when  */
/* mod_orx started the host).                                               */
/* OORX_TRACE=1 in the environment: every step of a child, on stderr.     */
static bool g_trace = false;
static void trace(const char *fmt, long long a = 0, long long b = 0)
{
    if (!g_trace) return;
    char m[200], t[160];
    snprintf(t, sizeof t, fmt, a, b);
    snprintf(m, sizeof m, "oorexx-host trace: %lu %llu: %s\n", GetCurrentProcessId(),
             (unsigned long long)GetTickCount64(), t);
    err_write(m);
}
#define TRACE(...) trace(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif
#ifdef _WIN32
static void die(const char *why)
{
    char m[200];
    snprintf(m, sizeof m, "oorexx-host: child %lu: %s (error %lu)\n", GetCurrentProcessId(),
             why, GetLastError());
    err_write(m);
    quit(1);
}
#endif

static void send_log(int level, const char *msg, size_t n)
{
    std::string payload(1, (char)level);
    payload.append(msg, n);
    send_frame(OORX_LOG, 0, payload.data(), (uint32_t)payload.size());
}

/* The process environment, which a program reads (Value(..., 'ENVIRONMENT')). */
/* On Windows, the Win32 one: rexx.dll has a C runtime of its own, and reads  */
/* the environment through the Win32 API, not through any C runtime's copy.   */
static void env_set(const std::string &k, const std::string &v)
{
#ifdef _WIN32
    SetEnvironmentVariableA(k.c_str(), v.c_str());
#else
    setenv(k.c_str(), v.c_str(), 1);
#endif
}

/* clearenv() is glibc's; elsewhere, unset every variable by name. On       */
/* Windows the names that start with "=" (a drive's current directory) are  */
/* not variables, and stay.                                                 */
static void env_clear(void)
{
#if defined(_WIN32)
    std::vector<std::string> names;
    char *block = GetEnvironmentStringsA();
    for (char *e = block; e && *e; e += strlen(e) + 1) {
        const char *eq = strchr(e + 1, '=');
        if (*e != '=') names.push_back(eq ? std::string(e, eq - e) : std::string(e));
    }
    if (block) FreeEnvironmentStringsA(block);
    for (size_t i = 0; i < names.size(); i++) SetEnvironmentVariableA(names[i].c_str(), NULL);
#elif defined(__GLIBC__) && !defined(ORX_PORTABLE)
    clearenv();
#else
    extern char **environ;
    std::vector<std::string> names;
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        names.push_back(eq ? std::string(*e, eq - *e) : std::string(*e));
    }
    for (size_t i = 0; i < names.size(); i++) unsetenv(names[i].c_str());
#endif
}

/* Run in the program's directory, as mod_cgi does.                         */
static void change_dir(const std::string &dir)
{
    if (dir.empty()) return;
#ifdef _WIN32
    SetCurrentDirectoryA(dir.c_str());
#else
    if (chdir(dir.c_str()) < 0) { /* not fatal */ }
#endif
}

/* The output pump. The program's output is a genuine .Stream (RexxHTTP, for
 * one, requires exactly that class, as a CGI always has) opened on the
 * write end of a pipe. This thread forwards whatever arrives on the read
 * end as DATA. Like mod_cgi, it asks for a FLUSH only when the program
 * goes quiet: if nothing more (data or EOF) arrives within QUIET_MS, the
 * client gets what there is now -- that is how an explicit flush in the
 * middle of a long response reaches the client early (lazy headers,
 * RexxHTTP invariant 3). A response written in one go reaches EOF within
 * the window, is never flushed, and Apache can give it a Content-Length.
 * It ends at EOF, when the stream and our copy are closed.                 */
/* A plugin's policy may turn this off (the mod_rexx layer does: mod_rexx  */
/* had no way to flush, and Apache holds a small response until the end,    */
/* where what the program returns can still change its status).            */
#define QUIET_MS 10
static bool g_quiet_flush = true;

/* The program's output channel for one phase: out_open gives the name the  */
/* bootstrap opens as a .Stream and starts the pump; out_close, once the    */
/* program has ended and its stream is closed, waits until the pump has     */
/* forwarded everything.                                                    */
struct out_channel {
    std::thread pump;
#ifndef _WIN32
    int wfd;                           /* the write end, the program's      */
#endif
};

#ifndef _WIN32
static void output_pump(int rfd)
{
    char buf[OORX_CHUNK];
    for (;;) {
        ssize_t n = read(rfd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        int more = 1;
        if (g_quiet_flush) {
            struct pollfd pf = { rfd, POLLIN, 0 };
            more = poll(&pf, 1, QUIET_MS) > 0;
        }
        send_frame(OORX_DATA, more ? 0 : OORX_FLAG_FLUSH, buf, (uint32_t)n);
    }
    close(rfd);
}

/* A pipe per phase, opened as /dev/fd/N: its EOF is the end of the output. */
static bool out_open(out_channel &o, std::string &name)
{
    int pfd[2];
    if (pipe(pfd) < 0) return false;
    o.wfd = pfd[1];
    o.pump = std::thread(output_pump, pfd[0]);
    char path[32];
    snprintf(path, sizeof path, "/dev/fd/%d", pfd[1]);
    name = path;
    return true;
}

static void out_close(out_channel &o)
{
    close(o.wfd);
    o.pump.join();                     /* all output forwarded */
}
#else
/* On Windows the program writes to 'STDOUT': .Stream will not open a pipe  */
/* by name, and rexx.dll carries its own C runtime, so the output must be   */
/* the process's standard output handle from birth. The master makes it the */
/* write end of a pipe that lasts as long as the child, and hands the child */
/* the read end too, opened for overlapped I/O. One read is always pending  */
/* on it, from one phase to the next; so the pump is one thread for the     */
/* child's whole life, since Windows cancels a thread's pending I/O when    */
/* the thread ends (measured: ERROR_OPERATION_ABORTED in the next phase).   */
/*                                                                          */
/* There is no EOF per phase. The end is safe to tell once the program has  */
/* ended (g_phase_done): every write it made has returned, so what it wrote */
/* is either in the pending read, which then completes, or in the pipe. So  */
/* the pump reads on until the pipe is empty AND the pending read has not   */
/* completed -- in that order, since a read may complete between the two   */
/* checks, but nothing can enter the pipe any more.                         */
static HANDLE     g_out_r = INVALID_HANDLE_VALUE;
static HANDLE     g_phase_start;       /* auto-reset: a phase has begun     */
static HANDLE     g_phase_done;        /* manual: the program has ended     */
static HANDLE     g_phase_flushed;     /* auto-reset: all of it forwarded   */
static OVERLAPPED g_ov;
static char       g_obuf[OORX_CHUNK];
static bool       g_read_pending = false;

/* Start the next read. False if the pipe is broken (the child cannot go on). */
static bool start_read(void)
{
    if (g_read_pending) return true;
    ResetEvent(g_ov.hEvent);
    if (!ReadFile(g_out_r, g_obuf, sizeof g_obuf, NULL, &g_ov) &&
        GetLastError() != ERROR_IO_PENDING)
        return false;
    g_read_pending = true;             /* even if it completed at once: the */
    return true;                       /* event is set, and is seen next    */
}

/* The pending read has completed: its bytes, or -1 if the pipe broke.      */
static long finish_read(void)
{
    DWORD n = 0;
    g_read_pending = false;
    if (!GetOverlappedResult(g_out_r, &g_ov, &n, FALSE)) return -1;
    return (long)n;
}

static void output_pump(void)
{
    std::string held;                  /* the last chunk, not sent yet: it  */
    bool have = false;                 /* goes with FLUSH if a quiet gap    */
    bool ending = false;               /* follows, without it otherwise     */
    for (;;) {
        if (!start_read()) die("cannot read the program's output");
        DWORD w;
        if (ending) {
            /* The program has ended: done when the pipe is empty and the   */
            /* pending read has not completed (checked in that order).      */
            DWORD avail = 0;
            if (!PeekNamedPipe(g_out_r, NULL, 0, NULL, &avail, NULL)) die("cannot peek at the output pipe");
            if (avail == 0 && WaitForSingleObject(g_ov.hEvent, 0) == WAIT_TIMEOUT) break;
            w = WaitForSingleObject(g_ov.hEvent, INFINITE);
        } else {
            HANDLE hs[2] = { g_ov.hEvent, g_phase_done };
            w = WaitForMultipleObjects(2, hs, FALSE,
                                       have && g_quiet_flush ? QUIET_MS : INFINITE);
        }
        if (w == WAIT_OBJECT_0) {
            long n = finish_read();
            if (n < 0) die("a read of the program's output failed");
            if (n == 0) continue;
            if (have) send_frame(OORX_DATA, 0, held.data(), (uint32_t)held.size());
            held.assign(g_obuf, (size_t)n);
            have = true;
        } else if (w == WAIT_TIMEOUT) {
            send_frame(OORX_DATA, OORX_FLAG_FLUSH, held.data(), (uint32_t)held.size());
            have = false;
        } else if (w == WAIT_OBJECT_0 + 1) {
            ending = true;
        } else die("waiting for the program's output failed");
    }
    if (have) send_frame(OORX_DATA, 0, held.data(), (uint32_t)held.size());
}

/* The pump's thread: one phase after another.                              */
static void pump_thread(void)
{
    for (;;) {
        WaitForSingleObject(g_phase_start, INFINITE);
        output_pump();
        SetEvent(g_phase_flushed);
    }
}

static bool out_open(out_channel &o, std::string &name)
{
    (void)o;
    ResetEvent(g_phase_done);
    SetEvent(g_phase_start);
    name = "STDOUT";
    return true;
}

static void out_close(out_channel &o)
{
    (void)o;
    SetEvent(g_phase_done);
    WaitForSingleObject(g_phase_flushed, INFINITE);   /* all output forwarded */
}
#endif

/* Ask the module for more body. Returns false at EOF or on failure.        */
static bool fill_in(size_t want)
{
    if (g_in_eof || g_broken) return false;
    unsigned char n[4];
    oorx_put_u32(n, (uint32_t)(want < OORX_CHUNK ? OORX_CHUNK : want));
    send_frame(OORX_READ, 0, n, 4);
    if (g_broken) return false;
    int type, flags; uint32_t len;
    if (oorx_recv_header(g_fd, &type, &flags, &len) < 0 || type != OORX_BODY)
        { g_broken = true; return false; }
    if (len == 0) { g_in_eof = true; return false; }
    size_t old = g_in.size();
    g_in.resize(old + len);
    if (oorx_read_all(g_fd, &g_in[old], len) < 0) { g_broken = true; return false; }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Native routines, registered as the library OORXHOST in every instance.   */
/* ------------------------------------------------------------------------ */

RexxRoutine1(int, oorx_log, RexxStringObject, s)
{
    send_log(OORX_LOG_ERR, context->StringData(s), context->StringLength(s));
    return 0;
}

/* Up to n bytes; fewer only at EOF (like CharIn on a pipe).                */
RexxRoutine1(RexxStringObject, oorx_read, wholenumber_t, n)
{
    if (n < 0) n = 0;
    while (g_in.size() < (size_t)n && fill_in((size_t)n - g_in.size()))
        ;
    size_t take = g_in.size() < (size_t)n ? g_in.size() : (size_t)n;
    RexxStringObject r = context->NewString(g_in.data(), take);
    g_in.erase(0, take);
    return r;
}

/* One line without its terminator ("" at EOF; see oorx_more).              */
RexxRoutine0(RexxStringObject, oorx_readline)
{
    size_t nl;
    while ((nl = g_in.find('\n')) == std::string::npos && fill_in(OORX_CHUNK))
        ;
    size_t take = nl == std::string::npos ? g_in.size() : nl;
    size_t line = take;
    if (line > 0 && g_in[line - 1] == '\r') line--;
    RexxStringObject r = context->NewString(g_in.data(), line);
    g_in.erase(0, nl == std::string::npos ? take : take + 1);
    return r;
}

/* 1 if more body may be read, 0 at EOF.                                    */
RexxRoutine0(int, oorx_more)
{
    if (!g_in.empty()) return 1;
    return fill_in(OORX_CHUNK) ? 1 : 0;
}

/* The routines of the APACHE environment (below, after the snapshot).    */
REXX_TYPED_ROUTINE_PROTOTYPE(oorx_field);
REXX_TYPED_ROUTINE_PROTOTYPE(oorx_entries);
REXX_TYPED_ROUTINE_PROTOTYPE(oorx_table);
REXX_TYPED_ROUTINE_PROTOTYPE(oorx_update);
REXX_TYPED_ROUTINE_PROTOTYPE(oorx_logl);

static RexxRoutineEntry host_routines[] = {
    REXX_TYPED_ROUTINE(oorx_log,      oorx_log),
    REXX_TYPED_ROUTINE(oorx_read,     oorx_read),
    REXX_TYPED_ROUTINE(oorx_readline, oorx_readline),
    REXX_TYPED_ROUTINE(oorx_more,     oorx_more),
    REXX_TYPED_ROUTINE(oorx_field,    oorx_field),
    REXX_TYPED_ROUTINE(oorx_entries,  oorx_entries),
    REXX_TYPED_ROUTINE(oorx_table,    oorx_table),
    REXX_TYPED_ROUTINE(oorx_update,   oorx_update),
    REXX_TYPED_ROUTINE(oorx_logl,     oorx_logl),
    REXX_LAST_ROUTINE()
};

static RexxPackageEntry host_package = {
    STANDARD_PACKAGE_HEADER
    REXX_INTERPRETER_5_0_0,
    "OORXHOST", "0.0.0",
    NULL, NULL,
    host_routines,
    NULL
};

/* ------------------------------------------------------------------------ */
/* The bootstrap: run in each fresh instance before the program, with the  */
/* path of the output pipe and a hook flag as its arguments. It points     */
/* every standard stream at the connection: output (Say, lineout(),        */
/* charout(), .output, .stdout) to a real .Stream on the pipe -- or, in a  */
/* hook, to the error log, since a hook has no client; input (parse pull,  */
/* linein(), charin(), .stdin) to the request body; .error, .stderr and    */
/* trace output to Apache's error log. .stdout and friends are             */
/* process-wide objects, so they must be replaced in .local, not only      */
/* behind the monitors (measured). Returns the output stream, which the    */
/* host closes at the end.                                                 */
/* ------------------------------------------------------------------------ */

static const char bootstrap_src[] =
"use strict arg pipe, hook\n"
"out = .Stream~new(pipe)\n"
"if out~open('WRITE APPEND') \\= 'READY:' then raise syntax 98.900 array('cannot open' pipe)\n"
"in  = .OoRexxIn~new\n"
"err = .OoRexxErr~new\n"
"dest = out; if hook then dest = err\n"
".output~destination(dest); .local~stdout = dest\n"
".input~destination(in);   .local~stdin  = in\n"
".error~destination(err);  .local~stderr = err\n"
".traceOutput~destination(err)\n"
"return out\n"
"\n"
"::routine oorx_log      external \"LIBRARY OORXHOST oorx_log\"\n"
"::routine oorx_read     external \"LIBRARY OORXHOST oorx_read\"\n"
"::routine oorx_readline external \"LIBRARY OORXHOST oorx_readline\"\n"
"::routine oorx_more     external \"LIBRARY OORXHOST oorx_more\"\n"
"\n"
"::class OoRexxIn subclass InputStream\n"
"::method charin\n"
"  use arg , len = 1\n"
"  data = oorx_read(len)\n"
"  if data~length < len then raise notready return (data)\n"
"  return data\n"
"::method linein\n"
"  if \\oorx_more() then raise notready return ('')\n"
"  return oorx_readline()\n"
"::method chars;       return oorx_more()\n"
"::method lines;       return oorx_more()\n"
"::method state;       if oorx_more() then return 'READY'; return 'NOTREADY'\n"
"::method description; return self~state':'\n"
"::method close;       return 'READY:'\n"
"::method open;        return 'READY:'\n"
"::method qualify;     return 'STDIN'\n"
"\n"
"::class OoRexxErr subclass OutputStream\n"
"::method charout\n"
"  if arg(1, 'O') then return 0\n"
"  call oorx_log arg(1); return 0\n"
"::method lineout\n"
"  if arg(1, 'O') then return 0\n"
"  call oorx_log arg(1); return 0\n"
"::method say;         call oorx_log arg(1); return 0\n"
"::method flush;       return 0\n"
"::method close;       return 'READY:'\n"
"::method open;        return 'READY:'\n"
"::method state;       return 'READY'\n"
"::method description; return 'READY:'\n"
"::method qualify;     return 'STDERR'\n";

/* ------------------------------------------------------------------------ */
/* Watchdog: Halt() the instance when the deadline passes.                  */
/* ------------------------------------------------------------------------ */

typedef std::chrono::steady_clock wd_clock;
static std::mutex              wd_lock;
static std::condition_variable wd_cond;
static RexxInstance           *wd_instance = NULL;
static wd_clock::time_point    wd_deadline;
static bool                    wd_armed = false;
static bool                    wd_fired = false;

static void watchdog(void)
{
    std::unique_lock<std::mutex> hold(wd_lock);
    for (;;) {
        if (!wd_armed) { wd_cond.wait(hold); continue; }
        wd_cond.wait_until(hold, wd_deadline);
        if (wd_armed && wd_clock::now() >= wd_deadline) {
            wd_armed = false;
            wd_fired = true;
            wd_instance->Halt();       /* instance API: safe from here */
        }
    }
}

static void wd_arm(RexxInstance *inst, unsigned seconds)
{
    std::lock_guard<std::mutex> hold(wd_lock);
    wd_deadline = wd_clock::now() + std::chrono::seconds(seconds);
    wd_instance = inst; wd_armed = seconds > 0; wd_fired = false;
    wd_cond.notify_one();
}

static bool wd_disarm(void)
{
    std::lock_guard<std::mutex> hold(wd_lock);
    bool fired = wd_fired;
    wd_armed = false; wd_instance = NULL;
    wd_cond.notify_one();
    return fired;
}

/* ------------------------------------------------------------------------ */
/* The snapshot of the request, and what the host offers to its plugins     */
/*                                                                          */
/* With flag SNAPSHOT, VARS is the module's snapshot of the request and of  */
/* the requests related to it (keys in oorexx_protocol.h). The host keeps   */
/* it for the phase: a plugin reads it, and a change the program makes      */
/* changes it too (a later read sees it) before it goes to the module as an */
/* UPDATE. The "+" keys are the client's own; the host only keeps them, in  */
/* order, for the plugin.                                                   */
/* ------------------------------------------------------------------------ */

/* A table as Apache keeps it: entries in their order, names as they were  */
/* written, several entries with one name; names compare without case. The  */
/* first entry with a name is the one apr_table_get gives.                  */
typedef std::vector<std::pair<std::string, std::string> > table;

static bool same_name(const std::string &a, const char *b) { return !strcasecmp(a.c_str(), b); }

/* apr_table_set, apr_table_add and apr_table_unset, on our copy: set keeps */
/* the first entry (and its name as written) with the new value, and drops  */
/* the others.                                                              */
static void table_apply(table &t, char action, const std::string &k, const std::string &v)
{
    bool kept = false;
    if (action == 'a') { t.push_back(std::make_pair(k, v)); return; }
    for (table::iterator e = t.begin(); e != t.end(); ) {
        if (!same_name(e->first, k.c_str())) { ++e; continue; }
        if (action == 's' && !kept) { e->second = v; kept = true; ++e; continue; }
        e = t.erase(e);
    }
    if (action == 's' && !kept) t.push_back(std::make_pair(k, v));
}

struct snap_request {
    std::map<std::string, std::string> r;   /* fields                       */
    table t[5];                             /* i o x e n                    */
};

static const char g_tables[] = "ioxen";

static struct {
    std::vector<snap_request> req;          /* [0] the phase's request      */
    std::vector<std::pair<std::string, std::string> > client;   /* "+" keys */
} g_snap;

static int table_index(char t)
{
    const char *p = t ? strchr(g_tables, t) : NULL;
    return p ? (int)(p - g_tables) : -1;
}

/* One key of the snapshot.                                                 */
static void snap_load(const std::string &k, const std::string &v)
{
    if (k.empty()) return;
    if (k[0] == '+') { g_snap.client.push_back(std::make_pair(k.substr(1), v)); return; }
    size_t q = 0, p = 0;
    if (k.compare(0, 2, "q:") == 0) {       /* q:N then a kind, ':', name   */
        p = 2;
        while (p < k.size() && isdigit((unsigned char)k[p])) q = q * 10 + (k[p++] - '0');
        if (q == 0 || q >= OORX_MAX_RELATED) return;
    }
    if (p + 2 > k.size() || k[p + 1] != ':') return;
    char kind = k[p];
    std::string name = k.substr(p + 2);
    if (kind == 'E') { if (!q) env_set(name, v); return; }
    if (g_snap.req.size() <= q) g_snap.req.resize(q + 1);
    if (kind == 'r') { g_snap.req[q].r[name] = v; return; }
    int t = table_index(kind);
    if (t >= 0) g_snap.req[q].t[t].push_back(std::make_pair(name, v));
}

static void send_update(const char *op, const std::string &key, const std::string &val)
{
    std::string p;
    unsigned char n[4];
    const std::string f[3] = { op, key, val };
    for (int i = 0; i < 3; i++) {
        oorx_put_u32(n, (uint32_t)f[i].size());
        p.append((const char *)n, 4);
        p += f[i];
    }
    send_frame(OORX_UPDATE, 0, p.data(), (uint32_t)p.size());
}

/* The functions a plugin gets (orx_host_plugin.h).                         */
static const char *api_field(int q, const char *name, size_t *len)
{
    if (q < 0 || (size_t)q >= g_snap.req.size()) return NULL;
    std::map<std::string, std::string>::const_iterator f = g_snap.req[q].r.find(name);
    if (f == g_snap.req[q].r.end()) return NULL;
    if (len) *len = f->second.size();
    return f->second.c_str();
}

static const char *api_table_get(int q, char t, const char *key, size_t *len)
{
    int i = table_index(t);
    if (i < 0 || q < 0 || (size_t)q >= g_snap.req.size()) return NULL;
    const table &tb = g_snap.req[q].t[i];
    for (table::const_iterator e = tb.begin(); e != tb.end(); ++e)
        if (same_name(e->first, key)) {
            if (len) *len = e->second.size();
            return e->second.c_str();
        }
    return NULL;
}

static void api_table_each(int q, char t, void *ctx, orx_kv_fn *fn)
{
    int i = table_index(t);
    if (i < 0 || q < 0 || (size_t)q >= g_snap.req.size()) return;
    const table &tb = g_snap.req[q].t[i];
    for (table::const_iterator e = tb.begin(); e != tb.end(); ++e)
        fn(ctx, e->first.data(), e->first.size(), e->second.data(), e->second.size());
}

static int api_related(void)
{
    return g_snap.req.empty() ? 0 : (int)g_snap.req.size() - 1;
}

static void api_client_each(void *ctx, orx_kv_fn *fn)
{
    for (size_t i = 0; i < g_snap.client.size(); i++)
        fn(ctx, g_snap.client[i].first.data(), g_snap.client[i].first.size(),
           g_snap.client[i].second.data(), g_snap.client[i].second.size());
}

/* The field each core field operation changes in the snapshot (the ones    */
/* that are not in it, like status_line, only go to the module).           */
static const char *op_field(char op)
{
    switch (op) {
    case 'S': return "status";        case 'M': return "method";
    case 'T': return "content_type";  case 'E': return "content_encoding";
    case 'H': return "handler";       case 'G': return "content_languages";
    case 'U': return "uri";           case 'F': return "filename";
    case 'P': return "path_info";     case 'Q': return "args";
    case 'K': return "content_languages";
    case 'R': return "user";
    }
    return NULL;
}

static void api_update(const char *op, const char *key, const char *value, size_t vl)
{
    std::string k = key ? key : "", v(value ? value : "", value ? vl : 0);
    if (op[0] != '+' && oorx_op_index(op) >= 0) {
        if (g_snap.req.empty()) g_snap.req.resize(1);
        snap_request &s = g_snap.req[0];
        if (op[1]) table_apply(s.t[table_index(op[1])], op[0], k, v);
        else if (const char *f = op_field(op[0])) {
            std::string &cur = s.r[f];
            if (op[0] == 'G') cur = cur.empty() ? v : cur + "," + v;   /* a list */
            else cur = v;
        }
    }
    send_update(op, k, v);
}

static void api_log(int level, const char *msg, size_t len) { send_log(level, msg, len); }

static size_t api_read_body(char *buf, size_t max)
{
    if (g_in.empty()) fill_in(max);
    size_t take = g_in.size() < max ? g_in.size() : max;
    memcpy(buf, g_in.data(), take);
    g_in.erase(0, take);
    return take;
}

static const orx_host_api g_api = {
    ORX_PLUGIN_ABI,
    api_field, api_table_get, api_table_each, api_related, api_client_each,
    api_update, api_log, api_read_body
};

/* ------------------------------------------------------------------------ */
/* The APACHE environment of a native program (Address APACHE)              */
/*                                                                          */
/* The environment itself is written in Rexx (host/apache.rex, built in as  */
/* apache_src): a .CommandHandler, registered in the instance of every      */
/* phase of a native program. These routines are all it needs from here:   */
/* reading the snapshot, sending a change, and logging at a level.          */
/* ------------------------------------------------------------------------ */

#include "../build/apache_src.h"

/* The anchor loads the package once, and keeps its class (a global        */
/* reference): every phase of a native program registers an instance of it. */
static RexxObjectPtr g_apache_class = NULLOBJECT;
static const char apache_register_src[] =
    "use strict arg class, phase\n"
    ".CommandHandler~register('APACHE', class~new(phase))\n";

/* A field of request q, or .nil if q is not in the snapshot.               */
RexxRoutine2(RexxObjectPtr, oorx_field, wholenumber_t, q, CSTRING, name)
{
    size_t n = 0;
    const char *v = api_field((int)q, name, &n);
    if (v) return context->NewString(v, n);
    if (q >= 0 && (size_t)q < g_snap.req.size()) return context->NullString();
    return context->Nil();
}

/* The values of the entries named key of table t of request q, in order.   */
RexxRoutine3(RexxArrayObject, oorx_entries, wholenumber_t, q, CSTRING, t, CSTRING, key)
{
    RexxArrayObject a = context->NewArray(0);
    int i = table_index(t[0]);
    if (i < 0 || t[1] || q < 0 || (size_t)q >= g_snap.req.size()) return a;
    const table &tb = g_snap.req[q].t[i];
    for (table::const_iterator e = tb.begin(); e != tb.end(); ++e)
        if (same_name(e->first, key))
            context->ArrayAppend(a, context->NewString(e->second.data(), e->second.size()));
    return a;
}

/* Every entry of table t of request q, in order: name, value, name...      */
RexxRoutine2(RexxArrayObject, oorx_table, wholenumber_t, q, CSTRING, t)
{
    RexxArrayObject a = context->NewArray(0);
    int i = table_index(t[0]);
    if (i < 0 || t[1] || q < 0 || (size_t)q >= g_snap.req.size()) return a;
    const table &tb = g_snap.req[q].t[i];
    for (table::const_iterator e = tb.begin(); e != tb.end(); ++e) {
        context->ArrayAppend(a, context->NewString(e->first.data(), e->first.size()));
        context->ArrayAppend(a, context->NewString(e->second.data(), e->second.size()));
    }
    return a;
}

/* A core operation (the handler has checked it): the snapshot changes, and */
/* the module applies it.                                                   */
RexxRoutine3(int, oorx_update, CSTRING, op, CSTRING, key, RexxStringObject, value)
{
    api_update(op, key, context->StringData(value), context->StringLength(value));
    return 0;
}

/* A line for the error log at an APLOG_* level (0..15: trace8 is 15).     */
RexxRoutine2(int, oorx_logl, wholenumber_t, level, RexxStringObject, s)
{
    if (level < 0 || level > 15) level = OORX_LOG_ERR;
    send_log((int)level, context->StringData(s), context->StringLength(s));
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Serving one connection                                                   */
/* ------------------------------------------------------------------------ */

static void logf_conn(const char *fmt, const char *a)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, fmt, a);
    if (n > 0) send_log(OORX_LOG_ERR, buf, (size_t)n < sizeof buf ? n : sizeof buf - 1);
}

static void send_result(uint32_t status, const std::string *value = NULL)
{
    std::string p(4, '\0');
    oorx_put_u32((unsigned char *)&p[0], status);
    if (value) p += *value;
    send_frame(OORX_RESULT, value ? OORX_FLAG_VALUE : 0, p.data(), (uint32_t)p.size());
}

/* A table of strings: (u32 length, bytes) after one another.               */
static bool parse_strings(const std::string &t, std::vector<std::string> &out)
{
    for (size_t p = 0; p < t.size(); ) {
        if (p + 4 > t.size()) return false;
        uint32_t n = oorx_get_u32((const unsigned char *)&t[p]); p += 4;
        if (p + n > t.size()) return false;
        out.push_back(t.substr(p, n)); p += n;
    }
    return true;
}


static void serve(oorx_sock_t fd)
{
    g_fd = fd; g_in.clear(); g_in_eof = false; g_broken = false;

    /* ACCEPT: until now the connection was only queued on the socket, and  */
    /* the module, which waits for this frame within OoRexxQueueTimeout,    */
    /* may have given up on it (then BEGIN never comes, and we return).     */
    const unsigned char version = OORX_PROTOCOL_VERSION;
    send_frame(OORX_ACCEPT, 0, &version, 1);
    TRACE("ACCEPT sent, broken %lld", (long long)g_broken);
    if (g_broken) return;

    /* BEGIN */
    int type, flags; uint32_t len;
    if (oorx_recv_header(fd, &type, &flags, &len) < 0 || type != OORX_BEGIN || len < 7) {
        TRACE("no BEGIN (type %lld)", (long long)type);
        return;
    }
    TRACE("BEGIN, %lld bytes", (long long)len);
    std::string begin(len, '\0');
    if (oorx_read_all(fd, &begin[0], len) < 0) return;
    const unsigned char *b = (const unsigned char *)begin.data();
    if (b[0] != OORX_PROTOCOL_VERSION) {
        logf_conn("oorexx-host: unsupported protocol version %s", "");
        send_result(500);
        return;
    }
    int phase = b[1], klass = b[2];
    unsigned timeout = oorx_get_u32(b + 3);
    std::string program = begin.substr(7);
    /* The content phase runs CGI programs; every other phase, native ones */
    /* (arguments in, a verdict out, Address APACHE). A class with a plugin */
    /* is the plugin's, in the phases it accepts.                           */
    const orx_host_plugin *plugin = klass > 0 && klass < MAX_CLASSES ? g_plugin[klass] : NULL;
    bool hook = klass == OORX_CLASS_NATIVE && phase >= OORX_PHASE_AUTHN &&
                phase <= OORX_PHASE_AUTHEN;
    if (!(phase == OORX_PHASE_CONTENT && klass == OORX_CLASS_CGI) && !hook && !plugin) {
        logf_conn("oorexx-host: phase/class not implemented yet for %s", program.c_str());
        send_result(501);
        return;
    }

    /* ARGS (optional), then VARS.                                          */
    std::vector<std::string> args;
    if (oorx_recv_header(fd, &type, &flags, &len) < 0) return;
    if (type == OORX_ARGS) {
        std::string a(len, '\0');
        if (len && oorx_read_all(fd, &a[0], len) < 0) return;
        if (!parse_strings(a, args)) return;
        if (oorx_recv_header(fd, &type, &flags, &len) < 0) return;
    }
    /* VARS: a bare environment, which becomes the process environment,     */
    /* exactly as a CGI's; or, with flag SNAPSHOT, the snapshot of the      */
    /* request (whose E: keys are that environment).                        */
    if (type != OORX_VARS) return;
    std::string vars(len, '\0');
    if (len && oorx_read_all(fd, &vars[0], len) < 0) return;
    env_clear();
    g_snap.req.clear(); g_snap.client.clear();
    bool snapshot = (flags & OORX_FLAG_SNAPSHOT) != 0;
    for (size_t p = 0; p + 4 <= vars.size(); ) {
        uint32_t kl = oorx_get_u32((const unsigned char *)&vars[p]); p += 4;
        if (p + kl + 4 > vars.size()) break;
        std::string k = vars.substr(p, kl); p += kl;
        uint32_t vl = oorx_get_u32((const unsigned char *)&vars[p]); p += 4;
        if (p + vl > vars.size()) break;
        std::string v = vars.substr(p, vl); p += vl;
        if (snapshot) snap_load(k, v);
        else env_set(k, v);
    }

    /* The instance's policy: the plugin's, or the core's (a CGI's or a    */
    /* hook's).                                                             */
    orx_instance_policy pol;
    memset(&pol, 0, sizeof pol);
    pol.quiet_flush = 1;
    pol.output_to_log = hook;
    if (plugin && plugin->begin(phase, program.c_str(), &pol)) {
        logf_conn("oorexx-host: phase not served by its class for %s", program.c_str());
        send_result(500);
        return;
    }

    /* Like mod_cgi: run in the program's directory, unless the policy      */
    /* says otherwise.                                                      */
#ifdef _WIN32
    size_t slash = program.find_last_of("/\\");
#else
    size_t slash = program.rfind('/');
#endif
    change_dir(pol.cwd ? std::string(pol.cwd)
                       : program.substr(0, slash == std::string::npos ? 0 : slash + 1));

    /* A fresh instance for this phase.                                     */
    RexxLibraryPackage lib = { "OORXHOST", &host_package };
    RexxOption opts[4];
    int no = 0;
    opts[no].optionName = REGISTER_LIBRARY; opts[no++].option = (void *)&lib;
    if (pol.exits) { opts[no].optionName = DIRECT_EXITS; opts[no++].option = (void *)pol.exits; }
    if (pol.address) {
        opts[no].optionName = INITIAL_ADDRESS_ENVIRONMENT; opts[no++].option = pol.address;
    }
    opts[no].optionName = NULL;
    g_quiet_flush = pol.quiet_flush != 0;
    RexxInstance *inst; RexxThreadContext *tc;
    if (!RexxCreateInterpreter(&inst, &tc, opts)) {
        logf_conn("oorexx-host: cannot create an interpreter instance%s", "");
        send_result(500);
        if (plugin) plugin->end();
        return;
    }

    uint32_t status = 0;
    std::string value;
    bool has_value = false;
    out_channel och;
    std::string pipe_path;
    if (!out_open(och, pipe_path)) {
        inst->Terminate();
        logf_conn("oorexx-host: cannot create the output pipe%s", "");
        send_result(500);
        if (plugin) plugin->end();
        return;
    }

    RexxRoutineObject boot = tc->NewRoutine("oorexx-host bootstrap",
                                            bootstrap_src, sizeof bootstrap_src - 1);
    RexxObjectPtr out = NULLOBJECT;
    if (boot != NULLOBJECT && !tc->CheckCondition()) {
        RexxArrayObject bargs = tc->ArrayOfTwo(tc->String(pipe_path.c_str()),
                                               pol.output_to_log ? tc->True() : tc->False());
        out = tc->CallRoutine(boot, bargs);
    }
    /* A native program talks to Apache through Address APACHE: the        */
    /* environment's package is the anchor's, already loaded.              */
    if (klass == OORX_CLASS_NATIVE && out != NULLOBJECT && !tc->CheckCondition()) {
        RexxRoutineObject env = tc->NewRoutine("oorexx-host APACHE registration",
                                               apache_register_src, sizeof apache_register_src - 1);
        if (env == NULLOBJECT || tc->CheckCondition() ||
            (tc->CallRoutine(env, tc->ArrayOfTwo(g_apache_class, tc->WholeNumber(phase))),
             tc->CheckCondition())) {
            RexxCondition c;
            RexxDirectoryObject ci = tc->GetConditionInfo();
            std::string m = "oorexx-host: the APACHE environment failed";
            if (ci != NULLOBJECT) {
                tc->DecodeConditionInfo(ci, &c);
                char n[64];
                snprintf(n, sizeof n, ": line %d: Error %d", (int)c.position, (int)c.rc);
                m += n;
                if (c.message != NULLOBJECT) { m += ": "; m += tc->CString(c.message); }
            }
            send_log(OORX_LOG_ERR, m.data(), m.size());
            /* Close the program's output, or the pump never sees its end.  */
            tc->ClearCondition();
            tc->SendMessage0(out, "CLOSE");
            tc->ClearCondition();
            out = NULLOBJECT;
        }
    }
    if (out == NULLOBJECT || tc->CheckCondition()) {
        tc->ClearCondition();
        logf_conn("oorexx-host: bootstrap failed%s", "");
        status = 500;
    } else {
        RexxArrayObject pargs = NULLOBJECT;
        /* A plugin's program gets the arguments the plugin gives.          */
        if (plugin) {
            args.clear();
            for (size_t i = 0; i < pol.argc; i++)
                args.push_back(std::string(pol.argv[i], pol.argl[i]));
        }
        if (!args.empty()) {
            pargs = tc->NewArray(args.size());
            for (size_t i = 0; i < args.size(); i++)
                tc->ArrayPut(pargs, tc->NewString(args[i].data(), args[i].size()), i + 1);
        }
        wd_arm(inst, timeout);
        if (plugin) plugin->running(1);
        RexxObjectPtr ret = tc->CallProgram(program.c_str(), pargs);
        if (plugin) plugin->running(0);
        bool halted = wd_disarm();
        if (!tc->CheckCondition() && ret != NULLOBJECT) {
            RexxStringObject rs = tc->ObjectToString(ret);
            if (!tc->CheckCondition() && rs != NULLOBJECT) {
                value.assign(tc->StringData(rs), tc->StringLength(rs));
                has_value = true;
            }
        }
        if (tc->CheckCondition()) {
            /* An uncaught condition: what a CGI would print on stderr.     */
            RexxCondition c;
            tc->DecodeConditionInfo(tc->GetConditionInfo(), &c);
            std::string m = "oorexx-host: ";
            m += program;
            char line[64];
            snprintf(line, sizeof line, ": line %d: Error %d", (int)c.position, (int)(c.rc));
            m += line;
            if (c.errortext != NULLOBJECT) { m += ": "; m += tc->CString(c.errortext); }
            if (c.message != NULLOBJECT)   { m += ": "; m += tc->CString(c.message); }
            if (halted) m += " (phase timeout)";
            tc->ClearCondition();
            send_log(OORX_LOG_ERR, m.data(), m.size());
            /* A CGI's failure shows in its output; a hook's is its verdict; */
            /* a plugin's program's, a 500 (mod_rexx's did the same).        */
            if (hook || plugin) { status = 500; has_value = false; }
        }
        /* Close the program's output: flushes what the stream still holds. */
        tc->SendMessage0(out, "CLOSE");
        tc->ClearCondition();
    }
    inst->Terminate();
    if (plugin) plugin->end();
    out_close(och);
    TRACE("phase done, status %lld", (long long)status);
    send_result(status, has_value ? &value : NULL);
}

/* ------------------------------------------------------------------------ */
/* Pool                                                                     */
/*                                                                          */
/* The master forks children and keeps between --processes and             */
/* --max-processes of them alive. It never touches ooRexx. Every child has */
/* a slot in a shared page with its state; the master reads the states to   */
/* decide when to grow (no child idle) or shrink (idle ones to spare for a  */
/* while). A child moves IDLE -> BUSY itself before accepting; the master   */
/* moves IDLE -> RETIRE to ask an idle child to leave. Both moves are       */
/* compare-and-swap, so a child never accepts after being retired.          */
/*                                                                          */
/* Draining (SIGTERM): the master closes the write end of a pipe that every */
/* child polls. A child that sees it accepts what is still queued on the    */
/* socket, serves it, and exits. When the module manages the host, it has   */
/* unlinked the socket's path before signalling, so nothing new arrives on  */
/* this socket; the next generation of hosts listens on a new one. After   */
/* --grace seconds the master SIGKILLs what is left.                        */
/* ------------------------------------------------------------------------ */

enum { SLOT_EMPTY = 0, SLOT_STARTING, SLOT_IDLE, SLOT_BUSY, SLOT_RETIRE };

#ifdef _WIN32
/* In a file mapping the children inherit; Interlocked* are full barriers. */
struct slot { volatile LONG state; DWORD pid; };
#else
struct slot { int state; pid_t pid; };
#endif

static struct slot *g_slots;        /* shared page, one per child           */

#ifdef _WIN32
static bool slot_cas(int i, int from, int to)
{
    return InterlockedCompareExchange(&g_slots[i].state, to, from) == from;
}

static void slot_set(int i, int v) { InterlockedExchange(&g_slots[i].state, v); }
static int  slot_get(int i)        { return (int)InterlockedCompareExchange(&g_slots[i].state, 0, 0); }
#else
static bool slot_cas(int i, int from, int to)
{
    return __atomic_compare_exchange_n(&g_slots[i].state, &from, to, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void slot_set(int i, int v) { __atomic_store_n(&g_slots[i].state, v, __ATOMIC_SEQ_CST); }
static int  slot_get(int i)        { return __atomic_load_n(&g_slots[i].state, __ATOMIC_SEQ_CST); }
#endif

/* How often an idle child looks at its slot, and how long the pool must    */
/* have idle children to spare before the master retires one.              */
#define CHILD_POLL_MS    500
#define MASTER_TICK_MS   200
#define KEEPER_TICK_MS   1000
#define SHRINK_AFTER_MS  10000

/* Exit status 2 means "the configuration is wrong": mod_orx does not       */
/* restart a host that exits with it, since it would only fail again.       */
#define EXIT_CONFIG 2

static std::vector<std::string> g_preload;   /* --preload FILE, in order    */

/* --plugin CLASS:PATH: load the plugin that serves a class of program, in */
/* the master, before any child and any interpreter. Anything wrong is a   */
/* configuration error.                                                    */
static bool load_plugin(const char *spec)
{
    char *end;
    long klass = strtol(spec, &end, 10);
    if (*end != ':' || klass <= 0 || klass >= MAX_CLASSES || klass == OORX_CLASS_CGI ||
        klass == OORX_CLASS_NATIVE || g_plugin[klass]) {
        fprintf(stderr, "oorexx-host: --plugin %s: not a class a plugin may serve\n", spec);
        return false;
    }
    const char *path = end + 1;
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path);
    if (!lib) {
        fprintf(stderr, "oorexx-host: cannot load the plugin %s: error %lu\n", path,
                GetLastError());
        return false;
    }
    orx_plugin_init_fn *init = (orx_plugin_init_fn *)(void *)GetProcAddress(lib, ORX_PLUGIN_INIT);
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "oorexx-host: cannot load the plugin %s: %s\n", path, dlerror());
        return false;
    }
    orx_plugin_init_fn *init = (orx_plugin_init_fn *)dlsym(lib, ORX_PLUGIN_INIT);
#endif
    const orx_host_plugin *pl = init ? init(&g_api) : NULL;
    if (!pl || pl->abi != ORX_PLUGIN_ABI || pl->klass != klass) {
        fprintf(stderr, "oorexx-host: %s is not a plugin for class %ld of this host "
                "(interface %d)\n", path, klass, ORX_PLUGIN_ABI);
        return false;
    }
    g_plugin[klass] = pl;
    return true;
}

/* The anchor: an instance that is never terminated, with the APACHE        */
/* environment, and the preloaded packages loaded in it by a program made   */
/* of one ::Requires per file. It                                           */
/* lives in the child's main thread, like every instance of the child, and  */
/* the watchdog never sees it. A package that fails to load is a            */
/* configuration error: the child exits with EXIT_CONFIG, and the master    */
/* with it.                                                                 */
static void load_anchor(void)
{
    RexxInstance *inst; RexxThreadContext *tc;
    RexxLibraryPackage lib = { "OORXHOST", &host_package };
    RexxOption opts[2];
    opts[0].optionName = REGISTER_LIBRARY; opts[0].option = (void *)&lib;
    opts[1].optionName = NULL;
    if (!RexxCreateInterpreter(&inst, &tc, opts)) {
        err_write("oorexx-host: cannot create the anchor instance\n");
        quit(EXIT_CONFIG);
    }
    /* The APACHE environment of native programs (its class is what this    */
    /* returns); and the preloaded packages, required.                      */
    std::string src = "use strict arg apache\n"
                      "p = .Package~new('oorexx-host: the APACHE environment',"
                      " apache~makeArray('0a'x))\n"
                      "return p~findClass('OoRexxApache')\n";
    for (size_t i = 0; i < g_preload.size(); i++) {
        std::string q;
        for (char ch : g_preload[i]) { q += ch; if (ch == '"') q += '"'; }
        src += "::requires \"" + q + "\"\n";
    }
    RexxRoutineObject r = tc->NewRoutine("oorexx-host preload", src.data(), src.size());
    /* Held for good: the routine keeps the packages it required loaded.  */
    if (r != NULLOBJECT) tc->RequestGlobalReference(r);
    if (r != NULLOBJECT && !tc->CheckCondition()) {
        g_apache_class = tc->CallRoutine(r, tc->ArrayOfOne(tc->NewString(apache_src,
                                                                          sizeof apache_src - 1)));
        if (g_apache_class != NULLOBJECT && !tc->CheckCondition())
            tc->RequestGlobalReference(g_apache_class);
    }
    if (r == NULLOBJECT || g_apache_class == NULLOBJECT || tc->CheckCondition()) {
        RexxCondition c;
        std::string m = g_preload.empty() ? "oorexx-host: the anchor instance failed"
                                          : "oorexx-host: OoRexxPreload failed";
        RexxDirectoryObject ci = tc->GetConditionInfo();
        if (ci != NULLOBJECT) {
            tc->DecodeConditionInfo(ci, &c);
            char n[64];
            snprintf(n, sizeof n, ": Error %d", (int)c.rc);
            m += n;
            if (c.program != NULLOBJECT) {
                snprintf(n, sizeof n, " at line %d", (int)c.position);
                m += " in "; m += tc->CString(c.program); m += n;
            }
            if (c.errortext != NULLOBJECT) { m += ": "; m += tc->CString(c.errortext); }
            if (c.message != NULLOBJECT)   { m += ": "; m += tc->CString(c.message); }
        }
        m += "\n";
        err_write(m);
        quit(EXIT_CONFIG);
    }
}

#ifndef _WIN32
static void child_main(int me, pid_t master, int lsock, int stopfd, int max_requests)
{
    /* Never outlive the master. Everywhere, its death closes the stop     */
    /* pipe, and an idle child drains and exits; on Linux, besides, a busy  */
    /* one is killed at once.                                               */
#ifdef ORX_PDEATHSIG
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    if (getppid() != master) _exit(0);
    signal(SIGTERM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    std::thread(watchdog).detach();

    /* The anchor: the first instance of the process (the expensive one),   */
    /* which stays, with the APACHE environment and the preloaded packages. */
    load_anchor();
    if (!slot_cas(me, SLOT_STARTING, SLOT_IDLE)) _exit(0);

    bool draining = false;
    for (int served = 0; max_requests <= 0 || served < max_requests; ) {
        struct pollfd pf[2] = { { lsock, POLLIN, 0 }, { stopfd, POLLIN, 0 } };
        int n = poll(pf, 2, draining ? 0 : CHILD_POLL_MS);
        if (n < 0) { if (errno == EINTR) continue; _exit(1); }
        if (pf[1].revents) draining = true;
        if (n == 0 && !draining) {
            if (slot_get(me) == SLOT_RETIRE) _exit(0);
            continue;
        }
        if (!slot_cas(me, SLOT_IDLE, SLOT_BUSY)) _exit(0);   /* retired */
        int fd = accept(lsock, NULL, NULL);
        if (fd < 0) {
            slot_set(me, SLOT_IDLE);
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
                errno == ECONNABORTED) {
                if (draining) _exit(0);   /* the queue is empty: done      */
                continue;                 /* another child took it          */
            }
            _exit(1);
        }
        /* The listening socket is non-blocking (so that idle children can  */
        /* race for a connection); the connection itself must block. Linux  */
        /* does not pass O_NONBLOCK on to accepted sockets, macOS does.     */
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        serve(fd);
        close(fd);
        served++;
        if (!slot_cas(me, SLOT_BUSY, SLOT_IDLE)) _exit(0);
    }
    _exit(0);
}

/* Signals reach the master's loop through a self-pipe.                     */
static int g_wake[2] = { -1, -1 };
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig)
{
    int e = errno;
    if (sig == SIGTERM || sig == SIGINT) g_stop = 1;
    if (write(g_wake[1], "x", 1) < 0) { /* the pipe is full: fine */ }
    errno = e;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s (--socket PATH | --listen-fd N) [--processes MIN]\n"
        "       [--max-processes MAX] [--max-requests N] [--grace SECONDS]\n"
        "       [--preload FILE]... [--plugin CLASS:PATH]...\n", argv0);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int listen_fd = -1;
    int min_procs = 4, max_procs = -1, max_requests = 1000, grace = 70;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc)               path = argv[++i];
        else if (!strcmp(argv[i], "--listen-fd") && i + 1 < argc)       listen_fd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--processes") && i + 1 < argc)       min_procs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-processes") && i + 1 < argc)   max_procs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-requests") && i + 1 < argc)    max_requests = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--grace") && i + 1 < argc)           grace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preload") && i + 1 < argc)         g_preload.push_back(argv[++i]);
        else if (!strcmp(argv[i], "--plugin") && i + 1 < argc) {
            if (!load_plugin(argv[++i])) return EXIT_CONFIG;
        }
        else { usage(argv[0]); return EXIT_CONFIG; }
    }
    if (max_procs < 0) max_procs = min_procs;
    if ((path == NULL) == (listen_fd < 0) || min_procs < 1 || max_procs < min_procs
        || grace < 0) {
        usage(argv[0]);
        return EXIT_CONFIG;
    }

    int lsock = listen_fd;
    pid_t keeper = 0;       /* managed: the master's parent */
    if (path) {
        /* Standalone: bind the socket ourselves (only the user the host    */
        /* runs as, Apache's, may connect).                                 */
        lsock = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof sa.sun_path) {
            fprintf(stderr, "oorexx-host: socket path too long\n"); return EXIT_CONFIG;
        }
        strcpy(sa.sun_path, path);
        unlink(path);
        if (bind(lsock, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lsock, 128) < 0) {
            perror("oorexx-host: bind/listen"); return EXIT_CONFIG;
        }
        chmod(path, 0600);
    } else {
        /* Managed by mod_orx: the module created, bound and secured the    */
        /* socket (as root, in Apache's run-time directory) and owns its    */
        /* path; we only accept on it.                                      */
        int type = 0; socklen_t tl = sizeof type;
        if (getsockopt(lsock, SOL_SOCKET, SO_TYPE, &type, &tl) < 0 || type != SOCK_STREAM) {
            fprintf(stderr, "oorexx-host: --listen-fd %d is not a stream socket\n", lsock);
            return EXIT_CONFIG;
        }
    }
    fcntl(lsock, F_SETFL, fcntl(lsock, F_GETFL) | O_NONBLOCK);

    if (listen_fd >= 0) {
        /* Managed: split into a KEEPER, Apache's only child, and the      */
        /* MASTER of the pool, the keeper's child. On SIGTERM the keeper   */
        /* passes it on and exits at once, so Apache reaps it right away;  */
        /* the master, orphaned, drains on its own. Apache unloads and     */
        /* reloads its modules on every restart, so nothing of a draining */
        /* host may need the module afterwards: not even being reaped.    */
        /* If the master dies on its own, the keeper exits with its status */
        /* and the module starts a new host.                               */
        /* The keeper's signals are blocked from before the fork, so that  */
        /* none can be lost: on Linux it takes them synchronously           */
        /* (sigwaitinfo), and the module has asked for a SIGTERM if Apache's */
        /* parent dies; elsewhere they reach a loop through a self-pipe, and */
        /* the loop also sees, every second, whether Apache's parent is     */
        /* still its parent.                                                */
        sigset_t ks, old;
        sigemptyset(&ks);
        sigaddset(&ks, SIGTERM); sigaddset(&ks, SIGINT); sigaddset(&ks, SIGCHLD);
        sigprocmask(SIG_BLOCK, &ks, &old);
        pid_t self = getpid();
#ifndef ORX_PDEATHSIG
        pid_t apache = getppid();
#endif
        pid_t m = fork();
        if (m < 0) { perror("oorexx-host: fork"); return 1; }
        if (m > 0) {
            signal(SIGHUP,  SIG_IGN);
            signal(SIGUSR1, SIG_IGN);
            signal(SIGUSR2, SIG_IGN);
            close(lsock);
            int st;
#ifdef ORX_PDEATHSIG
            for (;;) {
                if (waitpid(m, &st, WNOHANG) == m) break;
                int sig = sigwaitinfo(&ks, NULL);
                if (sig == SIGTERM || sig == SIGINT) { kill(m, SIGTERM); _exit(0); }
            }
#else
            if (pipe(g_wake) < 0) { kill(m, SIGTERM); _exit(1); }
            fcntl(g_wake[0], F_SETFL, O_NONBLOCK);
            fcntl(g_wake[1], F_SETFL, O_NONBLOCK);
            struct sigaction ka;
            memset(&ka, 0, sizeof ka);
            ka.sa_handler = on_signal;
            sigaction(SIGTERM, &ka, NULL);
            sigaction(SIGINT,  &ka, NULL);
            sigaction(SIGCHLD, &ka, NULL);
            sigprocmask(SIG_SETMASK, &old, NULL);
            for (;;) {
                if (waitpid(m, &st, WNOHANG) == m) break;
                if (g_stop || getppid() != apache) { kill(m, SIGTERM); _exit(0); }
                struct pollfd pf = { g_wake[0], POLLIN, 0 };
                poll(&pf, 1, KEEPER_TICK_MS);
                char junk[64];
                while (read(g_wake[0], junk, sizeof junk) > 0) ;
            }
#endif
            /* Never 15 or 7 (Apache's APEXIT_CHILDFATAL and CHILDSICK).    */
            _exit(WIFEXITED(st) && WEXITSTATUS(st) == EXIT_CONFIG ? EXIT_CONFIG : 1);
        }
        sigprocmask(SIG_SETMASK, &old, NULL);
#ifdef ORX_PDEATHSIG
        prctl(PR_SET_PDEATHSIG, SIGTERM);       /* the keeper died: drain */
#endif
        keeper = self;
        if (getppid() != keeper) return 0;      /* it died already */
    }

    void *page = mmap(NULL, sizeof(struct slot) * max_procs, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { perror("oorexx-host: mmap"); return 1; }
    g_slots = (struct slot *)page;
    memset(g_slots, 0, sizeof(struct slot) * max_procs);

    int stop[2];
    if (pipe(stop) < 0 || pipe(g_wake) < 0) { perror("oorexx-host: pipe"); return 1; }
    fcntl(g_wake[0], F_SETFL, O_NONBLOCK);
    fcntl(g_wake[1], F_SETFL, O_NONBLOCK);

    /* No SA_RESTART: a signal must interrupt poll() so the loop sees it.   */
    struct sigaction sa_sig;
    memset(&sa_sig, 0, sizeof sa_sig);
    sa_sig.sa_handler = on_signal;
    sigaction(SIGTERM, &sa_sig, NULL);
    sigaction(SIGINT,  &sa_sig, NULL);
    sigaction(SIGCHLD, &sa_sig, NULL);
    /* When prefork Apache stops, it signals its whole process group; a   */
    /* managed host has its own group, and ignores these anyway.            */
    signal(SIGHUP,  SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    pid_t master = getpid();
    long idle_since = -1;
    bool config_error = false;
    while (!g_stop && !config_error) {
#ifndef ORX_PDEATHSIG
        /* The keeper died (Linux sends a SIGTERM): drain.                  */
        if (keeper > 0 && getppid() != keeper) break;
#endif
        int st; pid_t dead;
        while ((dead = waitpid(-1, &st, WNOHANG)) > 0) {
            for (int i = 0; i < max_procs; i++)
                if (g_slots[i].pid == dead) { g_slots[i].pid = 0; slot_set(i, SLOT_EMPTY); }
            /* A child that cannot start (its preload failed) would fail    */
            /* again in every replacement: stop, and say why on the way out. */
            if (WIFEXITED(st) && WEXITSTATUS(st) == EXIT_CONFIG) config_error = true;
        }
        if (config_error) break;

        int total = 0, ready = 0, idle = 0;
        for (int i = 0; i < max_procs; i++) {
            int s = slot_get(i);
            if (s == SLOT_EMPTY) continue;
            total++;
            if (s == SLOT_IDLE) idle++;
            if (s == SLOT_IDLE || s == SLOT_STARTING) ready++;
        }

        int want = 0;
        if (total < min_procs) want = min_procs - total;
        else if (ready == 0 && total < max_procs) want = 1;
        for (int i = 0; i < max_procs && want > 0; i++) {
            if (slot_get(i) != SLOT_EMPTY) continue;
            slot_set(i, SLOT_STARTING);
            pid_t p = fork();
            if (p == 0) {
                close(stop[1]); close(g_wake[0]); close(g_wake[1]);
                child_main(i, master, lsock, stop[0], max_requests);
            }
            if (p < 0) { slot_set(i, SLOT_EMPTY); break; }
            g_slots[i].pid = p;
            want--;
        }

        /* Shrink: more than one child idle, and above the minimum, for     */
        /* SHRINK_AFTER_MS on end: retire one idle child, then wait again.  */
        if (idle > 1 && total > min_procs) {
            if (idle_since < 0) idle_since = now_ms();
            else if (now_ms() - idle_since >= SHRINK_AFTER_MS) {
                for (int i = 0; i < max_procs; i++)
                    if (slot_cas(i, SLOT_IDLE, SLOT_RETIRE)) break;
                idle_since = -1;
            }
        } else idle_since = -1;

        struct pollfd pf = { g_wake[0], POLLIN, 0 };
        poll(&pf, 1, MASTER_TICK_MS);
        char junk[64];
        while (read(g_wake[0], junk, sizeof junk) > 0) ;
    }

    /* Drain: tell the children, give them --grace seconds, then kill.      */
    close(stop[1]);
    long deadline = now_ms() + grace * 1000L;
    for (;;) {
        int left = 0;
        int st; pid_t dead;
        while ((dead = waitpid(-1, &st, WNOHANG)) > 0)
            for (int i = 0; i < max_procs; i++)
                if (g_slots[i].pid == dead) { g_slots[i].pid = 0; slot_set(i, SLOT_EMPTY); }
        for (int i = 0; i < max_procs; i++) if (g_slots[i].pid > 0) left++;
        if (left == 0 || now_ms() >= deadline) break;
        struct pollfd pf = { g_wake[0], POLLIN, 0 };
        poll(&pf, 1, MASTER_TICK_MS);
        char junk[64];
        while (read(g_wake[0], junk, sizeof junk) > 0) ;
    }
    /* SIGKILL, not SIGTERM: ooRexx installs its own handlers in every
     * process that creates an instance, and turns SIGTERM into a HALT.     */
    for (int i = 0; i < max_procs; i++) if (g_slots[i].pid > 0) kill(g_slots[i].pid, SIGKILL);
    while (wait(NULL) > 0) ;
    if (path) unlink(path);
    return config_error ? EXIT_CONFIG : 0;
}
#endif /* !_WIN32 */

#ifdef _WIN32
/* ------------------------------------------------------------------------ */
/* The pool on Windows                                                      */
/*                                                                          */
/* The same pool, with what Windows has instead of fork, signals and a      */
/* shared anonymous page:                                                   */
/*   - the master starts each child with CreateProcess, running this same   */
/*     executable with --child; the child inherits exactly the handles it   */
/*     needs (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): the listening socket, the */
/*     slots (a file mapping), the stop event, a handle on the master, and  */
/*     both ends of its output pipe, the write end as its standard output;  */
/*   - draining is an event the master sets (the stop pipe on Unix); a      */
/*     child that sees it, or the master gone, drains and exits; children   */
/*     wait in a blocking accept() (see child_main);                        */
/*   - the master is told to drain by an event too: mod_orx's (--stop-event) */
/*     or, standalone, its own, set by Ctrl+C. There is no keeper: Apache   */
/*     never reaps the host on Windows, and mod_orx puts it in a job that   */
/*     ends with Apache.                                                    */
/* ------------------------------------------------------------------------ */

static HANDLE g_stop_ev;            /* the master: drain                    */

static BOOL WINAPI on_ctrl(DWORD)
{
    SetEvent(g_stop_ev);
    return TRUE;
}

/* long is 32 bits on Windows: milliseconds since boot need more.          */
static long long now_ms(void) { return (long long)GetTickCount64(); }

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s (--socket PATH | --listen-fd SOCKET --stop-event HANDLE)\n"
        "       [--processes MIN] [--max-processes MAX] [--max-requests N]\n"
        "       [--grace SECONDS] [--preload FILE]... [--plugin CLASS:PATH]...\n", argv0);
}

static HANDLE handle_arg(const char *s)
{
    return (HANDLE)(uintptr_t)_strtoui64(s, NULL, 10);
}

/* One argument for a command line, quoted as the C runtime parses it.      */
static void append_arg(std::string &cmd, const std::string &a)
{
    cmd += ' ';
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) { cmd += a; return; }
    cmd += '"';
    size_t bs = 0;
    for (char ch : a) {
        if (ch == '\\') { bs++; continue; }
        if (ch == '"') cmd.append(bs * 2 + 1, '\\');
        else cmd.append(bs, '\\');
        bs = 0;
        cmd += ch;
    }
    cmd.append(bs * 2, '\\');
    cmd += '"';
}

/* A child waits for a connection in a blocking accept(), and only while it */
/* is idle; and only one child at a time, holding the pool's accept mutex,  */
/* as Apache's own MPMs do. Windows does not hand a connection reliably to  */
/* one of several processes waiting on one inherited AF_UNIX socket: in    */
/* WSAPoll, often to none of them; in accept(), often to one that does not */
/* return until the client gives up (both traced: 10 s queue timeouts with */
/* idle children).                                                         */
/*                                                                          */
/* A watcher thread does what the poll timeout does on Unix: it sees the    */
/* stop event, the master's death and a retirement. Then a child waiting   */
/* for the mutex leaves (the leave event), and one in accept() has it      */
/* cancelled (CancelSynchronousIo: closing the socket's handle does not end */
/* a blocking accept on AF_UNIX, measured), again every 100 ms until the   */
/* child is gone, in case the cancel came just before the accept began; a  */
/* connection taken just before is still served. The cancel and the end of */
/* the accept go under one lock (g_wlock), so that a cancel meant for the  */
/* accept never reaches the I/O of a connection the child has just taken   */
/* (serve's send and recv are synchronous I/O too). Draining, the child stops */
/* once it has waited 200 ms with nothing to take: the module has removed  */
/* the socket's path by then, so nothing new can arrive.                   */
#define DRAIN_IDLE_MS 200
enum { W_BUSY = 0, W_MUTEX, W_ACCEPT };
static volatile LONG      g_wstate = W_BUSY;
static volatile long long g_wait_since;
static volatile LONG      g_closing;        /* the watcher said: leave       */
static HANDLE             g_leave;          /* set: stop waiting, leave      */
static SRWLOCK            g_wlock = SRWLOCK_INIT; /* W_ACCEPT -> W_BUSY, cancel */

/* Cancel the main thread's accept, if it is still in one.                  */
static void cancel_accept(HANDLE main_thread)
{
    AcquireSRWLockExclusive(&g_wlock);
    if (InterlockedCompareExchange(&g_wstate, 0, 0) == W_ACCEPT)
        CancelSynchronousIo(main_thread);
    ReleaseSRWLockExclusive(&g_wlock);
}

static void accept_watch(int me, HANDLE main_thread, HANDLE stop, HANDLE master)
{
    HANDLE hs[2] = { stop, master };
    bool draining = false;
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, 100);
        if (!draining && (w == WAIT_OBJECT_0 || w == WAIT_OBJECT_0 + 1)) {
            draining = true;
            TRACE("draining");
        }
        if (draining) Sleep(20);               /* the stop events stay set */
        LONG st = InterlockedCompareExchange(&g_wstate, 0, 0);
        if (InterlockedCompareExchange(&g_closing, 0, 0)) {
            cancel_accept(main_thread);
            Sleep(80);
            continue;
        }
        bool retired = slot_get(me) == SLOT_RETIRE;
        if (st != W_BUSY && (retired || (draining &&
            (long long)GetTickCount64() - g_wait_since >= DRAIN_IDLE_MS))) {
            TRACE(retired ? "retired" : "drained");
            InterlockedExchange(&g_closing, 1);
            SetEvent(g_leave);
            cancel_accept(main_thread);
        }
    }
}

static void child_main(int me, SOCKET lsock, HANDLE stop, HANDLE master, HANDLE mutex,
                       int max_requests)
{
    std::thread(watchdog).detach();

    /* The anchor: the first instance of the process (the expensive one),   */
    /* which stays, with the APACHE environment and the preloaded packages. */
    load_anchor();
    TRACE("slot %lld: anchor loaded", me);
    if (!slot_cas(me, SLOT_STARTING, SLOT_IDLE)) { TRACE("slot %lld: not STARTING", me); quit(0); }
    g_leave = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_leave) die("no leave event");
    HANDLE self_thread;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &self_thread, 0, FALSE, DUPLICATE_SAME_ACCESS))
        die("no handle on the main thread");
    std::thread(accept_watch, me, self_thread, stop, master).detach();

    for (int served = 0; max_requests <= 0 || served < max_requests; ) {
        g_wait_since = (long long)GetTickCount64();
        InterlockedExchange(&g_wstate, W_MUTEX);
        HANDLE hs[2] = { mutex, g_leave };
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED_0) quit(0);   /* leave */
        /* The watcher may have said to leave meanwhile.                    */
        if (InterlockedCompareExchange(&g_closing, 0, 0)) { ReleaseMutex(mutex); quit(0); }
        InterlockedExchange(&g_wstate, W_ACCEPT);
        SOCKET fd = accept(lsock, NULL, NULL);
        int e = fd == INVALID_SOCKET ? WSAGetLastError() : 0;
        AcquireSRWLockExclusive(&g_wlock);         /* no cancel in flight now */
        InterlockedExchange(&g_wstate, W_BUSY);
        ReleaseSRWLockExclusive(&g_wlock);
        ReleaseMutex(mutex);
        if (fd == INVALID_SOCKET) {
            TRACE("accept: error %lld", e);
            if (InterlockedCompareExchange(&g_closing, 0, 0)) quit(0);   /* done */
            if (e == WSAEINTR || e == WSAECONNABORTED || e == WSAECONNRESET) continue;
            SetLastError((DWORD)e);
            die("accept failed");
        }
        TRACE("accepted, served %lld", served);
        /* Retired while it waited: it serves this one, and leaves.          */
        bool leave = !slot_cas(me, SLOT_IDLE, SLOT_BUSY);
        /* No command a program runs may inherit the connection.            */
        u_long blocking = 0;
        ioctlsocket(fd, FIONBIO, &blocking);
        SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
        serve(fd);
        closesocket(fd);
        served++;
        if (leave || !slot_cas(me, SLOT_BUSY, SLOT_IDLE)) quit(0);
    }
    TRACE("max requests: leaving");
    quit(0);
}

/* What the master hands every child: the handles, and the options that    */
/* matter to a child.                                                       */
struct pool {
    SOCKET lsock;
    HANDLE map, stop, self, mutex;
    std::vector<std::string> child_opts;
    std::string exe;
    unsigned long pipes;            /* to name each output pipe uniquely    */
};

/* Start the child of slot i. Returns its process handle, or NULL.         */
static HANDLE spawn_child(pool &p, int i, DWORD *pid)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    char name[96];
    snprintf(name, sizeof name, "\\\\.\\pipe\\oorexx-host-%lu-%d-%lu",
             GetCurrentProcessId(), i, p.pipes++);
    HANDLE r = CreateNamedPipeA(name,
                   PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                   1, 0, 65536, 0, &sa);
    if (r == INVALID_HANDLE_VALUE) return NULL;
    HANDLE w = CreateFileA(name, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
    if (w == INVALID_HANDLE_VALUE) { CloseHandle(r); return NULL; }
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, NULL);
    /* Standard error: the master's (Apache's error log, when managed).     */
    HANDLE err = NULL, e0 = GetStdHandle(STD_ERROR_HANDLE);
    if (e0 && e0 != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), e0, GetCurrentProcess(), &err, 0, TRUE,
                        DUPLICATE_SAME_ACCESS);

    std::vector<HANDLE> inherit;
    inherit.push_back((HANDLE)p.lsock);
    inherit.push_back(p.map); inherit.push_back(p.stop); inherit.push_back(p.self);
    inherit.push_back(p.mutex);
    inherit.push_back(r); inherit.push_back(w);
    if (nul != INVALID_HANDLE_VALUE) inherit.push_back(nul);
    if (err) inherit.push_back(err);

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    std::vector<char> attrs(size);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)&attrs[0];
    HANDLE proc = NULL;
    if (InitializeProcThreadAttributeList(al, 1, 0, &size) &&
        UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherit[0],
                                  inherit.size() * sizeof(HANDLE), NULL, NULL)) {
        char spec[200];
        snprintf(spec, sizeof spec, "%d:%llu:%llu:%llu:%llu:%llu:%llu", i,
                 (unsigned long long)p.lsock, (unsigned long long)(uintptr_t)p.map,
                 (unsigned long long)(uintptr_t)p.stop, (unsigned long long)(uintptr_t)p.self,
                 (unsigned long long)(uintptr_t)r, (unsigned long long)(uintptr_t)p.mutex);
        std::string cmd;
        append_arg(cmd, p.exe);
        cmd.erase(0, 1);
        append_arg(cmd, "--child");
        append_arg(cmd, spec);
        for (size_t k = 0; k < p.child_opts.size(); k++) append_arg(cmd, p.child_opts[k]);

        STARTUPINFOEXA si;
        memset(&si, 0, sizeof si);
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput  = nul != INVALID_HANDLE_VALUE ? nul : NULL;
        si.StartupInfo.hStdOutput = w;
        si.StartupInfo.hStdError  = err;
        si.lpAttributeList = al;
        PROCESS_INFORMATION pi;
        std::vector<char> line(cmd.begin(), cmd.end());
        line.push_back('\0');
        if (CreateProcessA(p.exe.c_str(), &line[0], NULL, NULL, TRUE,
                           EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL,
                           &si.StartupInfo, &pi)) {
            CloseHandle(pi.hThread);
            proc = pi.hProcess;
            *pid = pi.dwProcessId;
        }
        DeleteProcThreadAttributeList(al);
    }
    CloseHandle(r); CloseHandle(w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (err) CloseHandle(err);
    return proc;
}

/* A child: --child SLOT:SOCKET:MAP:STOP:MASTER:PIPE:MUTEX (handle values). */
static int run_child(const char *spec, int max_requests)
{
    unsigned long long v[7];
    int me;
    if (sscanf(spec, "%d:%llu:%llu:%llu:%llu:%llu:%llu", &me, &v[1], &v[2], &v[3], &v[4], &v[5],
               &v[6]) != 7)
        return EXIT_CONFIG;
    SOCKET lsock  = (SOCKET)v[1];
    HANDLE map    = (HANDLE)(uintptr_t)v[2];
    HANDLE stop   = (HANDLE)(uintptr_t)v[3];
    HANDLE master = (HANDLE)(uintptr_t)v[4];
    g_out_r       = (HANDLE)(uintptr_t)v[5];
    HANDLE mutex  = (HANDLE)(uintptr_t)v[6];
    /* None of these is for the commands a program may run.                */
    SetHandleInformation((HANDLE)lsock, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(map, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stop, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(master, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(g_out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(mutex, HANDLE_FLAG_INHERIT, 0);
    g_slots = (struct slot *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!g_slots) return 1;
    memset(&g_ov, 0, sizeof g_ov);
    g_ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_phase_start = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_phase_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_phase_flushed = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_ov.hEvent || !g_phase_start || !g_phase_done || !g_phase_flushed) return 1;
    std::thread(pump_thread).detach();
    child_main(me, lsock, stop, master, mutex, max_requests);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *child = NULL, *stop_arg = NULL;
    SOCKET listen_fd = INVALID_SOCKET;
    int min_procs = 4, max_procs = -1, max_requests = 1000, grace = 70;
    std::vector<std::string> child_opts;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc)               path = argv[++i];
        else if (!strcmp(argv[i], "--listen-fd") && i + 1 < argc)
            listen_fd = (SOCKET)_strtoui64(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--stop-event") && i + 1 < argc)      stop_arg = argv[++i];
        else if (!strcmp(argv[i], "--child") && i + 1 < argc)           child = argv[++i];
        else if (!strcmp(argv[i], "--processes") && i + 1 < argc)       min_procs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-processes") && i + 1 < argc)   max_procs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-requests") && i + 1 < argc) {
            max_requests = atoi(argv[++i]);
            child_opts.push_back(argv[i - 1]); child_opts.push_back(argv[i]);
        }
        else if (!strcmp(argv[i], "--grace") && i + 1 < argc)           grace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preload") && i + 1 < argc) {
            g_preload.push_back(argv[++i]);
            child_opts.push_back(argv[i - 1]); child_opts.push_back(argv[i]);
        }
        else if (!strcmp(argv[i], "--plugin") && i + 1 < argc) {
            if (!load_plugin(argv[++i])) return EXIT_CONFIG;
            child_opts.push_back(argv[i - 1]); child_opts.push_back(argv[i]);
        }
        else { usage(argv[0]); return EXIT_CONFIG; }
    }
    {
        char t[8];
        g_trace = GetEnvironmentVariableA("OORX_TRACE", t, sizeof t) > 0;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "oorexx-host: WSAStartup failed\n");
        return EXIT_CONFIG;
    }
    if (child) return run_child(child, max_requests);

    if (max_procs < 0) max_procs = min_procs;
    if ((path == NULL) == (listen_fd == INVALID_SOCKET) || (listen_fd != INVALID_SOCKET && !stop_arg)
        || min_procs < 1 || max_procs < min_procs || grace < 0) {
        usage(argv[0]);
        return EXIT_CONFIG;
    }

    pool p;
    p.pipes = 0;
    p.child_opts = child_opts;
    {
        char exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        if (n == 0 || n >= sizeof exe) { fprintf(stderr, "oorexx-host: own path?\n"); return 1; }
        p.exe = exe;
    }

    SOCKET lsock = listen_fd;
    if (path) {
        /* Standalone: bind the socket ourselves. Who may connect is what   */
        /* the directory's ACL allows: put it in a directory of your own.   */
        lsock = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        if (lsock == INVALID_SOCKET || strlen(path) >= sizeof sa.sun_path) {
            fprintf(stderr, "oorexx-host: socket path too long, or no socket\n");
            return EXIT_CONFIG;
        }
        strcpy(sa.sun_path, path);
        DeleteFileA(path);
        if (bind(lsock, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(lsock, 128) != 0) {
            fprintf(stderr, "oorexx-host: bind/listen: WSA error %d\n", WSAGetLastError());
            return EXIT_CONFIG;
        }
        g_stop_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        SetConsoleCtrlHandler(on_ctrl, TRUE);
    } else {
        /* Managed by mod_orx: the module created, bound and secured the    */
        /* socket and owns its path; we only accept on it, and drain when   */
        /* it sets the event.                                               */
        int type = 0, tl = sizeof type;
        if (getsockopt(lsock, SOL_SOCKET, SO_TYPE, (char *)&type, &tl) != 0 ||
            type != SOCK_STREAM) {
            fprintf(stderr, "oorexx-host: --listen-fd %llu is not a stream socket\n",
                    (unsigned long long)lsock);
            return EXIT_CONFIG;
        }
        g_stop_ev = handle_arg(stop_arg);
        SetHandleInformation(g_stop_ev, HANDLE_FLAG_INHERIT, 0);
    }
    /* Blocking: the children wait in accept() (see child_main). The mode   */
    /* belongs to the socket, shared by every process that holds it.        */
    u_long blocking = 0;
    ioctlsocket(lsock, FIONBIO, &blocking);
    SetHandleInformation((HANDLE)lsock, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    p.lsock = lsock;

    SECURITY_ATTRIBUTES isa = { sizeof isa, NULL, TRUE };
    DWORD bytes = (DWORD)(sizeof(struct slot) * max_procs);
    p.map = CreateFileMappingA(INVALID_HANDLE_VALUE, &isa, PAGE_READWRITE, 0, bytes, NULL);
    g_slots = p.map ? (struct slot *)MapViewOfFile(p.map, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    if (!g_slots) { fprintf(stderr, "oorexx-host: no shared slots\n"); return 1; }
    memset(g_slots, 0, bytes);
    p.stop = CreateEventA(&isa, TRUE, FALSE, NULL);
    p.mutex = CreateMutexA(&isa, FALSE, NULL);
    if (!p.stop || !p.mutex ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &p.self,
                         SYNCHRONIZE, TRUE, 0)) {
        fprintf(stderr, "oorexx-host: no stop event\n");
        return 1;
    }

    std::vector<HANDLE> procs(max_procs, (HANDLE)NULL);
    long long idle_since = -1;
    bool config_error = false;
    for (;;) {
        for (int i = 0; i < max_procs; i++) {
            if (!procs[i] || WaitForSingleObject(procs[i], 0) != WAIT_OBJECT_0) continue;
            DWORD code = 0;
            GetExitCodeProcess(procs[i], &code);
            TRACE("master: child %lld ended, status %lld", (long long)g_slots[i].pid, (long long)code);
            if (code != 0)
                fprintf(stderr, "oorexx-host: child %lu ended with status %lu\n",
                        g_slots[i].pid, code);
            CloseHandle(procs[i]); procs[i] = NULL;
            g_slots[i].pid = 0; slot_set(i, SLOT_EMPTY);
            /* A child that cannot start (its preload failed) would fail    */
            /* again in every replacement: stop, and say why on the way out. */
            if (code == EXIT_CONFIG) config_error = true;
        }
        if (config_error) break;

        int total = 0, ready = 0, idle = 0;
        for (int i = 0; i < max_procs; i++) {
            int s = slot_get(i);
            if (s == SLOT_EMPTY) continue;
            total++;
            if (s == SLOT_IDLE) idle++;
            if (s == SLOT_IDLE || s == SLOT_STARTING) ready++;
        }

        int want = 0;
        if (total < min_procs) want = min_procs - total;
        else if (ready == 0 && total < max_procs) want = 1;
        for (int i = 0; i < max_procs && want > 0; i++) {
            if (slot_get(i) != SLOT_EMPTY) continue;
            slot_set(i, SLOT_STARTING);
            DWORD pid = 0;
            HANDLE h = spawn_child(p, i, &pid);
            if (!h) {
                slot_set(i, SLOT_EMPTY);
                fprintf(stderr, "oorexx-host: cannot start a child: error %lu\n", GetLastError());
                break;
            }
            procs[i] = h;
            g_slots[i].pid = pid;
            TRACE("master: started child %lld in slot %lld", (long long)pid, i);
            want--;
        }

        /* Shrink: more than one child idle, and above the minimum, for     */
        /* SHRINK_AFTER_MS on end: retire one idle child, then wait again.  */
        if (idle > 1 && total > min_procs) {
            if (idle_since < 0) idle_since = now_ms();
            else if (now_ms() - idle_since >= SHRINK_AFTER_MS) {
                for (int i = 0; i < max_procs; i++)
                    if (slot_cas(i, SLOT_IDLE, SLOT_RETIRE)) break;
                idle_since = -1;
            }
        } else idle_since = -1;

        if (WaitForSingleObject(g_stop_ev, MASTER_TICK_MS) == WAIT_OBJECT_0) break;
    }

    /* Drain: tell the children, give them --grace seconds, then kill.      */
    SetEvent(p.stop);
    long long deadline = now_ms() + grace * 1000LL;
    for (;;) {
        int left = 0;
        for (int i = 0; i < max_procs; i++) {
            if (!procs[i]) continue;
            if (WaitForSingleObject(procs[i], 0) == WAIT_OBJECT_0) {
                CloseHandle(procs[i]); procs[i] = NULL;
                g_slots[i].pid = 0; slot_set(i, SLOT_EMPTY);
            } else left++;
        }
        if (left == 0 || now_ms() >= deadline) break;
        Sleep(MASTER_TICK_MS);
    }
    for (int i = 0; i < max_procs; i++)
        if (procs[i]) { TerminateProcess(procs[i], 1); CloseHandle(procs[i]); }
    if (path) { closesocket(lsock); DeleteFileA(path); }
    return config_error ? EXIT_CONFIG : 0;
}
#endif /* _WIN32 */
