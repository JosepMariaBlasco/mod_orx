/*
 * mod_orx -- run ooRexx programs from Apache, with the Rexx code in
 * separate processes (oorexx-host) and only the hooks inside Apache.
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * What there is so far:
 *
 *   - The content handler "oorexx-cgi", which runs a Rexx CGI program with
 *     the semantics of mod_cgi (same checks, same environment, same header
 *     parsing, same Location handling), but in an oorexx-host process
 *     instead of a fork+exec per request.
 *   - Rexx programs in three more phases, through Apache's own mechanisms:
 *     an authentication provider ("AuthBasicProvider oorexx", with the
 *     program in OoRexxAuthnProgram), an authorization provider ("Require
 *     oorexx program [words]") and a logging hook (OoRexxLogHook). Such a
 *     program gets the CGI environment and its arguments, and its return
 *     value is the verdict:
 *
 *       phase  arguments                  1        0        no value
 *       authn  user, password             granted  denied   user not found
 *       authz  user ("" if none), words   granted  denied   neutral
 *       log    status, bytes, microsecs   (the value is ignored)
 *
 *     Any other value, or an error in the program, is a general error (500).
 *   - The host's life cycle, in the manner of mod_cgid's daemon: the module
 *     binds the socket (as root, in Apache's run-time directory, owned by
 *     Apache's User), starts oorexx-host on it in post_config, restarts it
 *     if it dies, and drains it on every restart and stop of Apache, so that
 *     requests in flight finish while the new generation already serves.
 *
 *   LoadModule orx_module modules/mod_orx.so
 *   OoRexxHost /usr/local/bin/oorexx-host     # or "none": started by hand
 *   OoRexxProcesses 2 8                       # min max
 *   OoRexxMaxRequests 1000                    # phases per host process
 *   OoRexxTimeout 60
 *   OoRexxPreload /path/HTTP.Request.cls ...  # loaded once per host process
 *   <Directory ...>  Options +ExecCGI  AddHandler oorexx-cgi .rex  </Directory>
 *   <Location ...>   AuthType Basic  AuthName x  AuthBasicProvider oorexx
 *                    OoRexxAuthnProgram /path/authn.rex
 *                    Require oorexx /path/authz.rex admins     </Location>
 *   OoRexxLogHook /path/log.rex
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"
#include "unixd.h"
#include "mod_unixd.h"
#include "apr_strings.h"
#include "apr_thread_proc.h"
#include "ap_provider.h"
#include "mod_auth.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "../include/oorexx_protocol.h"
#include "../build/config.h"   /* OORX_DEFAULT_HOST, written by make */

module AP_MODULE_DECLARE_DATA orx_module;

#define DEFAULT_TIMEOUT 60
#define DEFAULT_SOCKET        "oorexx.sock"   /* in DefaultRuntimeDir */
#define DEFAULT_MIN_PROCESSES 2
#define DEFAULT_MAX_PROCESSES 8
#define DEFAULT_MAX_REQUESTS  1000
#define HOST_EXIT_CONFIG      2     /* oorexx-host: "my configuration is wrong" */
#define CONNECT_FIRST_DELAY   apr_time_from_msec(10)
#define CONNECT_MAX_DELAY     apr_time_from_msec(500)
#define CONNECT_BUDGET        apr_time_from_sec(5)

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/*                                                                          */
/* The pool directives are global (one pool per Apache), and so they live  */
/* in plain statics, reset in pre_config on every (re)read of the config.  */
/* ------------------------------------------------------------------------ */

static const char *g_host;          /* OoRexxHost; NULL = the default     */
static int  g_host_none;            /* OoRexxHost none: started by hand    */
static int  g_min_procs, g_max_procs, g_max_requests;
static int  g_max_timeout;          /* the largest OoRexxTimeout anywhere  */
static const char *g_socket_cfg;    /* OoRexxSocket; NULL = the default    */
static apr_array_header_t *g_preload; /* OoRexxPreload: absolute paths      */

typedef struct {
    int         timeout;        /* -1 = unset                            */
    const char *authn_program;  /* OoRexxAuthnProgram; NULL = unset      */
    const char *log_hook;       /* OoRexxLogHook; NULL = unset, "" = off */
} oorx_dir_cfg;

static void *create_dir_cfg(apr_pool_t *p, char *dir)
{
    oorx_dir_cfg *c = apr_pcalloc(p, sizeof *c);
    (void)dir;
    c->timeout = -1;
    return c;
}

static void *merge_dir_cfg(apr_pool_t *p, void *basev, void *addv)
{
    oorx_dir_cfg *base = basev, *add = addv, *c = apr_pcalloc(p, sizeof *c);
    c->timeout = add->timeout >= 0 ? add->timeout : base->timeout;
    c->authn_program = add->authn_program ? add->authn_program : base->authn_program;
    c->log_hook = add->log_hook ? add->log_hook : base->log_hook;
    return c;
}

