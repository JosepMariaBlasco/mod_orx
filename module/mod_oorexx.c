/*
 * mod_oorexx -- run ooRexx programs from Apache, with the Rexx code in
 * separate processes (oorexx-host) and only the hooks inside Apache.
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * Milestone 0: the content handler "oorexx-cgi", which runs a Rexx CGI
 * program with the semantics of mod_cgi (same checks, same environment,
 * same header parsing, same Location handling), but in an oorexx-host
 * process instead of a fork+exec per request. The host is started outside
 * Apache for now; managing it from here comes next.
 *
 *   LoadModule oorexx_module modules/mod_oorexx.so
 *   OoRexxSocket /run/oorexx/host.sock
 *   OoRexxTimeout 60
 *   <Directory ...>  Options +ExecCGI  AddHandler oorexx-cgi .rex  </Directory>
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"
#include "apr_strings.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#include "../include/oorexx_protocol.h"

module AP_MODULE_DECLARE_DATA oorexx_module;

#define DEFAULT_TIMEOUT 60

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

typedef struct { const char *socket; } oorx_server_cfg;
typedef struct { int timeout; } oorx_dir_cfg;   /* -1 = unset */

static void *create_server_cfg(apr_pool_t *p, server_rec *s)
{
    oorx_server_cfg *c = apr_pcalloc(p, sizeof *c);
    (void)s;
    return c;
}

static void *merge_server_cfg(apr_pool_t *p, void *basev, void *addv)
{
    oorx_server_cfg *base = basev, *add = addv, *c = apr_pcalloc(p, sizeof *c);
    c->socket = add->socket ? add->socket : base->socket;
    return c;
}

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
    return c;
}

static const char *set_socket(cmd_parms *cmd, void *dummy, const char *arg)
{
    oorx_server_cfg *c = ap_get_module_config(cmd->server->module_config,
                                              &oorexx_module);
    (void)dummy;
    c->socket = ap_server_root_relative(cmd->pool, arg);
    return c->socket ? NULL : "Invalid OoRexxSocket path";
}

static const char *set_timeout(cmd_parms *cmd, void *dcfg, const char *arg)
{
    oorx_dir_cfg *c = dcfg;
    (void)cmd;
    c->timeout = atoi(arg);
    return c->timeout > 0 ? NULL : "OoRexxTimeout must be a positive number of seconds";
}

static const command_rec oorx_cmds[] = {
    AP_INIT_TAKE1("OoRexxSocket", set_socket, NULL, RSRC_CONF,
                  "Path of the Unix socket where oorexx-host listens"),
    AP_INIT_TAKE1("OoRexxTimeout", set_timeout, NULL, RSRC_CONF | ACCESS_CONF,
                  "Seconds a phase may run before the host halts it"),
    {NULL}
};

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
} oorx_conn;

static int connect_host(request_rec *r, const char *path, int timeout)
{
    struct sockaddr_un sa;
    struct timeval tv;
    int fd;

    if (strlen(path) >= sizeof sa.sun_path) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, errno, r,
                      "mod_oorexx: cannot connect to oorexx-host at %s", path);
        close(fd);
        return -1;
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
            unsigned char s[4];
            if (len != 4 || oorx_read_all(c->fd, s, 4) < 0) { c->failed = 1; break; }
            c->status = oorx_get_u32(s);
            c->done = 1;
            break;
        }
        default:
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, c->r,
                          "mod_oorexx: unexpected frame type %d from oorexx-host", type);
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

static int append_kv(apr_pool_t *p, char **buf, apr_size_t *len, apr_size_t *cap,
                     const char *k, apr_size_t kl, const char *v, apr_size_t vl)
{
    apr_size_t need = *len + 8 + kl + vl;
    if (need > *cap) {
        apr_size_t cap2 = need * 2;
        char *nb = apr_palloc(p, cap2);
        if (*len) memcpy(nb, *buf, *len);
        *buf = nb; *cap = cap2;
    }
    oorx_put_u32((unsigned char *)*buf + *len, (uint32_t)kl); *len += 4;
    memcpy(*buf + *len, k, kl); *len += kl;
    oorx_put_u32((unsigned char *)*buf + *len, (uint32_t)vl); *len += 4;
    memcpy(*buf + *len, v, vl); *len += vl;
    return 1;
}

static int oorx_cgi_handler(request_rec *r)
{
    oorx_server_cfg *scfg;
    oorx_dir_cfg *dcfg;
    oorx_conn *c;
    char **env, *vars = NULL, *begin;
    apr_size_t vlen = 0, vcap = 0, plen;
    int timeout, ret, i;
    char sbuf[MAX_STRING_LEN];
    const char *location;

    if (strcmp(r->handler, "oorexx-cgi")) return DECLINED;

    scfg = ap_get_module_config(r->server->module_config, &oorexx_module);
    dcfg = ap_get_module_config(r->per_dir_config, &oorexx_module);
    timeout = dcfg->timeout > 0 ? dcfg->timeout : DEFAULT_TIMEOUT;

    /* The same preconditions as mod_cgi, with the same answers. */
    if (!(ap_allow_options(r) & OPT_EXECCGI)) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_oorexx: Options ExecCGI is off in this directory: %s", r->filename);
        return HTTP_FORBIDDEN;
    }
    if (r->used_path_info == AP_REQ_REJECT_PATH_INFO && r->path_info && *r->path_info) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_oorexx: AcceptPathInfo off disallows user's path: %s", r->path_info);
        return HTTP_NOT_FOUND;
    }
    if (r->finfo.filetype == APR_NOFILE) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_oorexx: script not found or unable to stat: %s", r->filename);
        return HTTP_NOT_FOUND;
    }
    if (r->finfo.filetype == APR_DIR) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_oorexx: attempt to invoke directory as script: %s", r->filename);
        return HTTP_FORBIDDEN;
    }
    if (!scfg->socket) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "mod_oorexx: OoRexxSocket is not set");
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
    c->fd = connect_host(r, scfg->socket, timeout);
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
        ap_log_rerror(APLOG_MARK, APLOG_ERR, errno, r, "mod_oorexx: cannot talk to oorexx-host");
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
                      "mod_oorexx: connection to oorexx-host lost before the end of %s",
                      r->filename);
    return OK;
}

static void register_hooks(apr_pool_t *p)
{
    (void)p;
    ap_hook_handler(oorx_cgi_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

AP_DECLARE_MODULE(oorexx) = {
    STANDARD20_MODULE_STUFF,
    create_dir_cfg,
    merge_dir_cfg,
    create_server_cfg,
    merge_server_cfg,
    oorx_cmds,
    register_hooks
};
