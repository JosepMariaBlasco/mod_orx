/* Probe: from inside Apache for Windows (mpm_winnt), the parent creates an
 * AF_UNIX listening socket in post_config and starts three processes that
 * inherit it (transport.exe child-afunix-inherit), as mod_orx starts its
 * host; the handler, in Apache's child process, connects and returns the
 * PID of the process that served it.
 *
 *   ORX_PROBE_HOST=C:\...\transport.exe   in the environment of httpd.exe
 *   <Location /probe> SetHandler orxprobe </Location>
 *
 * Copyright 2026 Josep Maria Blasco -- Apache License 2.0
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "ap_mpm.h"

module AP_MODULE_DECLARE_DATA orxprobe_module;

static const char *g_path;

static int probe_post_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    (void)plog; (void)ptemp;
    g_path = ap_server_root_relative(pconf, "logs/orxprobe.sock");
    /* mpm_winnt: the child process reads the configuration too, and has  */
    /* AP_PARENT_PID in its environment. Only the parent starts anything. */
    const char *parent = getenv("AP_PARENT_PID");
    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s,
                 "orxprobe: post_config in pid %lu (%s), state %d",
                 GetCurrentProcessId(), parent ? "Apache's child" : "Apache's parent",
                 ap_state_query(AP_SQ_MAIN_STATE));
    if (parent || ap_state_query(AP_SQ_MAIN_STATE) == AP_SQ_MS_CREATE_PRE_CONFIG) return OK;

    const char *host = getenv("ORX_PROBE_HOST");
    if (!host) { ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "orxprobe: no ORX_PROBE_HOST"); return OK; }
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    DeleteFileA(g_path);
    SOCKET ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX; strncpy(sa.sun_path, g_path, sizeof sa.sun_path - 1);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 64) != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "orxprobe: bind/listen %s: WSA error %d",
                     g_path, WSAGetLastError());
        return OK;
    }
    /* Inheritable only while the three processes are created. */
    SetHandleInformation((HANDLE)ls, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    for (int i = 0; i < 3; i++) {
        char cmd[2 * MAX_PATH];
        snprintf(cmd, sizeof cmd, "\"%s\" child-afunix-inherit %llu", host, (unsigned long long)ls);
        STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(host, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "orxprobe: CreateProcess: error %lu", GetLastError());
            continue;
        }
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s, "orxprobe: host process %lu started on %s",
                     pi.dwProcessId, g_path);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    SetHandleInformation((HANDLE)ls, HANDLE_FLAG_INHERIT, 0);
    return OK;
}

static int probe_handler(request_rec *r)
{
    if (strcmp(r->handler, "orxprobe")) return DECLINED;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX; strncpy(sa.sun_path, g_path, sizeof sa.sun_path - 1);
    DWORD tmo = 10000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    ap_set_content_type(r, "text/plain");
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        ap_rprintf(r, "connect failed: WSA error %d (Apache child %lu)\n", WSAGetLastError(), GetCurrentProcessId());
        closesocket(s);
        return OK;
    }
    send(s, "x", 1, 0);
    char b[32]; int n = recv(s, b, sizeof b - 1, 0);
    closesocket(s);
    if (n <= 0) { ap_rprintf(r, "no reply: WSA error %d\n", WSAGetLastError()); return OK; }
    b[n] = 0;
    ap_rprintf(r, "served by %s (Apache child %lu)\n", b, GetCurrentProcessId());
    return OK;
}

static void probe_hooks(apr_pool_t *p)
{
    (void)p;
    ap_hook_post_config(probe_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(probe_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

AP_DECLARE_MODULE(orxprobe) = {
    STANDARD20_MODULE_STUFF, NULL, NULL, NULL, NULL, NULL, probe_hooks
};
