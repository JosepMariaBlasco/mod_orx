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
 *   - Phases: content (a CGI program), and the hooks authn, authz and log.
 *     A hook program gets the CGI environment, its arguments (ARGS), and
 *     returns a value that the module reads as its verdict. Its output has
 *     no client to go to, so it goes to the error log, like .error.
 *   - A watchdog thread Halt()s the instance when the phase's deadline
 *     passes. It only ever uses the INSTANCE API: using a thread context
 *     from a thread other than its own aborts the process (measured).
 *
 *   - With --preload, every child keeps one instance alive (the ANCHOR) in
 *     which it has loaded the given packages once. While an instance lives,
 *     the interpreter does not reset between phases, and a program that
 *     ::Requires one of those packages finds it already loaded (measured:
 *     RexxHTTP's four classes, 4.6 ms -> 0.34 ms per phase). Only what the
 *     anchor loaded is shared; a program's own packages are still loaded
 *     afresh in every phase. A changed preloaded file is seen only by new
 *     processes (a restart of Apache, or recycling).
 *
 *   - SIGTERM drains: no child accepts anything new, the children finish
 *     what they hold and what is queued, and the master exits; after
 *     --grace seconds whatever is left is killed.
 *
 * Usage:
 *   oorexx-host (--socket PATH | --listen-fd N) [--processes MIN]
 *               [--max-processes MAX] [--max-requests N] [--grace SECONDS]
 *               [--preload FILE]...
 */

#include <oorexxapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <string>
#include <vector>

extern "C" {
#include "../include/oorexx_protocol.h"
}

/* ------------------------------------------------------------------------ */
/* Per-connection state. A child serves one connection at a time, so plain  */
/* globals are the honest representation.                                   */
/* ------------------------------------------------------------------------ */

static int         g_fd = -1;          /* the connection                    */
static std::string g_in;               /* body bytes received, not consumed */
static bool        g_in_eof = false;   /* the module said: no more body     */
static volatile bool g_broken = false; /* the connection failed: stop I/O   */

/* Two threads write frames on the connection: the main thread (READ, LOG,
 * RESULT) and the output pump (DATA). Only the main thread ever reads. A
 * frame is written under this lock, so frames never interleave.            */
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

static void send_frame(int type, int flags, const void *p, uint32_t n)
{
    if (g_broken) return;
    pthread_mutex_lock(&g_send_lock);
    if (oorx_send(g_fd, type, flags, p, n) < 0) g_broken = true;
    pthread_mutex_unlock(&g_send_lock);
}

static void send_log(int level, const char *msg, size_t n)
{
    std::string payload(1, (char)level);
    payload.append(msg, n);
    send_frame(OORX_LOG, 0, payload.data(), (uint32_t)payload.size());
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
#define QUIET_MS 10
static void *output_pump(void *arg)
{
    int rfd = (int)(intptr_t)arg;
    char buf[OORX_CHUNK];
    for (;;) {
        ssize_t n = read(rfd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        struct pollfd pf = { rfd, POLLIN, 0 };
        int more = poll(&pf, 1, QUIET_MS) > 0;
        send_frame(OORX_DATA, more ? 0 : OORX_FLAG_FLUSH, buf, (uint32_t)n);
    }
    close(rfd);
    return NULL;
}

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

static RexxRoutineEntry host_routines[] = {
    REXX_TYPED_ROUTINE(oorx_log,      oorx_log),
    REXX_TYPED_ROUTINE(oorx_read,     oorx_read),
    REXX_TYPED_ROUTINE(oorx_readline, oorx_readline),
    REXX_TYPED_ROUTINE(oorx_more,     oorx_more),
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

static pthread_mutex_t wd_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wd_cond = PTHREAD_COND_INITIALIZER;
static RexxInstance   *wd_instance = NULL;
static struct timespec wd_deadline;
static bool            wd_armed = false;
static bool            wd_fired = false;

static void *watchdog(void *)
{
    pthread_mutex_lock(&wd_lock);
    for (;;) {
        if (!wd_armed) { pthread_cond_wait(&wd_cond, &wd_lock); continue; }
        int rc = pthread_cond_timedwait(&wd_cond, &wd_lock, &wd_deadline);
        if (rc == ETIMEDOUT && wd_armed) {
            wd_armed = false;
            wd_fired = true;
            wd_instance->Halt();       /* instance API: safe from here */
        }
    }
    return NULL;
}

static void wd_arm(RexxInstance *inst, unsigned seconds)
{
    pthread_mutex_lock(&wd_lock);
    clock_gettime(CLOCK_REALTIME, &wd_deadline);
    wd_deadline.tv_sec += seconds;
    wd_instance = inst; wd_armed = seconds > 0; wd_fired = false;
    pthread_cond_signal(&wd_cond);
    pthread_mutex_unlock(&wd_lock);
}

static bool wd_disarm(void)
{
    pthread_mutex_lock(&wd_lock);
    bool fired = wd_fired;
    wd_armed = false; wd_instance = NULL;
    pthread_cond_signal(&wd_cond);
    pthread_mutex_unlock(&wd_lock);
    return fired;
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

static void serve(int fd)
{
    g_fd = fd; g_in.clear(); g_in_eof = false; g_broken = false;

    /* BEGIN */
    int type, flags; uint32_t len;
    if (oorx_recv_header(fd, &type, &flags, &len) < 0 || type != OORX_BEGIN || len < 7)
        return;
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
    /* The content phase runs CGI programs; the hooks (authn, authz, log)   */
    /* run native ones: arguments in, a verdict out.                        */
    bool hook = phase == OORX_PHASE_AUTHN || phase == OORX_PHASE_AUTHZ ||
                phase == OORX_PHASE_LOG;
    if (!(phase == OORX_PHASE_CONTENT && klass == OORX_CLASS_CGI) &&
        !(hook && klass == OORX_CLASS_NATIVE)) {
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
    /* VARS: becomes the process environment, exactly as a CGI's.          */
    if (type != OORX_VARS) return;
    std::string vars(len, '\0');
    if (len && oorx_read_all(fd, &vars[0], len) < 0) return;
    clearenv();
    for (size_t p = 0; p + 4 <= vars.size(); ) {
        uint32_t kl = oorx_get_u32((const unsigned char *)&vars[p]); p += 4;
        if (p + kl + 4 > vars.size()) break;
        std::string k = vars.substr(p, kl); p += kl;
        uint32_t vl = oorx_get_u32((const unsigned char *)&vars[p]); p += 4;
        if (p + vl > vars.size()) break;
        std::string v = vars.substr(p, vl); p += vl;
        setenv(k.c_str(), v.c_str(), 1);
    }

    /* Like mod_cgi: run in the program's directory.                        */
    std::string dir = program.substr(0, program.rfind('/') + 1);
    if (!dir.empty() && chdir(dir.c_str()) < 0) { /* not fatal */ }

    /* A fresh instance for this phase.                                     */
    RexxLibraryPackage lib = { "OORXHOST", &host_package };
    RexxOption opts[2];
    opts[0].optionName = REGISTER_LIBRARY; opts[0].option = (void *)&lib;
    opts[1].optionName = NULL;
    RexxInstance *inst; RexxThreadContext *tc;
    if (!RexxCreateInterpreter(&inst, &tc, opts)) {
        logf_conn("oorexx-host: cannot create an interpreter instance%s", "");
        send_result(500);
        return;
    }

    uint32_t status = 0;
    std::string value;
    bool has_value = false;
    int pfd[2];
    pthread_t pump;
    if (pipe(pfd) < 0) {
        inst->Terminate();
        logf_conn("oorexx-host: cannot create the output pipe%s", "");
        send_result(500);
        return;
    }
    pthread_create(&pump, NULL, output_pump, (void *)(intptr_t)pfd[0]);
    char pipe_path[32];
    snprintf(pipe_path, sizeof pipe_path, "/dev/fd/%d", pfd[1]);

    RexxRoutineObject boot = tc->NewRoutine("oorexx-host bootstrap",
                                            bootstrap_src, sizeof bootstrap_src - 1);
    RexxObjectPtr out = NULLOBJECT;
    if (boot != NULLOBJECT && !tc->CheckCondition()) {
        RexxArrayObject bargs = tc->ArrayOfTwo(tc->String(pipe_path),
                                               hook ? tc->True() : tc->False());
        out = tc->CallRoutine(boot, bargs);
    }
    if (out == NULLOBJECT || tc->CheckCondition()) {
        tc->ClearCondition();
        logf_conn("oorexx-host: bootstrap failed%s", "");
        status = 500;
    } else {
        RexxArrayObject pargs = NULLOBJECT;
        if (!args.empty()) {
            pargs = tc->NewArray(args.size());
            for (size_t i = 0; i < args.size(); i++)
                tc->ArrayPut(pargs, tc->NewString(args[i].data(), args[i].size()), i + 1);
        }
        wd_arm(inst, timeout);
        RexxObjectPtr ret = tc->CallProgram(program.c_str(), pargs);
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
            /* A CGI's failure shows in its output; a hook's is its verdict. */
            if (hook) { status = 500; has_value = false; }
        }
        /* Close the program's output: flushes what the stream still holds. */
        tc->SendMessage0(out, "CLOSE");
        tc->ClearCondition();
    }
    inst->Terminate();
    close(pfd[1]);
    pthread_join(pump, NULL);          /* all output forwarded */
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

struct slot { int state; pid_t pid; };

static struct slot *g_slots;        /* shared page, one per child           */

static bool slot_cas(int i, int from, int to)
{
    return __atomic_compare_exchange_n(&g_slots[i].state, &from, to, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void slot_set(int i, int v) { __atomic_store_n(&g_slots[i].state, v, __ATOMIC_SEQ_CST); }
static int  slot_get(int i)        { return __atomic_load_n(&g_slots[i].state, __ATOMIC_SEQ_CST); }

/* How often an idle child looks at its slot, and how long the pool must    */
/* have idle children to spare before the master retires one.              */
#define CHILD_POLL_MS    500
#define MASTER_TICK_MS   200
#define SHRINK_AFTER_MS  10000

/* Exit status 2 means "the configuration is wrong": mod_orx does not       */
/* restart a host that exits with it, since it would only fail again.       */
#define EXIT_CONFIG 2

static std::vector<std::string> g_preload;   /* --preload FILE, in order    */

/* The anchor: an instance that is never terminated, with the preloaded     */
/* packages loaded in it by a program made of one ::Requires per file. It   */
/* lives in the child's main thread, like every instance of the child, and  */
/* the watchdog never sees it. A package that fails to load is a            */
/* configuration error: the child exits with EXIT_CONFIG, and the master    */
/* with it.                                                                 */
static void load_anchor(void)
{
    RexxInstance *inst; RexxThreadContext *tc;
    if (!RexxCreateInterpreter(&inst, &tc, NULL)) {
        fprintf(stderr, "oorexx-host: cannot create the preload instance\n");
        _exit(EXIT_CONFIG);
    }
    std::string src = "return\n";
    for (size_t i = 0; i < g_preload.size(); i++) {
        std::string q;
        for (char ch : g_preload[i]) { q += ch; if (ch == '"') q += '"'; }
        src += "::requires \"" + q + "\"\n";
    }
    RexxRoutineObject r = tc->NewRoutine("oorexx-host preload", src.data(), src.size());
    /* Held for good: the routine keeps the packages it required loaded.  */
    if (r != NULLOBJECT) tc->RequestGlobalReference(r);
    if (r != NULLOBJECT && !tc->CheckCondition()) tc->CallRoutine(r, NULL);
    if (r == NULLOBJECT || tc->CheckCondition()) {
        RexxCondition c;
        std::string m = "oorexx-host: OoRexxPreload failed";
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
        /* One write: every child fails alike, and lines must not mix.     */
        m += "\n";
        if (write(2, m.data(), m.size()) < 0) { /* nowhere else to say it */ }
        _exit(EXIT_CONFIG);
    }
}

static void child_main(int me, pid_t master, int lsock, int stopfd, int max_requests)
{
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);  /* never outlive the master */
    if (getppid() != master) _exit(0);
#endif
    signal(SIGTERM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    pthread_t t;
    pthread_create(&t, NULL, watchdog, NULL);

    /* Pre-warm: the first instance of a process is the expensive one.      */
    /* With --preload, the anchor is that first instance, and it stays.     */
    if (!g_preload.empty()) load_anchor();
    else {
        RexxInstance *inst; RexxThreadContext *tc;
        if (RexxCreateInterpreter(&inst, &tc, NULL)) inst->Terminate();
    }
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
        "       [--preload FILE]...\n", argv0);
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
        else { usage(argv[0]); return EXIT_CONFIG; }
    }
    if (max_procs < 0) max_procs = min_procs;
    if ((path == NULL) == (listen_fd < 0) || min_procs < 1 || max_procs < min_procs
        || grace < 0) {
        usage(argv[0]);
        return EXIT_CONFIG;
    }

    int lsock = listen_fd;
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
        /* The keeper takes its signals synchronously (sigwaitinfo), so   */
        /* they are blocked from before the fork: none can be lost.        */
        sigset_t ks, old;
        sigemptyset(&ks);
        sigaddset(&ks, SIGTERM); sigaddset(&ks, SIGINT); sigaddset(&ks, SIGCHLD);
        sigprocmask(SIG_BLOCK, &ks, &old);
        pid_t m = fork();
        if (m < 0) { perror("oorexx-host: fork"); return 1; }
        if (m > 0) {
            signal(SIGHUP,  SIG_IGN);
            signal(SIGUSR1, SIG_IGN);
            signal(SIGUSR2, SIG_IGN);
            close(lsock);
            int st;
            for (;;) {
                if (waitpid(m, &st, WNOHANG) == m) break;
                int sig = sigwaitinfo(&ks, NULL);
                if (sig == SIGTERM || sig == SIGINT) { kill(m, SIGTERM); _exit(0); }
            }
            /* Never 15 or 7 (Apache's APEXIT_CHILDFATAL and CHILDSICK).    */
            _exit(WIFEXITED(st) && WEXITSTATUS(st) == EXIT_CONFIG ? EXIT_CONFIG : 1);
        }
        sigprocmask(SIG_SETMASK, &old, NULL);
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM);       /* the keeper died: drain */
#endif
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