/* A program path in a directive: relative to ServerRoot, like ScriptAlias. */
static const char *set_program(cmd_parms *cmd, void *dcfg, const char *arg)
{
    oorx_dir_cfg *c = dcfg;
    const char *path = ap_server_root_relative(cmd->pool, arg);
    if (!path) return apr_pstrcat(cmd->pool, "Invalid path: ", arg, NULL);
    if (cmd->info == (void *)1) c->authn_program = path;
    else c->log_hook = path;
    return NULL;
}

static const char *set_log_hook(cmd_parms *cmd, void *dcfg, const char *arg)
{
    oorx_dir_cfg *c = dcfg;
    if (!strcasecmp(arg, "none")) { c->log_hook = ""; return NULL; }
    return set_program(cmd, dcfg, arg);
}

static const char *set_socket(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)dummy;
    if (err) return err;
    g_socket_cfg = ap_server_root_relative(cmd->pool, arg);
    return g_socket_cfg ? NULL : "Invalid OoRexxSocket path";
}

static const char *set_timeout(cmd_parms *cmd, void *dcfg, const char *arg)
{
    oorx_dir_cfg *c = dcfg;
    (void)cmd;
    c->timeout = atoi(arg);
    if (c->timeout <= 0) return "OoRexxTimeout must be a positive number of seconds";
    if (c->timeout > g_max_timeout) g_max_timeout = c->timeout;
    return NULL;
}

static int parse_count(const char *arg, int min)
{
    char *end;
    long v = strtol(arg, &end, 10);
    return (*arg && !*end && v >= min && v <= 100000) ? (int)v : -1;
}

static const char *set_host(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)dummy;
    if (err) return err;
    if (!strcasecmp(arg, "none")) { g_host_none = 1; g_host = NULL; return NULL; }
    g_host_none = 0;
    g_host = ap_server_root_relative(cmd->pool, arg);
    return g_host ? NULL : "Invalid OoRexxHost path";
}

static const char *set_processes(cmd_parms *cmd, void *dummy, const char *a1, const char *a2)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)dummy;
    if (err) return err;
    g_min_procs = parse_count(a1, 1);
    g_max_procs = a2 ? parse_count(a2, 1) : g_min_procs;
    if (g_min_procs < 0 || g_max_procs < 0)
        return "OoRexxProcesses takes one or two positive numbers: min [max]";
    if (g_max_procs < g_min_procs)
        return "OoRexxProcesses: max must not be lower than min";
    return NULL;
}

static const char *set_max_requests(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)dummy;
    if (err) return err;
    g_max_requests = parse_count(arg, 0);
    return g_max_requests >= 0 ? NULL :
        "OoRexxMaxRequests must be a number of phases (0 = no limit)";
}

/* OoRexxPreload file...: packages every host process loads once, in an    */
/* instance it keeps alive, so that the programs that ::Requires them find  */
/* them already loaded. May be repeated; the files add up.                  */
static const char *add_preload(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    const char *path;
    (void)dummy;
    if (err) return err;
    path = ap_server_root_relative(cmd->pool, arg);
    if (!path) return apr_pstrcat(cmd->pool, "Invalid OoRexxPreload path: ", arg, NULL);
    /* A missing file is caught here, when the configuration is read, rather */
    /* than by every host process failing to start.                          */
    {
        apr_finfo_t fi;
        if (apr_stat(&fi, path, APR_FINFO_TYPE, cmd->temp_pool) != APR_SUCCESS ||
            fi.filetype != APR_REG)
            return apr_pstrcat(cmd->pool, "OoRexxPreload: not a file: ", path, NULL);
    }
    if (!g_preload) g_preload = apr_array_make(cmd->pool, 4, sizeof(const char *));
    *(const char **)apr_array_push(g_preload) = path;
    return NULL;
}

static const command_rec oorx_cmds[] = {
    AP_INIT_TAKE1("OoRexxSocket", set_socket, NULL, RSRC_CONF,
                  "Path of the Unix socket where oorexx-host listens"),
    AP_INIT_TAKE1("OoRexxTimeout", set_timeout, NULL, RSRC_CONF | ACCESS_CONF,
                  "Seconds a phase may run before the host halts it"),
    AP_INIT_TAKE1("OoRexxHost", set_host, NULL, RSRC_CONF,
                  "The oorexx-host executable, or \"none\" if it is started by hand"),
    AP_INIT_TAKE12("OoRexxProcesses", set_processes, NULL, RSRC_CONF,
                   "Minimum and maximum number of oorexx-host processes"),
    AP_INIT_TAKE1("OoRexxMaxRequests", set_max_requests, NULL, RSRC_CONF,
                  "Phases an oorexx-host process serves before it is replaced (0 = no limit)"),
    AP_INIT_ITERATE("OoRexxPreload", add_preload, NULL, RSRC_CONF,
                    "Rexx packages every oorexx-host process loads once, at start"),
    AP_INIT_TAKE1("OoRexxAuthnProgram", set_program, (void *)1, RSRC_CONF | ACCESS_CONF,
                  "The Rexx program behind \"AuthBasicProvider oorexx\""),
    AP_INIT_TAKE1("OoRexxLogHook", set_log_hook, NULL, RSRC_CONF | ACCESS_CONF,
                  "A Rexx program to run in the logging phase, or \"none\""),
    {NULL}
};

