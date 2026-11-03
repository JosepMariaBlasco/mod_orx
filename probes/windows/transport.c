/* Probe: can several processes take connections from one endpoint on
 * Windows, as oorexx-host's children do on Unix (all accept() on one
 * listening socket)?
 *
 *   transport.exe afunix-inherit N   one AF_UNIX listening socket, inherited
 *   transport.exe afunix-dup N       the same, passed with WSADuplicateSocket
 *   transport.exe pipe N             a named pipe, one instance per process
 *
 * The parent starts N children, then 12 clients at once (threads), each of
 * which connects, sends a byte and reads the PID of the child that served
 * it; a child takes 200 ms per connection, so the clients must queue. Then
 * a last check: with every child busy, a client that may wait 50 ms only
 * must be told so (the queue timeout mod_orx needs).
 *
 * Prints one RESULT line: answered, distinct PIDs, elapsed time.
 *
 * Copyright 2026 Josep Maria Blasco -- Apache License 2.0
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLIENTS  12
#define WORK_MS  200

static char g_path[MAX_PATH];      /* the AF_UNIX path or the pipe's name */
static int  g_pipe;                /* the transport the clients use       */
static DWORD g_pids[CLIENTS];
static DWORD g_retries[CLIENTS];
static int  g_errors[CLIENTS];

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* ---------------------------------------------------------------- children */

static int serve_socket(SOCKET s)
{
    for (;;) {
        SOCKET c = accept(s, NULL, NULL);
        if (c == INVALID_SOCKET) {
            printf("  child %lu: accept failed, WSA error %d\n", GetCurrentProcessId(), WSAGetLastError());
            fflush(stdout);
            return 1;
        }
        char b[16], out[32];
        recv(c, b, sizeof b, 0);
        Sleep(WORK_MS);
        int n = sprintf(out, "%lu", GetCurrentProcessId());
        send(c, out, n, 0);
        closesocket(c);
    }
}

static int child_afunix_inherit(const char *h)
{
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    return serve_socket((SOCKET)_strtoui64(h, NULL, 10));
}

static int child_afunix_dup(void)
{
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    WSAPROTOCOL_INFOW info; DWORD got = 0, n;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    while (got < sizeof info && ReadFile(in, (char *)&info + got, sizeof info - got, &n, NULL) && n)
        got += n;
    if (got != sizeof info) { printf("  child: short protocol info (%lu)\n", got); return 1; }
    SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, 0);
    if (s == INVALID_SOCKET) {
        printf("  child %lu: WSASocket from the protocol info failed, WSA error %d\n",
               GetCurrentProcessId(), WSAGetLastError());
        fflush(stdout);
        return 1;
    }
    return serve_socket(s);
}

static int child_pipe(const char *name)
{
    HANDLE h = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  child %lu: CreateNamedPipe failed, error %lu\n", GetCurrentProcessId(), GetLastError());
        fflush(stdout);
        return 1;
    }
    for (;;) {
        if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            printf("  child %lu: ConnectNamedPipe failed, error %lu\n", GetCurrentProcessId(), GetLastError());
            fflush(stdout);
            return 1;
        }
        char b[16], out[32]; DWORD n;
        ReadFile(h, b, sizeof b, &n, NULL);
        Sleep(WORK_MS);
        int k = sprintf(out, "%lu", GetCurrentProcessId());
        WriteFile(h, out, (DWORD)k, &n, NULL);
        FlushFileBuffers(h);
        DisconnectNamedPipe(h);
    }
}

/* ---------------------------------------------------------------- clients */

/* Connects with a wait of at most wait_ms; returns the served PID, 0 on a
 * timeout, -1 on another error (in *err). */
static long client(int wait_ms, DWORD *retries, int *err)
{
    char b[32];
    *retries = 0; *err = 0;
    if (g_pipe) {
        HANDLE h;
        double end = now_ms() + wait_ms;
        for (;;) {
            h = CreateFileA(g_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_PIPE_BUSY) { *err = (int)GetLastError(); return -1; }
            double left = end - now_ms();
            if (left <= 0) return 0;
            (*retries)++;
            if (!WaitNamedPipeA(g_path, (DWORD)left) && GetLastError() == ERROR_SEM_TIMEOUT) return 0;
        }
        DWORD n;
        WriteFile(h, "x", 1, &n, NULL);
        BOOL ok = ReadFile(h, b, sizeof b - 1, &n, NULL);
        CloseHandle(h);
        if (!ok) { *err = (int)GetLastError(); return -1; }
        b[n] = 0;
        return atol(b);
    } else {
        SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX; strcpy(sa.sun_path, g_path);
        DWORD tmo = (DWORD)wait_ms;   /* what the module would wait for a reply */
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
        if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
            *err = WSAGetLastError(); closesocket(s); return -1;
        }
        send(s, "x", 1, 0);
        int n = recv(s, b, sizeof b - 1, 0);
        int e = WSAGetLastError();
        closesocket(s);
        if (n <= 0) { if (e == WSAETIMEDOUT) return 0; *err = e; return -1; }
        b[n] = 0;
        return atol(b);
    }
}

