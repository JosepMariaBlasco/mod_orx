/*
 * oorexx-host -- runs Rexx programs on behalf of mod_oorexx.
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * Process model (see the design notes in README.md):
 *
 *   - A master process binds one Unix socket and forks a pool of children.
 *     The master never touches ooRexx: it only keeps the pool alive.
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
 *   - A watchdog thread Halt()s the instance when the phase's deadline
 *     passes. It only ever uses the INSTANCE API: using a thread context
 *     from a thread other than its own aborts the process (measured).
 *
 * Usage:
 *   oorexx-host --socket PATH [--processes N] [--max-requests N]
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
/* The bootstrap: run in each fresh instance before the program, with the */
/* path of the output pipe as its argument. It points every standard       */
/* stream at the connection: output (Say, lineout(), charout(), .output,   */
/* .stdout) to a real .Stream on the pipe; input (parse pull, linein(),    */
/* charin(), .stdin) to the request body; .error, .stderr and trace output */
/* to Apache's error log. .stdout and friends are process-wide objects, so */
/* they must be replaced in .local, not only behind the monitors           */
/* (measured). Returns the output stream, which the host closes at the end.*/
/* ------------------------------------------------------------------------ */

static const char bootstrap_src[] =
"use strict arg pipe\n"
"out = .Stream~new(pipe)\n"
"if out~open('WRITE APPEND') \\= 'READY:' then raise syntax 98.900 array('cannot open' pipe)\n"
"in  = .OoRexxIn~new\n"
"err = .OoRexxErr~new\n"
".output~destination(out); .local~stdout = out\n"
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

static void send_result(uint32_t status)
{
    unsigned char s[4];
    oorx_put_u32(s, status);
    send_frame(OORX_RESULT, 0, s, 4);
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
    if (phase != OORX_PHASE_CONTENT || klass != OORX_CLASS_CGI) {
        logf_conn("oorexx-host: phase/class not implemented yet for %s", program.c_str());
        send_result(501);
        return;
    }

    /* VARS: becomes the process environment, exactly as a CGI's.          */
    if (oorx_recv_header(fd, &type, &flags, &len) < 0 || type != OORX_VARS) return;
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
        RexxArrayObject args = tc->ArrayOfOne(tc->String(pipe_path));
        out = tc->CallRoutine(boot, args);
    }
    if (out == NULLOBJECT || tc->CheckCondition()) {
        tc->ClearCondition();
        logf_conn("oorexx-host: bootstrap failed%s", "");
        status = 500;
    } else {
        wd_arm(inst, timeout);
        tc->CallProgram(program.c_str(), NULLOBJECT);
        bool halted = wd_disarm();
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
        }
        /* Close the program's output: flushes what the stream still holds. */
        tc->SendMessage0(out, "CLOSE");
        tc->ClearCondition();
    }
    inst->Terminate();
    close(pfd[1]);
    pthread_join(pump, NULL);          /* all output forwarded */
    send_result(status);
}

/* ------------------------------------------------------------------------ */
/* Pool                                                                     */
/* ------------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;
static void on_term(int) { g_stop = 1; }

static void child_main(int lsock, int max_requests)
{
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);  /* never outlive the master */
#endif
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    pthread_t t;
    pthread_create(&t, NULL, watchdog, NULL);

    /* Pre-warm: the first instance of a process is the expensive one.      */
    RexxInstance *inst; RexxThreadContext *tc;
    if (RexxCreateInterpreter(&inst, &tc, NULL)) inst->Terminate();

    for (int served = 0; max_requests <= 0 || served < max_requests; served++) {
        int fd = accept(lsock, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) { served--; continue; } _exit(1); }
        serve(fd);
        close(fd);
    }
    _exit(0);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int processes = 4, max_requests = 1000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc)            path = argv[++i];
        else if (!strcmp(argv[i], "--processes") && i + 1 < argc)    processes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-requests") && i + 1 < argc) max_requests = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s --socket PATH [--processes N] [--max-requests N]\n", argv[0]); return 2; }
    }
    if (!path || processes < 1) { fprintf(stderr, "oorexx-host: --socket is required\n"); return 2; }

    int lsock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) { fprintf(stderr, "oorexx-host: socket path too long\n"); return 1; }
    strcpy(sa.sun_path, path);
    unlink(path);
    if (bind(lsock, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lsock, 128) < 0) {
        perror("oorexx-host: bind/listen"); return 1;
    }
    /* Only the user the host runs as (Apache's) may connect.               */
    chmod(path, 0600);

    /* No SA_RESTART: a signal must interrupt wait() so the loop sees it.   */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof sa_term);
    sa_term.sa_handler = on_term;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);
    std::vector<pid_t> kids(processes, 0);
    while (!g_stop) {
        for (int i = 0; i < processes; i++) {
            if (kids[i] == 0) {
                pid_t p = fork();
                if (p == 0) child_main(lsock, max_requests);
                kids[i] = p > 0 ? p : 0;
            }
        }
        int st;
        pid_t dead = wait(&st);
        if (dead > 0)
            for (int i = 0; i < processes; i++) if (kids[i] == dead) kids[i] = 0;
    }
    /* SIGKILL, not SIGTERM: ooRexx installs its own handlers in every
     * process that creates an instance, and turns SIGTERM into a HALT.
     * The children hold no state worth a graceful exit.                    */
    for (int i = 0; i < processes; i++) if (kids[i] > 0) kill(kids[i], SIGKILL);
    while (wait(NULL) > 0) ;
    unlink(path);
    return 0;
}