/* ------------------------------------------------------------------------ */
/* The host's life cycle                                                    */
/*                                                                          */
/* One "generation" per reading of the configuration. Its socket is bound   */
/* here, in the parent, as root; the parent keeps the listening descriptor  */
/* for the whole generation, so connections queue on it even while a dead   */
/* host is being replaced, and passes it to the host as descriptor 3.       */
/*                                                                          */
/* When the configuration is reread (any restart) or Apache stops, pconf is */
/* cleared: the generation's cleanup unlinks the socket's path FIRST (so no */
/* new connection can reach this generation) and then SIGTERMs the host,    */
/* which drains -- finishes what it holds and what is queued -- and exits.  */
/* The new generation starts right away on a new socket at the same path,   */
/* and the requests of old workers keep their connections to the old one.   */
/*                                                                          */
/* Everything of a generation lives in pconf and ends with it, because      */
/* Apache unloads and reloads this module on every restart: a callback left */
/* registered past pconf would be called, when the old host died, in code   */
/* that is no longer there (measured: Apache's parent segfaults after the   */
/* module is upgraded). So the child Apache has is the host's KEEPER, which */
/* on SIGTERM hands the signal to the host's master and exits at once; the  */
/* cleanup reaps it there and then, and the master drains as an orphan.     */
/* ------------------------------------------------------------------------ */

typedef struct {
    apr_proc_t  proc;          /* the host's keeper                          */
    int         running;       /* proc is alive and registered               */
    int         retired;       /* the generation is over: do not restart     */
    int         lfd;           /* the listening socket (-1 once closed)      */
    const char *path;          /* its path                                   */
    const char *host;
    char      **argv;
    server_rec *s;
    apr_pool_t *pool;          /* a subpool of pconf: the registrations      */
    pid_t       parent;        /* Apache's parent process                    */
} oorx_gen;

static const char *g_socket_path;   /* what the handler connects to         */
static oorx_gen   *g_gen;           /* the current generation (parent only) */

static void host_maint(int reason, void *data, apr_wait_t status);

static int start_host(oorx_gen *g)
{
    pid_t pid = fork();
    if (pid < 0) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, errno, g->s, "mod_orx: cannot fork oorexx-host");
        return -1;
    }
    if (pid == 0) {
        int fd, sig;
        sigset_t none;
        /* Its own process group: when prefork Apache stops, it signals   */
        /* its whole group, and that must not reach the host (measured).   */
        setpgid(0, 0);
        if (ap_unixd_setup_child()) _exit(HOST_EXIT_CONFIG);
#ifdef __linux__
        /* After the change of user, which clears it. The host drains if  */
        /* Apache's parent dies without telling it.                        */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != g->parent) _exit(0);
#endif
        /* What exec keeps and the host should not: Apache's signal mask   */
        /* and ignored signals, and every descriptor but the socket.       */
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (sig = 1; sig < NSIG; sig++) signal(sig, SIG_DFL);
        if (dup2(g->lfd, 3) < 0) _exit(HOST_EXIT_CONFIG);
        fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); }   /* stderr: the error log */
        {
            long maxfd = sysconf(_SC_OPEN_MAX);
            if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
            for (fd = 4; fd < maxfd; fd++) close(fd);
        }
        execv(g->host, g->argv);
        _exit(127);
    }
    memset(&g->proc, 0, sizeof g->proc);
    g->proc.pid = pid;
    g->running = 1;
    apr_proc_other_child_register(&g->proc, host_maint, g, NULL, g->pool);
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, g->s,
                 "mod_orx: oorexx-host started (pid %ld) on %s", (long)pid, g->path);
    return 0;
}

/* APR hands the maintenance callback the child's exit CODE (or the number  */
/* of the signal that killed it), not a raw wait() status: mod_cgid, too,   */
/* compares it directly.                                                    */
static void host_maint(int reason, void *data, apr_wait_t status)
{
    oorx_gen *g = data;
    switch (reason) {
    case APR_OC_REASON_DEATH:
    case APR_OC_REASON_LOST:
        apr_proc_other_child_unregister(g);
        g->running = 0;
        if (g->retired) break;              /* a drained host: all is well */
        if (reason == APR_OC_REASON_DEATH &&
            (status == HOST_EXIT_CONFIG || status == 127)) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, 0, g->s,
                         "mod_orx: oorexx-host (%s) could not start (status %d); "
                         "not restarting it", g->host, (int)status);
            break;
        }
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, g->s,
                     "mod_orx: oorexx-host (pid %ld) %s (status %d); restarting it",
                     (long)g->proc.pid,
                     reason == APR_OC_REASON_LOST ? "was lost" : "ended", (int)status);
        start_host(g);
        break;
    default:            /* RESTART, RUNNING, UNWRITABLE, UNREGISTER: nothing */
        break;
    }
}