static DWORD WINAPI client_thread(LPVOID arg)
{
    int i = (int)(INT_PTR)arg;
    long pid = client(10000, &g_retries[i], &g_errors[i]);
    g_pids[i] = pid > 0 ? (DWORD)pid : 0;
    return 0;
}

/* ---------------------------------------------------------------- parent */

static HANDLE spawn(const char *args, HANDLE in, BOOL inherit, DWORD *pid)
{
    char exe[MAX_PATH], cmd[2 * MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (in) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, inherit, 0, NULL, NULL, &si, &pi)) {
        printf("  CreateProcess failed, error %lu\n", GetLastError());
        return NULL;
    }
    CloseHandle(pi.hThread);
    *pid = pi.dwProcessId;
    return pi.hProcess;
}

static int parent(const char *mode, int nchildren)
{
    HANDLE kids[64]; DWORD kpid[64]; int nk = 0;
    SOCKET ls = INVALID_SOCKET;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    if (nchildren < 1 || nchildren > 64) nchildren = 3;
    printf("== %s, %d children\n", mode, nchildren);

    if (!strcmp(mode, "pipe")) {
        g_pipe = 1;
        snprintf(g_path, sizeof g_path, "\\\\.\\pipe\\orx-probe-%lu", GetCurrentProcessId());
        for (int i = 0; i < nchildren; i++) {
            char a[MAX_PATH + 16]; snprintf(a, sizeof a, "child-pipe %s", g_path);
            if ((kids[nk] = spawn(a, NULL, FALSE, &kpid[nk])) != NULL) nk++;
        }
    } else {
        char tmp[MAX_PATH]; GetTempPathA(sizeof tmp, tmp);
        snprintf(g_path, sizeof g_path, "%sorx-probe-%lu.sock", tmp, GetCurrentProcessId());
        DeleteFileA(g_path);
        ls = socket(AF_UNIX, SOCK_STREAM, 0);
        if (ls == INVALID_SOCKET) { printf("RESULT %s FAIL: socket(AF_UNIX) WSA error %d\n", mode, WSAGetLastError()); return 1; }
        struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX; strcpy(sa.sun_path, g_path);
        if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 64) != 0) {
            printf("RESULT %s FAIL: bind/listen WSA error %d\n", mode, WSAGetLastError()); return 1;
        }
        if (!strcmp(mode, "afunix-inherit")) {
            if (!SetHandleInformation((HANDLE)ls, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                printf("  SetHandleInformation failed, error %lu\n", GetLastError());
            for (int i = 0; i < nchildren; i++) {
                char a[64]; snprintf(a, sizeof a, "child-afunix-inherit %llu", (unsigned long long)ls);
                if ((kids[nk] = spawn(a, NULL, TRUE, &kpid[nk])) != NULL) nk++;
            }
        } else {
            for (int i = 0; i < nchildren; i++) {
                HANDLE rd, wr;
                SECURITY_ATTRIBUTES sa2 = { sizeof sa2, NULL, TRUE };
                if (!CreatePipe(&rd, &wr, &sa2, 0)) { printf("  CreatePipe failed\n"); break; }
                SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
                if ((kids[nk] = spawn("child-afunix-dup", rd, TRUE, &kpid[nk])) == NULL) break;
                CloseHandle(rd);
                WSAPROTOCOL_INFOW info; DWORD n;
                if (WSADuplicateSocketW(ls, kpid[nk], &info) != 0)
                    printf("  WSADuplicateSocket for %lu failed, WSA error %d\n", kpid[nk], WSAGetLastError());
                else
                    WriteFile(wr, &info, sizeof info, &n, NULL);
                CloseHandle(wr);
                nk++;
            }
        }
    }
    Sleep(1500);                    /* let the children get ready */

    double t0 = now_ms();
    HANDLE th[CLIENTS];
    for (int i = 0; i < CLIENTS; i++) th[i] = CreateThread(NULL, 0, client_thread, (LPVOID)(INT_PTR)i, 0, NULL);
    WaitForMultipleObjects(CLIENTS, th, TRUE, 30000);
    double t1 = now_ms();

    int answered = 0, distinct = 0; DWORD retries = 0;
    DWORD seen[CLIENTS]; int ns = 0;
    for (int i = 0; i < CLIENTS; i++) {
        retries += g_retries[i];
        if (g_pids[i] == 0) { printf("  client %d: no answer (error %d)\n", i, g_errors[i]); continue; }
        answered++;
        int k; for (k = 0; k < ns && seen[k] != g_pids[i]; k++) ;
        if (k == ns) seen[ns++] = g_pids[i];
    }
    distinct = ns;
    printf("  served by:"); for (int i = 0; i < CLIENTS; i++) printf(" %lu", g_pids[i]); printf("\n");

    /* Every child busy: a client allowed 50 ms must be told it timed out. */
    HANDLE busy[CLIENTS]; int nb = nk < CLIENTS ? nk : CLIENTS;
    for (int i = 0; i < nb; i++) busy[i] = CreateThread(NULL, 0, client_thread, (LPVOID)(INT_PTR)i, 0, NULL);
    Sleep(50);
    DWORD r; int e;
    double q0 = now_ms();
    long late = client(50, &r, &e);
    double q1 = now_ms();
    WaitForMultipleObjects(nb, busy, TRUE, 30000);
    const char *qres = late == 0 ? "timed out" : late > 0 ? "served (no timeout)" : "error";
    printf("  queue timeout: a 50 ms client with %d children busy: %s after %.0f ms (error %d)\n",
           nb, qres, q1 - q0, e);

    for (int i = 0; i < nk; i++) { TerminateProcess(kids[i], 0); CloseHandle(kids[i]); }
    if (ls != INVALID_SOCKET) { closesocket(ls); DeleteFileA(g_path); }

    int ok = answered == CLIENTS && distinct == nk && nk == nchildren;
    printf("RESULT %s %s: %d/%d answered, %d of %d children served, %.0f ms (ideal %d), "
           "%lu busy retries; queue timeout: %s\n",
           mode, ok ? "ok" : "FAIL", answered, CLIENTS, distinct, nchildren, t1 - t0,
           ((CLIENTS + nchildren - 1) / nchildren) * WORK_MS, retries, qres);
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------- permissions */

/* afunix-serve PATH: bind PATH and answer "ok" to every connection. */
static int afunix_serve(const char *path)
{
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    DeleteFileA(path);
    SOCKET ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX; strcpy(sa.sun_path, path);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 64) != 0) {
        printf("serve: bind/listen %s: WSA error %d\n", path, WSAGetLastError());
        return 1;
    }
    printf("serve: listening on %s\n", path);
    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) return 1;
        send(c, "ok", 2, 0);
        closesocket(c);
    }
}

