/* Probe: can a Rexx program's output be a real .Stream on a named pipe?
 *
 * On Unix, oorexx-host opens the program's output as .Stream~new on
 * /dev/fd/N, the write end of a pipe that a thread (the output pump)
 * forwards to the module, asking for a flush when the program goes quiet
 * for 10 ms. Windows has no /dev/fd; this checks the same with a named
 * pipe: whether .Stream opens it (WRITE APPEND, as the bootstrap does,
 * and other modes), what bytes arrive, and whether an explicit flush, or
 * none, gets part of the output across before the program ends.
 *
 *   pipestream.exe
 *
 * Copyright 2026 Josep Maria Blasco -- Apache License 2.0
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include "oorexxapi.h"

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* What the pump saw: chunks, with their time and whether a quiet gap of
 * QUIET_MS followed (the flush mod_orx would ask for). */
#define QUIET_MS 10
struct chunk { double t; std::string data; bool quiet_after; };
struct pump { HANDLE pipe; std::vector<chunk> got; double t0; DWORD error; };

static DWORD WINAPI pump_thread(LPVOID arg)
{
    pump *p = (pump *)arg;
    OVERLAPPED ov; memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ConnectNamedPipe(p->pipe, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) WaitForSingleObject(ov.hEvent, INFINITE);
        else if (e != ERROR_PIPE_CONNECTED) { p->error = e; return 0; }
    }
    char buf[4096];
    bool pending = false;
    for (;;) {
        DWORD n = 0;
        if (!pending) {
            ResetEvent(ov.hEvent);
            if (!ReadFile(p->pipe, buf, sizeof buf, NULL, &ov)) {
                DWORD e = GetLastError();
                if (e == ERROR_BROKEN_PIPE) break;
                if (e != ERROR_IO_PENDING) { p->error = e; break; }
            }
            pending = true;
        }
        WaitForSingleObject(ov.hEvent, INFINITE);
        pending = false;
        if (!GetOverlappedResult(p->pipe, &ov, &n, FALSE)) {
            DWORD e = GetLastError();
            if (e != ERROR_BROKEN_PIPE) p->error = e;
            break;
        }
        chunk c; c.t = now_ms() - p->t0; c.data.assign(buf, n); c.quiet_after = false;
        p->got.push_back(c);
        /* As the pump: start the next read, and see whether it completes   */
        /* within QUIET_MS; if not, the client would get a flush now.       */
        ResetEvent(ov.hEvent);
        if (!ReadFile(p->pipe, buf, sizeof buf, NULL, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE) break;
            if (e != ERROR_IO_PENDING) { p->error = e; break; }
        }
        pending = true;
        if (WaitForSingleObject(ov.hEvent, QUIET_MS) == WAIT_TIMEOUT) p->got.back().quiet_after = true;
    }
    CloseHandle(ov.hEvent);
    return 0;
}

static std::string visible(const std::string &s)
{
    std::string r;
    for (unsigned char ch : s) {
        if (ch == '\r') r += "\\r"; else if (ch == '\n') r += "\\n";
        else if (ch < 32 || ch > 126) { char b[8]; sprintf(b, "\\x%02x", ch); r += b; }
        else r += (char)ch;
    }
    return r;
}

/* Runs src (which gets the pipe's name as its argument) in a fresh
 * instance, with a pump on a fresh pipe; prints what arrived. */