static apr_status_t end_generation(void *data)
{
    oorx_gen *g = data;
    if (getpid() != g->parent) return APR_SUCCESS;   /* never in a worker */
    g->retired = 1;
    if (g->lfd >= 0) {
        unlink(g->path);              /* first: nothing new reaches it     */
        close(g->lfd);
        g->lfd = -1;
    }
    if (g->running) {
        /* The keeper exits as soon as it has passed SIGTERM on (a few    */
        /* milliseconds, measured); a second of patience, then SIGKILL -- */
        /* the master drains anyway when its keeper dies.                  */
        /* The keeper's registration is gone already: it lives in a       */
        /* subpool, and APR destroys the subpools of pconf before it runs */
        /* pconf's own cleanups. (Unregistering it here again would crash */
        /* inside APR: measured.)                                          */
        int i, st;
        kill(g->proc.pid, SIGTERM);
        for (i = 0; i < 100; i++) {
            pid_t r = waitpid(g->proc.pid, &st, WNOHANG);
            if (r == g->proc.pid || (r < 0 && errno == ECHILD)) break;
            apr_sleep(apr_time_from_msec(10));
        }
        if (i == 100) { kill(g->proc.pid, SIGKILL); waitpid(g->proc.pid, &st, 0); }
        g->running = 0;
    }
    if (g_gen == g) g_gen = NULL;
    return APR_SUCCESS;
}

static int bind_socket(server_rec *s, const char *path)
{
    struct sockaddr_un sa;
    int fd;
    if (strlen(path) >= sizeof sa.sun_path) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, "mod_orx: socket path too long: %s", path);
        return -1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, errno, s, "mod_orx: cannot create a socket");
        return -1;
    }
    /* Workers fork without exec and close it in child_init; anything     */
    /* else Apache starts with exec must not inherit it.                   */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, errno, s, "mod_orx: cannot bind %s", path);
        close(fd);
        return -1;
    }
    /* Only Apache's User may connect, as with mod_cgid's ScriptSock.      */
    if (chmod(path, 0600) < 0 ||
        (geteuid() == 0 && chown(path, ap_unixd_config.user_id, ap_unixd_config.group_id) < 0) ||
        listen(fd, 1024) < 0) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, errno, s, "mod_orx: cannot set up %s", path);
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static int oorx_pre_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp)
{
    (void)pconf; (void)plog; (void)ptemp;
    g_host = NULL; g_host_none = 0;
    g_min_procs = DEFAULT_MIN_PROCESSES;
    g_max_procs = DEFAULT_MAX_PROCESSES;
    g_max_requests = DEFAULT_MAX_REQUESTS;
    g_max_timeout = DEFAULT_TIMEOUT;
    g_socket_cfg = NULL;
    g_socket_path = NULL;
    g_preload = NULL;
    return OK;
}

static int oorx_post_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp,
                            server_rec *s)
{
    apr_pool_t *pp = pconf;
    oorx_gen *g;
    char **argv;
    (void)plog; (void)ptemp;

    g_socket_path = g_socket_cfg;
    if (g_host_none) return OK;                /* the host is not ours    */
    /* Like mod_cgid: nothing to start in the first, configuration-only,   */
    /* pass of startup, nor when Apache only checks or dumps its config.  */
    if (ap_state_query(AP_SQ_MAIN_STATE) == AP_SQ_MS_CREATE_PRE_CONFIG ||
        ap_state_query(AP_SQ_RUN_MODE) != AP_SQ_RM_NORMAL)
        return OK;

    if (!g_socket_path) g_socket_path = ap_runtime_dir_relative(pconf, DEFAULT_SOCKET);

    g = apr_pcalloc(pp, sizeof *g);
    g->s = s;
    if (apr_pool_create(&g->pool, pconf) != APR_SUCCESS) return HTTP_INTERNAL_SERVER_ERROR;
    g->parent = getpid();
    g->path = apr_pstrdup(pp, g_socket_path);
    g->host = apr_pstrdup(pp, g_host ? g_host : OORX_DEFAULT_HOST);
    if (access(g->host, X_OK) < 0) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, errno, s,
                     "mod_orx: OoRexxHost %s is not an executable file", g->host);
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    if ((g->lfd = bind_socket(s, g->path)) < 0) return HTTP_INTERNAL_SERVER_ERROR;

    {
        int i, n = g_preload ? g_preload->nelts : 0, a = 11;
        argv = apr_pcalloc(pp, (12 + 2 * n) * sizeof *argv);
        argv[0]  = (char *)g->host;
        argv[1]  = "--listen-fd";     argv[2]  = "3";
        argv[3]  = "--processes";     argv[4]  = apr_itoa(pp, g_min_procs);
        argv[5]  = "--max-processes"; argv[6]  = apr_itoa(pp, g_max_procs);
        argv[7]  = "--max-requests";  argv[8]  = apr_itoa(pp, g_max_requests);
        /* A drained host gets as long as the longest phase may last, and a */
        /* margin, before what is left is killed.                            */
        argv[9]  = "--grace";         argv[10] = apr_itoa(pp, g_max_timeout + 10);
        for (i = 0; i < n; i++) {
            argv[a++] = "--preload";
            argv[a++] = apr_pstrdup(pp, APR_ARRAY_IDX(g_preload, i, const char *));
        }
        argv[a] = NULL;
    }
    g->argv = argv;

    apr_pool_cleanup_register(pconf, g, end_generation, apr_pool_cleanup_null);
    g_gen = g;
    return start_host(g) == 0 ? OK : HTTP_INTERNAL_SERVER_ERROR;
}