/* afunix-connect PATH: connect, read the reply. Exit 0 if served. */
static int afunix_connect(const char *path)
{
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    char user[256]; DWORD ul = sizeof user;
    if (!GetUserNameA(user, &ul)) strcpy(user, "?");
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX; strcpy(sa.sun_path, path);
    DWORD tmo = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        printf("connect as %s: refused, WSA error %d\n", user, WSAGetLastError());
        return 1;
    }
    char b[8]; int n = recv(s, b, sizeof b - 1, 0);
    closesocket(s);
    if (n <= 0) { printf("connect as %s: connected, no reply (WSA error %d)\n", user, WSAGetLastError()); return 1; }
    b[n] = 0;
    printf("connect as %s: served (%s)\n", user, b);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 3 && !strcmp(argv[1], "afunix-serve"))   return afunix_serve(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "afunix-connect")) return afunix_connect(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "child-afunix-inherit")) return child_afunix_inherit(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "child-afunix-dup"))     return child_afunix_dup();
    if (argc >= 3 && !strcmp(argv[1], "child-pipe"))           return child_pipe(argv[2]);
    if (argc >= 2) return parent(argv[1], argc >= 3 ? atoi(argv[2]) : 3);
    fprintf(stderr, "usage: transport (afunix-inherit|afunix-dup|pipe) [N]\n");
    return 2;
}