static int run_case(const char *label, const char *src, const char *expect_no_cr, bool duplex = false)
{
    static int seq = 0;
    char name[128];
    snprintf(name, sizeof name, "\\\\.\\pipe\\orx-out-%lu-%d", GetCurrentProcessId(), ++seq);
    pump p; p.error = 0;
    p.pipe = CreateNamedPipeA(name, (duplex ? PIPE_ACCESS_DUPLEX : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                              1, 65536, 65536, 0, NULL);
    if (p.pipe == INVALID_HANDLE_VALUE) { printf("%s: CreateNamedPipe failed, %lu\n", label, GetLastError()); return 1; }
    p.t0 = now_ms();
    HANDLE th = CreateThread(NULL, 0, pump_thread, &p, 0, NULL);

    RexxInstance *inst; RexxThreadContext *tc;
    if (!RexxCreateInterpreter(&inst, &tc, NULL)) { printf("%s: no instance\n", label); return 1; }
    std::string ret = "(none)";
    RexxRoutineObject r = tc->NewRoutine(label, src, strlen(src));
    if (r != NULLOBJECT) {
        RexxObjectPtr res = tc->CallRoutine(r, tc->ArrayOfOne(tc->String(name)));
        if (tc->CheckCondition()) {
            RexxCondition c;
            tc->DecodeConditionInfo(tc->GetConditionInfo(), &c);
            char m[512];
            snprintf(m, sizeof m, "(condition: Error %d line %d: %s)", (int)c.rc, (int)c.position,
                     c.message != NULLOBJECT ? tc->CString(c.message) : "");
            ret = m;
            tc->ClearCondition();
        } else if (res != NULLOBJECT) ret = tc->CString(res);
    } else ret = "(NewRoutine failed)";
    double tend = now_ms() - p.t0;
    inst->Terminate();
    /* The pump ends when the last writer closes; a program that never      */
    /* opened the pipe leaves it waiting: give up after a second.           */
    if (WaitForSingleObject(th, 1000) == WAIT_TIMEOUT) { CancelIoEx(p.pipe, NULL); WaitForSingleObject(th, 1000); }
    CloseHandle(p.pipe);

    std::string all;
    for (auto &c : p.got) all += c.data;
    printf("== %s\n   returned: %s (program ended at %.0f ms)\n", label, ret.c_str(), tend);
    for (auto &c : p.got)
        printf("   %7.1f ms: %3zu bytes \"%s\"%s\n", c.t, c.data.size(), visible(c.data).c_str(),
               c.quiet_after ? "  [quiet: flush]" : "");
    if (p.error) printf("   pump error %lu\n", p.error);
    std::string nocr;
    for (char ch : all) if (ch != '\r') nocr += ch;
    bool ok = expect_no_cr == NULL || nocr == expect_no_cr;
    printf("RESULT %s %s: %zu bytes in %zu chunk(s)\n", label, ok ? "ok" : "FAIL", all.size(), p.got.size());
    return ok ? 0 : 1;
}

/* A: what _sopen (the CRT call SysFile::open makes) does on a pipe, with
 * the flags ooRexx uses for WRITE: O_RDWR|O_CREAT first, then
 * O_WRONLY|O_CREAT; both with _O_NOINHERIT|_O_BINARY and _SH_DENYWR. */
static void sopen_matrix(void)
{
    struct { const char *n; int f; } fl[] = {
        { "O_RDWR|O_CREAT",   _O_RDWR | _O_CREAT },
        { "O_WRONLY|O_CREAT", _O_WRONLY | _O_CREAT },
        { "O_WRONLY",         _O_WRONLY },
        { "O_RDWR",           _O_RDWR },
    };
    for (int dup = 0; dup < 2; dup++)
        for (auto &f : fl) {
            char name[128];
            static int k = 0;
            snprintf(name, sizeof name, "\\\\.\\pipe\\orx-sopen-%lu-%d", GetCurrentProcessId(), ++k);
            HANDLE h = CreateNamedPipeA(name, (dup ? PIPE_ACCESS_DUPLEX : PIPE_ACCESS_INBOUND),
                                        PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
            errno = 0;
            int fd = _sopen(name, f.f | _O_NOINHERIT | _O_BINARY, _SH_DENYWR, _S_IREAD | _S_IWRITE);
            int e = errno; DWORD w = GetLastError();
            printf("   _sopen %-17s on a %-7s pipe: %s (errno %d, Win32 %lu)\n", f.n,
                   dup ? "duplex" : "inbound", fd >= 0 ? "OPENS" : "fails", fd >= 0 ? 0 : e, fd >= 0 ? 0 : w);
            if (fd >= 0) _close(fd);
            CloseHandle(h);
        }
}

/* B: the child of the pool born with its standard output on a pipe that
 * lasts as long as it does; every phase's program writes to 'STDOUT'. */
static int stdout_child(void)
{
    RexxInstance *anchor; RexxThreadContext *atc;
    if (!RexxCreateInterpreter(&anchor, &atc, NULL)) { fprintf(stderr, "child: no anchor\n"); return 1; }
    const char *src =
        "use arg n; out = .Stream~new('STDOUT'); r = out~open('WRITE APPEND')\n"
        ".output~destination(out); .local~stdout = out\n"
        "say 'req' n; out~flush; call SysSleep 0.2\n"
        "call charout , 'bin:' || '00010a0d1a7fff'x || ';'\n"
        "out~close; return r out~class~id";
    for (int i = 1; i <= 3; i++) {
        RexxInstance *inst; RexxThreadContext *tc;
        RexxCreateInterpreter(&inst, &tc, NULL);
        RexxRoutineObject r = tc->NewRoutine("phase", src, strlen(src));
        RexxObjectPtr res = tc->CallRoutine(r, tc->ArrayOfOne(tc->WholeNumber(i)));
        if (tc->CheckCondition()) {
            RexxCondition c; tc->DecodeConditionInfo(tc->GetConditionInfo(), &c);
            fprintf(stderr, "child: phase %d: condition, Error %d\n", i, (int)c.rc);
            tc->ClearCondition();
        } else fprintf(stderr, "child: phase %d returned %s\n", i, res ? tc->CString(res) : "(none)");
        inst->Terminate();
        Sleep(300);
    }
    anchor->Terminate();
    return 0;
}

static int stdout_parent(void)
{
    char name[128];
    snprintf(name, sizeof name, "\\\\.\\pipe\\orx-stdout-%lu", GetCurrentProcessId());
    pump p; p.error = 0;
    p.pipe = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE w = CreateFileA(name, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
    if (w == INVALID_HANDLE_VALUE) { printf("B: cannot open the pipe's client end, %lu\n", GetLastError()); return 1; }
    p.t0 = now_ms();
    HANDLE th = CreateThread(NULL, 0, pump_thread, &p, 0, NULL);
    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" stdout-child", exe);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = w; si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { printf("B: CreateProcess %lu\n", GetLastError()); return 1; }
    CloseHandle(w);                   /* the child holds the only write end */
    WaitForSingleObject(pi.hProcess, 30000);
    WaitForSingleObject(th, 5000);
    CloseHandle(p.pipe);
    std::string all;
    printf("== B: 'STDOUT' on a pipe that lasts the child's life, three phases\n");
    for (auto &c : p.got) {
        all += c.data;
        printf("   %7.1f ms: %3zu bytes \"%s\"%s\n", c.t, c.data.size(), visible(c.data).c_str(),
               c.quiet_after ? "  [quiet: flush]" : "");
    }
    std::string one = std::string("bin:") + std::string("\x00\x01\x0a\x0d\x1a\x7f\xff", 7) + ";";
    std::string want;
    for (int i = 1; i <= 3; i++) want += "req " + std::to_string(i) + "\r\n" + one;
    bool ok = all == want;
    printf("RESULT B stdout %s: %zu bytes (expected %zu, exactly)\n", ok ? "ok" : "FAIL", all.size(), want.size());
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "stdout-child")) return stdout_child();
    printf("== A: _sopen on a named pipe\n");
    sopen_matrix();
    RexxInstance *anchor; RexxThreadContext *atc;
    if (!RexxCreateInterpreter(&anchor, &atc, NULL)) { printf("no anchor\n"); return 1; }
    int f = 0;
    f += run_case("open WRITE APPEND, lineout and charout",
        "use arg pipe; out = .Stream~new(pipe); r = out~open('WRITE APPEND')\n"
        "out~lineout('line1'); out~charout('abc'); out~close; return r",
        "line1\nabc");
    f += run_case("open WRITE REPLACE",
        "use arg pipe; out = .Stream~new(pipe); r = out~open('WRITE REPLACE')\n"
        "out~lineout('x'); out~close; return r", "x\n");
    f += run_case("open WRITE",
        "use arg pipe; out = .Stream~new(pipe); r = out~open('WRITE')\n"
        "out~lineout('x'); out~close; return r", "x\n");
    f += run_case("explicit flush, then 300 ms, then more",
        "use arg pipe; out = .Stream~new(pipe); r = out~open('WRITE APPEND')\n"
        "out~lineout('part1'); out~flush; call SysSleep 0.3; out~lineout('part2'); out~close; return r",
        "part1\npart2\n");
    f += run_case("no flush, 300 ms between",
        "use arg pipe; out = .Stream~new(pipe); r = out~open('WRITE APPEND')\n"
        "out~lineout('part1'); call SysSleep 0.3; out~lineout('part2'); out~close; return r",
        "part1\npart2\n");
    f += run_case("as the bootstrap: Say through .output and .stdout",
        "use arg pipe; out = .Stream~new(pipe); r = out~open('WRITE APPEND')\n"
        ".output~destination(out); .local~stdout = out\n"
        "say 'Content-Type: text/plain'; say ''; call charout , 'body'; .stdout~lineout('!')\n"
        "out~close; return r",
        "Content-Type: text/plain\n\nbody!\n");
    f += run_case("a binary byte sequence (charout)",
        "use arg pipe; out = .Stream~new(pipe); r = out~open('WRITE APPEND')\n"
        "out~charout('00010a0d1a7fff'x); out~close; return r", NULL);
    f += run_case("duplex pipe: open WRITE APPEND, lineout and charout",
        "use arg pipe; out = .Stream~new(pipe); r = out~open('WRITE APPEND')\n"
        "out~lineout('line1'); out~charout('abc'); out~close; return r",
        "line1\nabc", true);
    anchor->Terminate();
    int b = stdout_parent();
    printf("RESULT pipestream A (a named pipe) %s; B (STDOUT) %s\n", f ? "FAIL" : "ok", b ? "FAIL" : "ok");
    return 0;
}