static void oorx_child_init(apr_pool_t *p, server_rec *s)
{
    (void)p; (void)s;
    /* A worker only connects; the listening socket is the parent's.       */
    if (g_gen && g_gen->lfd >= 0) close(g_gen->lfd);
}

/* ------------------------------------------------------------------------ */
/* The connection to the host, and the frame pump                           */
/* ------------------------------------------------------------------------ */

typedef struct {
    request_rec *r;
    int          fd;
    char        *buf;          /* DATA received and not yet consumed */
    apr_size_t   len, pos, cap;
    int          flush;        /* the last DATA frame asked for a flush */
    int          done;         /* RESULT received */
    int          failed;       /* the connection broke */
    int          has_body;     /* ap_should_client_block said yes */
    uint32_t     status;       /* from RESULT */
    const char  *value;        /* from RESULT: what the program returned, */
                               /* or NULL if it returned nothing           */
} oorx_conn;

static int connect_host(request_rec *r, const char *path, int timeout)
{
    struct sockaddr_un sa;
    struct timeval tv;
    int fd;

    apr_interval_time_t delay = CONNECT_FIRST_DELAY, waited = 0;

    if (strlen(path) >= sizeof sa.sun_path) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    /* While Apache rereads its configuration, the old generation's socket */
    /* is gone and the new one is not there yet (ENOENT, ECONNREFUSED);    */
    /* mod_cgid retries in the same way. A full queue is EAGAIN.           */
    for (;;) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) break;
        if ((errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN) ||
            waited >= CONNECT_BUDGET) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, errno, r,
                          "mod_orx: cannot connect to oorexx-host at %s", path);
            close(fd);
            return -1;
        }
        close(fd);
        apr_sleep(delay);
        waited += delay;
        if (delay < CONNECT_MAX_DELAY) delay *= 2;
    }
    /* The host halts the program at the deadline; this margin only guards
     * against a host that does not answer at all. */
    tv.tv_sec = timeout + 10; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

static void serve_read(oorx_conn *c, uint32_t want)
{
    long n = 0;
    char *b;
    if (want > OORX_CHUNK * 4) want = OORX_CHUNK * 4;
    b = apr_palloc(c->r->pool, want ? want : 1);
    if (c->has_body && want) {
        n = ap_get_client_block(c->r, b, want);
        if (n < 0) n = 0;           /* a broken client body reads as EOF */
    }
    if (oorx_send(c->fd, OORX_BODY, 0, b, (uint32_t)n) < 0) c->failed = 1;
}

/* Process frames until one DATA frame arrives (returns 1) or the phase
 * ends or fails (returns 0). READ and LOG are serviced on the way. */
static int pump(oorx_conn *c)
{
    while (!c->done && !c->failed) {
        int type, flags;
        uint32_t len;
        if (oorx_recv_header(c->fd, &type, &flags, &len) < 0) { c->failed = 1; break; }
        switch (type) {
        case OORX_DATA:
            if (c->pos > 0 && c->pos == c->len) c->pos = c->len = 0;
            if (c->len + len > c->cap) {
                apr_size_t cap = (c->len + len) * 2 + 1;
                char *nb = apr_palloc(c->r->pool, cap);
                memcpy(nb, c->buf + c->pos, c->len - c->pos);
                c->len -= c->pos; c->pos = 0;
                c->buf = nb; c->cap = cap;
            }
            if (len && oorx_read_all(c->fd, c->buf + c->len, len) < 0) { c->failed = 1; break; }
            c->len += len;
            c->flush = flags & OORX_FLAG_FLUSH;
            return 1;
        case OORX_READ: {
            unsigned char n[4];
            if (len != 4 || oorx_read_all(c->fd, n, 4) < 0) { c->failed = 1; break; }
            serve_read(c, oorx_get_u32(n));
            break;
        }
        case OORX_LOG: {
            char *m = apr_palloc(c->r->pool, len + 1);
            if (len && oorx_read_all(c->fd, m, len) < 0) { c->failed = 1; break; }
            m[len] = '\0';
            ap_log_rerror(APLOG_MARK, len && m[0] == OORX_LOG_INFO ? APLOG_INFO :
                          len && m[0] == OORX_LOG_WARNING ? APLOG_WARNING : APLOG_ERR,
                          0, c->r, "%s", len ? m + 1 : "");
            break;
        }
        case OORX_RESULT: {
            char *m = apr_palloc(c->r->pool, len + 1);
            if (len < 4 || oorx_read_all(c->fd, m, len) < 0) { c->failed = 1; break; }
            m[len] = '\0';
            c->status = oorx_get_u32((unsigned char *)m);
            c->value = flags & OORX_FLAG_VALUE ? m + 4 : NULL;
            c->done = 1;
            break;
        }
        default:
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, c->r,
                          "mod_orx: unexpected frame type %d from oorexx-host", type);
            c->failed = 1;
        }
    }
    return 0;
}

/* getsfunc for ap_scan_script_header_err_core_ex: one line of output. */
static int getsfunc_host(char *w, int len, void *data)
{
    oorx_conn *c = data;
    int n = 0;
    while (n < len - 1) {
        if (c->pos == c->len && !pump(c)) break;
        w[n] = c->buf[c->pos++];
        if (w[n++] == '\n') break;
    }
    w[n] = '\0';
    return n > 0;
}

/* Drain whatever the program still sends (after a redirect, or an error). */
static void discard_output(oorx_conn *c)
{
    c->pos = c->len = 0;
    while (pump(c)) c->pos = c->len = 0;
}

static apr_status_t close_fd(void *data)
{
    close((int)(apr_uintptr_t)data);
    return APR_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* The oorexx-cgi handler                                                   */
/* ------------------------------------------------------------------------ */

/* Tables on the wire: strings as (u32 length, bytes), one after another. */
static void append_str(apr_pool_t *p, char **buf, apr_size_t *len, apr_size_t *cap,
                       const char *v, apr_size_t vl)
{
    apr_size_t need = *len + 4 + vl;
    if (need > *cap) {
        apr_size_t cap2 = need * 2;
        char *nb = apr_palloc(p, cap2);
        if (*len) memcpy(nb, *buf, *len);
        *buf = nb; *cap = cap2;
    }
    oorx_put_u32((unsigned char *)*buf + *len, (uint32_t)vl); *len += 4;
    memcpy(*buf + *len, v, vl); *len += vl;
}

static void append_kv(apr_pool_t *p, char **buf, apr_size_t *len, apr_size_t *cap,
                      const char *k, apr_size_t kl, const char *v, apr_size_t vl)
{
    append_str(p, buf, len, cap, k, kl);
    append_str(p, buf, len, cap, v, vl);
}

static int oorx_cgi_handler(request_rec *r)
{
    oorx_dir_cfg *dcfg;
    oorx_conn *c;
    char **env, *vars = NULL, *begin;
    apr_size_t vlen = 0, vcap = 0, plen;
    int timeout, ret, i;
    char sbuf[MAX_STRING_LEN];
    const char *location;

    if (strcmp(r->handler, "oorexx-cgi")) return DECLINED;

    dcfg = ap_get_module_config(r->per_dir_config, &orx_module);
    timeout = dcfg->timeout > 0 ? dcfg->timeout : DEFAULT_TIMEOUT;

    /* The same preconditions as mod_cgi, with the same answers. */
    if (!(ap_allow_options(r) & OPT_EXECCGI)) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_orx: Options ExecCGI is off in this directory: %s", r->filename);
        return HTTP_FORBIDDEN;
    }
    if (r->used_path_info == AP_REQ_REJECT_PATH_INFO && r->path_info && *r->path_info) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_orx: AcceptPathInfo off disallows user's path: %s", r->path_info);
        return HTTP_NOT_FOUND;
    }
    if (r->finfo.filetype == APR_NOFILE) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_orx: script not found or unable to stat: %s", r->filename);
        return HTTP_NOT_FOUND;
    }
    if (r->finfo.filetype == APR_DIR) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_orx: attempt to invoke directory as script: %s", r->filename);
        return HTTP_FORBIDDEN;
    }
    if (!g_socket_path) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_orx: OoRexxHost is none and OoRexxSocket is not set");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* The CGI environment, exactly as mod_cgi builds it. */
    ap_add_common_vars(r);
    ap_add_cgi_vars(r);
    env = ap_create_environment(r->pool, r->subprocess_env);
    for (i = 0; env[i]; i++) {
        const char *eq = strchr(env[i], '=');
        if (!eq) continue;
        append_kv(r->pool, &vars, &vlen, &vcap, env[i], eq - env[i], eq + 1, strlen(eq + 1));
    }

    if ((ret = ap_setup_client_block(r, REQUEST_CHUNKED_DECHUNK)) != OK) return ret;

    c = apr_pcalloc(r->pool, sizeof *c);
    c->r = r;
    c->fd = connect_host(r, g_socket_path, timeout);
    if (c->fd < 0) return HTTP_SERVICE_UNAVAILABLE;
    apr_pool_cleanup_register(r->pool, (void *)(apr_uintptr_t)c->fd, close_fd,
                              apr_pool_cleanup_null);
    c->has_body = ap_should_client_block(r);

    plen = strlen(r->filename);
    begin = apr_palloc(r->pool, 7 + plen);
    begin[0] = OORX_PROTOCOL_VERSION;
    begin[1] = OORX_PHASE_CONTENT;
    begin[2] = OORX_CLASS_CGI;
    oorx_put_u32((unsigned char *)begin + 3, (uint32_t)timeout);
    memcpy(begin + 7, r->filename, plen);
    if (oorx_send(c->fd, OORX_BEGIN, 0, begin, (uint32_t)(7 + plen)) < 0 ||
        oorx_send(c->fd, OORX_VARS, 0, vars, (uint32_t)vlen) < 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, errno, r, "mod_orx: cannot talk to oorexx-host");
        return HTTP_SERVICE_UNAVAILABLE;
    }

    /* Headers: Apache's own CGI header parser, as mod_cgi uses. */
    ret = ap_scan_script_header_err_core_ex(r, sbuf, getsfunc_host, c, APLOG_MODULE_INDEX);
    if (ret != OK) {
        discard_output(c);
        if (ret == HTTP_NOT_MODIFIED) { r->status = ret; return OK; }
        return ret;
    }

    location = apr_table_get(r->headers_out, "Location");
    if (location && r->status == 200) {
        discard_output(c);
        if (location[0] == '/') {
            /* Internal redirect, exactly as mod_cgi does it. */
            r->method = "GET";
            r->method_number = M_GET;
            apr_table_unset(r->headers_in, "Content-Length");
            ap_internal_redirect_handler(location, r);
            return OK;
        }
        return HTTP_MOVED_TEMPORARILY;
    }

    /* Body: what was buffered past the headers, then the rest as it comes. */
    for (;;) {
        if (c->pos < c->len) {
            ap_rwrite(c->buf + c->pos, (int)(c->len - c->pos), r);
            c->pos = c->len;
        }
        if (c->flush) { ap_rflush(r); c->flush = 0; }
        if (!pump(c)) break;
    }
    if (c->failed && !c->done)
        ap_log_rerror(APLOG_MARK, APLOG_ERR, errno, r,
                      "mod_orx: connection to oorexx-host lost before the end of %s",
                      r->filename);
    return OK;
}

/* ------------------------------------------------------------------------ */
/* Hooks: authentication, authorization and logging                         */
/*                                                                          */
/* A hook program gets the CGI environment (built on a copy of             */
/* subprocess_env, so that running it changes nothing in the request) and   */
/* its arguments, and returns a value: its verdict. See the table at the    */
/* top of this file.                                                        */
/* ------------------------------------------------------------------------ */

/* Run one hook phase. Returns the status in RESULT (0 = the program ran),   */
/* or -1 if the host could not be reached or the connection broke; *value   */
/* is what the program returned, or NULL if it returned nothing.            */
static int run_hook(request_rec *r, int phase, const char *program,
                    const char **args, int nargs, const char **value)
{
    oorx_dir_cfg *dcfg = ap_get_module_config(r->per_dir_config, &orx_module);
    int timeout = dcfg->timeout > 0 ? dcfg->timeout : DEFAULT_TIMEOUT;
    apr_table_t *saved = r->subprocess_env;
    char **env, *vars = NULL, *abuf = NULL, *begin;
    apr_size_t vlen = 0, vcap = 0, alen = 0, acap = 0, plen;
    oorx_conn *c;
    int i;

    *value = NULL;
    if (!g_socket_path) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_orx: OoRexxHost is none and OoRexxSocket is not set");
        return -1;
    }

    r->subprocess_env = apr_table_copy(r->pool, saved);
    ap_add_common_vars(r);
    if (r->uri && r->filename) ap_add_cgi_vars(r);
    env = ap_create_environment(r->pool, r->subprocess_env);
    r->subprocess_env = saved;
    for (i = 0; env[i]; i++) {
        const char *eq = strchr(env[i], '=');
        if (!eq) continue;
        append_kv(r->pool, &vars, &vlen, &vcap, env[i], eq - env[i], eq + 1, strlen(eq + 1));
    }
    for (i = 0; i < nargs; i++)
        append_str(r->pool, &abuf, &alen, &acap, args[i], strlen(args[i]));

    c = apr_pcalloc(r->pool, sizeof *c);
    c->r = r;
    c->fd = connect_host(r, g_socket_path, timeout);
    if (c->fd < 0) return -1;
    apr_pool_cleanup_register(r->pool, (void *)(apr_uintptr_t)c->fd, close_fd,
                              apr_pool_cleanup_null);

    plen = strlen(program);
    begin = apr_palloc(r->pool, 7 + plen);
    begin[0] = OORX_PROTOCOL_VERSION;
    begin[1] = (char)phase;
    begin[2] = OORX_CLASS_NATIVE;
    oorx_put_u32((unsigned char *)begin + 3, (uint32_t)timeout);
    memcpy(begin + 7, program, plen);
    if (oorx_send(c->fd, OORX_BEGIN, 0, begin, (uint32_t)(7 + plen)) < 0 ||
        oorx_send(c->fd, OORX_ARGS, 0, abuf, (uint32_t)alen) < 0 ||
        oorx_send(c->fd, OORX_VARS, 0, vars, (uint32_t)vlen) < 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, errno, r, "mod_orx: cannot talk to oorexx-host");
        return -1;
    }
    discard_output(c);                   /* a hook's output went to the log */
    if (!c->done) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, errno, r,
                      "mod_orx: connection to oorexx-host lost while running %s", program);
        return -1;
    }
    *value = c->value;
    return (int)c->status;
}

/* "1", "0" or nothing; anything else is reported and counts as an error.   */
enum { VERDICT_ERROR, VERDICT_YES, VERDICT_NO, VERDICT_NONE };

static int verdict(request_rec *r, const char *program, int status, const char *value)
{
    if (status != 0) return VERDICT_ERROR;   /* the host has logged why */
    if (!value) return VERDICT_NONE;
    if (!strcmp(value, "1")) return VERDICT_YES;
    if (!strcmp(value, "0")) return VERDICT_NO;
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                  "mod_orx: %s returned \"%s\"; expected 1, 0 or no value",
                  program, value);
    return VERDICT_ERROR;
}

/* AuthBasicProvider oorexx: the program in OoRexxAuthnProgram.             */
static authn_status oorx_check_password(request_rec *r, const char *user,
                                        const char *password)
{
    oorx_dir_cfg *dcfg = ap_get_module_config(r->per_dir_config, &orx_module);
    const char *args[2], *value;
    int status;
    if (!dcfg->authn_program) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_orx: AuthBasicProvider oorexx, but no OoRexxAuthnProgram");
        return AUTH_GENERAL_ERROR;
    }
    args[0] = user; args[1] = password;
    status = run_hook(r, OORX_PHASE_AUTHN, dcfg->authn_program, args, 2, &value);
    switch (verdict(r, dcfg->authn_program, status, value)) {
    case VERDICT_YES:  return AUTH_GRANTED;
    case VERDICT_NO:   return AUTH_DENIED;
    case VERDICT_NONE: return AUTH_USER_NOT_FOUND;
    default:           return AUTH_GENERAL_ERROR;
    }
}

static const authn_provider oorx_authn_provider = { oorx_check_password, NULL };

/* Require oorexx program [words...]                                        */
typedef struct { const char *program, *words; } oorx_require;

static const char *oorx_parse_require(cmd_parms *cmd, const char *line,
                                      const void **parsed)
{
    oorx_require *q = apr_pcalloc(cmd->pool, sizeof *q);
    const char *t = line;
    char *prog = ap_getword_conf(cmd->pool, &t);
    if (!*prog) return "Require oorexx: the program is missing";
    q->program = ap_server_root_relative(cmd->pool, prog);
    if (!q->program) return apr_pstrcat(cmd->pool, "Require oorexx: invalid path: ", prog, NULL);
    while (*t == ' ' || *t == '\t') t++;
    q->words = apr_pstrdup(cmd->pool, t);
    *parsed = q;
    return NULL;
}

/* The program runs with or without a user. Apache asks first without one;  */
/* where authentication is configured, a "no" then means "no, not without   */
/* knowing who you are", which makes Apache authenticate and ask again      */
/* (AUTHZ_DENIED_NO_USER). Where it is not, a "no" is a plain no: there is  */
/* nobody to ask (measured: Apache answers 500 otherwise, AH00027).         */
static authz_status oorx_check_authz(request_rec *r, const char *line,
                                     const void *parsed)
{
    const oorx_require *q = parsed;
    const char *args[2], *value;
    int status;
    (void)line;
    args[0] = r->user ? r->user : "";
    args[1] = q->words;
    status = run_hook(r, OORX_PHASE_AUTHZ, q->program, args, 2, &value);
    switch (verdict(r, q->program, status, value)) {
    case VERDICT_YES:  return AUTHZ_GRANTED;
    case VERDICT_NO:   return r->user || !ap_auth_type(r) ? AUTHZ_DENIED
                                                          : AUTHZ_DENIED_NO_USER;
    case VERDICT_NONE: return AUTHZ_NEUTRAL;
    default:           return AUTHZ_GENERAL_ERROR;
    }
}

static const authz_provider oorx_authz_provider = { oorx_check_authz, oorx_parse_require };

/* OoRexxLogHook: after the response, with what was finally sent.           */
static int oorx_log_transaction(request_rec *r)
{
    oorx_dir_cfg *dcfg = ap_get_module_config(r->per_dir_config, &orx_module);
    request_rec *last = r;
    const char *args[3], *value;
    if (!dcfg->log_hook || !*dcfg->log_hook) return DECLINED;
    while (last->next) last = last->next;          /* after internal redirects */
    args[0] = apr_itoa(r->pool, last->status);
    args[1] = apr_off_t_toa(r->pool, last->bytes_sent);
    args[2] = apr_psprintf(r->pool, "%" APR_TIME_T_FMT, apr_time_now() - r->request_time);
    run_hook(r, OORX_PHASE_LOG, dcfg->log_hook, args, 3, &value);
    return DECLINED;                               /* other loggers run too */
}

static void register_hooks(apr_pool_t *p)
{
    ap_hook_pre_config(oorx_pre_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(oorx_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(oorx_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(oorx_cgi_handler, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_log_transaction(oorx_log_transaction, NULL, NULL, APR_HOOK_MIDDLE);
    ap_register_auth_provider(p, AUTHN_PROVIDER_GROUP, "oorexx", AUTHN_PROVIDER_VERSION,
                              &oorx_authn_provider, AP_AUTH_INTERNAL_PER_CONF);
    ap_register_auth_provider(p, AUTHZ_PROVIDER_GROUP, "oorexx", AUTHZ_PROVIDER_VERSION,
                              &oorx_authz_provider, AP_AUTH_INTERNAL_PER_CONF);
}

AP_DECLARE_MODULE(orx) = {
    STANDARD20_MODULE_STUFF,
    create_dir_cfg,
    merge_dir_cfg,
    NULL,
    NULL,
    oorx_cmds,
    register_hooks
};
