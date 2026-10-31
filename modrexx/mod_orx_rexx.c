/*
 * mod_orx_rexx -- the mod_rexx layer of mod_orx: programs written for
 * mod_rexx run unchanged, in every phase, with mod_rexx's own directives
 * and their meaning, so that a configuration written for mod_rexx works as
 * it is once its LoadModule line is changed for these two:
 *
 *   LoadModule orx_module      modules/mod_orx.so
 *   LoadModule orx_rexx_module modules/mod_orx_rexx.so
 *
 *   <Directory ...>  AddHandler rexx-handler .rex
 *                    RexxSetVar NAME value
 *                    RexxFixupHandler /path/fixup.rex   </Directory>
 *   RexxTransHandler /path/trans.rex
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * It is optional: mod_orx neither needs nor knows it. It is built only on
 * the core's interface (include/mod_orx.h): the core runs each phase in
 * oorexx-host, and this module tells it what to send and what to allow.
 * In the host, its plugin (orx_rexx_plugin.so, registered with the core
 * at startup) provides the WWW* variables and functions. mod_rexx itself
 * cannot be loaded with it: they define the same directives.
 *
 * Its hooks run before the core's in every phase but two: they name
 * "mod_orx.c" as their successor (without its directory: Apache strips it;
 * measured). In translate_name and type_checker they name nothing, and keep
 * mod_rexx's place, which the order of LoadModule decides (after mod_alias
 * and mod_mime, as a rule): the core's native hooks there run early, before
 * mod_alias and mod_mime, and naming the core would drag these along.
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "apr_strings.h"

#include "../include/oorexx_protocol.h"
#include "../include/mod_orx.h"
#include "../build/config.h"   /* ORX_REXX_PLUGIN, written by make */

module AP_MODULE_DECLARE_DATA orx_rexx_module;

/* The core's functions (mod_orx.h), retrieved at startup.                  */
static APR_OPTIONAL_FN_TYPE(orx_abi)             *orx_abi;
static APR_OPTIONAL_FN_TYPE(orx_vars_new)        *orx_vars_new;
static APR_OPTIONAL_FN_TYPE(orx_vars_add)        *orx_vars_add;
static APR_OPTIONAL_FN_TYPE(orx_vars_related)    *orx_vars_related;
static APR_OPTIONAL_FN_TYPE(orx_phase_open)      *orx_phase_open;
static APR_OPTIONAL_FN_TYPE(orx_phase_stream)    *orx_phase_stream;
static APR_OPTIONAL_FN_TYPE(orx_phase_discard)   *orx_phase_discard;
static APR_OPTIONAL_FN_TYPE(orx_conn_drop)       *orx_conn_drop;
static APR_OPTIONAL_FN_TYPE(orx_conn_result)     *orx_conn_result;
static APR_OPTIONAL_FN_TYPE(orx_register_plugin) *orx_register_plugin;

static void retrieve(void)
{
    orx_abi             = APR_RETRIEVE_OPTIONAL_FN(orx_abi);
    orx_vars_new        = APR_RETRIEVE_OPTIONAL_FN(orx_vars_new);
    orx_vars_add        = APR_RETRIEVE_OPTIONAL_FN(orx_vars_add);
    orx_vars_related    = APR_RETRIEVE_OPTIONAL_FN(orx_vars_related);
    orx_phase_open      = APR_RETRIEVE_OPTIONAL_FN(orx_phase_open);
    orx_phase_stream    = APR_RETRIEVE_OPTIONAL_FN(orx_phase_stream);
    orx_phase_discard   = APR_RETRIEVE_OPTIONAL_FN(orx_phase_discard);
    orx_conn_drop       = APR_RETRIEVE_OPTIONAL_FN(orx_conn_drop);
    orx_conn_result     = APR_RETRIEVE_OPTIONAL_FN(orx_conn_result);
    orx_register_plugin = APR_RETRIEVE_OPTIONAL_FN(orx_register_plugin);
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/*                                                                          */
/* mod_rexx's directives (RexxHandler, RexxTransHandler..., RexxSetVar),    */
/* with their meaning: RexxAuthenHandler runs in auth_checker and           */
/* RexxAuthzHandler in check_user_id (the other way round from what their   */
/* names say); paths are taken as written; and a section with any Rexx*     */
/* directive replaces all those of the sections around it, which are not   */
/* inherited (mod_rexx had no merger: measured).                            */
/*                                                                          */
/* MR_CONTENT is RexxHandler: the program the content handler runs instead  */
/* of the requested file.                                                   */
/* ------------------------------------------------------------------------ */

enum { MR_CONTENT, MR_POST_READ, MR_TRANSLATE, MR_HEADER_PARSER, MR_ACCESS,
       MR_AUTHEN, MR_AUTHZ, MR_TYPE, MR_FIXUP, MR_LOG, MR_COUNT };

typedef struct {
    const char *ph[MR_COUNT];   /* the programs; NULL = unset            */
    apr_array_header_t *setvars;/* RexxSetVar: name, value...            */
} modrexx_set;

typedef struct {
    modrexx_set *rx;            /* all or nothing; NULL = none           */
} rx_dir_cfg;

static void *create_dir_cfg(apr_pool_t *p, char *dir)
{
    (void)dir;
    return apr_pcalloc(p, sizeof(rx_dir_cfg));
}

static void *merge_dir_cfg(apr_pool_t *p, void *basev, void *addv)
{
    rx_dir_cfg *base = basev, *add = addv, *c = apr_pcalloc(p, sizeof *c);
    c->rx = add->rx ? add->rx : base->rx;
    return c;
}

/* The section's set of Rexx* directives, made on its first one.            */
static modrexx_set *set_of(cmd_parms *cmd, rx_dir_cfg *c)
{
    if (!c->rx) c->rx = apr_pcalloc(cmd->pool, sizeof *c->rx);
    return c->rx;
}

/* RexxSetVar name value: a variable more for every program.                */
static const char *add_setvar(cmd_parms *cmd, void *dcfg, const char *name,
                              const char *value)
{
    modrexx_set *s = set_of(cmd, dcfg);
    if (!s->setvars) s->setvars = apr_array_make(cmd->pool, 4, sizeof(const char *));
    *(const char **)apr_array_push(s->setvars) = name;
    *(const char **)apr_array_push(s->setvars) = value;
    return NULL;
}

/* RexxHandler, RexxTransHandler...: the program of one phase, as written.  */
static const char *set_mr_program(cmd_parms *cmd, void *dcfg, const char *arg)
{
    modrexx_set *s = set_of(cmd, dcfg);
    s->ph[(int)(apr_uintptr_t)cmd->info] = arg;
    return NULL;
}

static const command_rec rx_cmds[] = {
    AP_INIT_TAKE2("RexxSetVar", add_setvar, NULL, OR_ALL,
                  "A variable (name and value) for every Rexx* program, as in mod_rexx"),
#define MR_DIRECTIVE(name, i, what) \
    AP_INIT_TAKE1("Rexx" name, set_mr_program, (void *)(apr_uintptr_t)(i), \
                  OR_ALL, "As in mod_rexx: a Rexx program " what)
    MR_DIRECTIVE("Handler", MR_CONTENT, "run by the content handler instead of the file"),
    MR_DIRECTIVE("PostReadRequestHandler", MR_POST_READ, "for post_read_request"),
    MR_DIRECTIVE("TransHandler", MR_TRANSLATE, "for translate_name"),
    MR_DIRECTIVE("HeaderParserHandler", MR_HEADER_PARSER, "for header_parser"),
    MR_DIRECTIVE("AccessHandler", MR_ACCESS, "for access_checker"),
    MR_DIRECTIVE("AuthenHandler", MR_AUTHEN, "for authentication (run in auth_checker)"),
    MR_DIRECTIVE("AuthzHandler", MR_AUTHZ, "for authorization (run in check_user_id)"),
    MR_DIRECTIVE("TypeHandler", MR_TYPE, "for type_checker"),
    MR_DIRECTIVE("FixupHandler", MR_FIXUP, "for fixups"),
    MR_DIRECTIVE("LogHandler", MR_LOG, "for log_transaction"),
#undef MR_DIRECTIVE
    {NULL}
};

/* ------------------------------------------------------------------------ */
/* Programs written for mod_rexx: the content handler and the other phases  */
/*                                                                          */
/* What a mod_rexx program expects (arg(1) is the request, the WWW*         */
/* functions read and change it, the variables WWW* exist before its first  */
/* line, what it returns is the handler's status) is provided by the host;  */
/* the module sends it the core's snapshot of the request, plus what only   */
/* mod_rexx reads, under the client's "+" keys:                             */
/*                                                                          */
/*   +v:NAME            the initial variables, in the order they are set    */
/*   +r:NAME            fields of the request that only mod_rexx reads      */
/*   +q:Nr:NAME         the same, of related request N                      */
/*   +s:NAME  +c:NAME   fields of its server and its connection             */
/*                                                                          */
/* and applies the changes the program makes. Its own change, +R, is       */
/* WWWInternal_Redirect, done at the end of the phase.                      */
/*                                                                          */
/* This part uses only the core's interface (mod_orx.h). Measured against   */
/* mod_rexx 2.4.0 (the last one) under Apache 2.4.                          */
/* ------------------------------------------------------------------------ */

#define MODREXX_VERSION "Mod_Rexx/2.4.0"

static void mr_put(orx_vars *v, apr_pool_t *p, const char *pre, const char *key,
                   const char *value)
{
    orx_vars_add(v, apr_pstrcat(p, pre, key, NULL), value);
}

/* The fields of one request that only mod_rexx reads.                      */
static void mr_request(orx_vars *v, const char *pre, request_rec *r)
{
    mr_put(v, r->pool, pre, "proxyreq", apr_itoa(r->pool, r->proxyreq));
    mr_put(v, r->pool, pre, "header_only", apr_itoa(r->pool, r->header_only));
    mr_put(v, r->pool, pre, "status_line", r->status_line);
    mr_put(v, r->pool, pre, "method_number", apr_itoa(r->pool, r->method_number));
    mr_put(v, r->pool, pre, "allowed", apr_itoa(r->pool, (int)r->allowed));
    mr_put(v, r->pool, pre, "no_cache", apr_itoa(r->pool, r->no_cache));
    mr_put(v, r->pool, pre, "finfo_stmode", apr_psprintf(r->pool, "%X", (unsigned)r->finfo.filetype));
    mr_put(v, r->pool, pre, "is_main", r->main ? "0" : "1");
    mr_put(v, r->pool, pre, "url_prefix", ap_construct_url(r->pool, "", r));
    mr_put(v, r->pool, pre, "remote_name", ap_get_remote_host(r->connection, r->per_dir_config,
                                                     REMOTE_NAME, NULL));
}

static orx_vars *modrexx_snapshot(request_rec *r, apr_array_header_t *setvars)
{
    orx_vars *v = orx_vars_new(r, ORX_VARS_ENV | ORX_VARS_REQUEST);
    request_rec *sub, **rel;
    int i, n;

    /* The initial variables, as mod_rexx's RXINI exit sets them.          */
    mr_put(v, r->pool, "v:", "WWWAUTH_TYPE", r->ap_auth_type);
    mr_put(v, r->pool, "v:", "WWWCONTENT_LENGTH", apr_table_get(r->headers_in, "Content-Length"));
    mr_put(v, r->pool, "v:", "WWWCONTENT_TYPE", apr_table_get(r->headers_in, "Content-Type"));
    mr_put(v, r->pool, "v:", "WWWGATEWAY_INTERFACE", MODREXX_VERSION);
    mr_put(v, r->pool, "v:", "WWWHTTP_USER_ACCEPT", apr_table_get(r->headers_in, "Accept"));
    mr_put(v, r->pool, "v:", "WWWHTTP_USER_AGENT", apr_table_get(r->headers_in, "User-Agent"));
    mr_put(v, r->pool, "v:", "WWWPATH_INFO", r->path_info);
    /* mod_rexx looks the path info up even when there is none, and gets   */
    /* the request's directory then.                                       */
    sub = ap_sub_req_lookup_uri(r->path_info ? r->path_info : "", r, NULL);
    mr_put(v, r->pool, "v:", "WWWPATH_TRANSLATED", sub->filename);
    ap_destroy_sub_req(sub);
    mr_put(v, r->pool, "v:", "WWWQUERY_STRING", r->args);
    mr_put(v, r->pool, "v:", "WWWREMOTE_ADDR", r->useragent_ip);
    mr_put(v, r->pool, "v:", "WWWREMOTE_HOST", ap_get_remote_host(r->connection, r->per_dir_config,
                                                         REMOTE_HOST, NULL));
    mr_put(v, r->pool, "v:", "WWWREMOTE_IDENT", ap_get_remote_logname(r));
    mr_put(v, r->pool, "v:", "WWWREMOTE_USER", r->user);
    mr_put(v, r->pool, "v:", "WWWREQUEST_METHOD", r->method);
    mr_put(v, r->pool, "v:", "WWWSCRIPT_NAME", r->uri);
    mr_put(v, r->pool, "v:", "WWWSERVER_NAME", ap_get_server_name(r));
    mr_put(v, r->pool, "v:", "WWWSERVER_PORT", apr_itoa(r->pool, ap_get_server_port(r)));
    mr_put(v, r->pool, "v:", "WWWSERVER_PROTOCOL", r->protocol);
    mr_put(v, r->pool, "v:", "WWWSERVER_SOFTWARE", ap_get_server_description());
    mr_put(v, r->pool, "v:", "WWWDEFAULT_TYPE", "text/plain");
    mr_put(v, r->pool, "v:", "WWWIS_MAIN_REQUEST", r->main ? "0" : "1");
    mr_put(v, r->pool, "v:", "WWWSERVER_ROOT", ap_server_root_relative(r->pool, ""));
    mr_put(v, r->pool, "v:", "WWWUNPARSEDURI", r->unparsed_uri);
    mr_put(v, r->pool, "v:", "WWWURI", r->uri);
    mr_put(v, r->pool, "v:", "WWWFILENAME", r->filename);
    mr_put(v, r->pool, "v:", "WWWPOST_STRING", "");
    if (setvars)
        for (i = 0; i + 1 < setvars->nelts; i += 2)
            mr_put(v, r->pool, "v:", APR_ARRAY_IDX(setvars, i, const char *),
                   APR_ARRAY_IDX(setvars, i + 1, const char *));
    /* mod_rexx's defaults; RSP itself is not supported.                   */
    mr_put(v, r->pool, "v:", "WWWRSPCOMPILER", "rspcomp.rex");
    mr_put(v, r->pool, "v:", "WWWFNAMETEMPLATE", "./execrsp?????.rex");

    /* The request and its related ones, numbered as in the core's part.   */
    n = orx_vars_related(v, &rel);
    for (i = 0; i < n; i++)
        mr_request(v, i ? apr_psprintf(r->pool, "q:%dr:", i) : "r:", rel[i]);
    mr_put(v, r->pool, "s:", "server_admin", r->server->server_admin);
    mr_put(v, r->pool, "s:", "server_hostname", r->server->server_hostname);
    mr_put(v, r->pool, "s:", "port", apr_itoa(r->pool, r->server->port));
    mr_put(v, r->pool, "s:", "is_virtual", apr_itoa(r->pool, r->server->is_virtual));
    mr_put(v, r->pool, "c:", "aborted", apr_itoa(r->pool, r->connection->aborted));
    return v;
}

/* The layer's own change: +R, WWWInternal_Redirect's URI. The program's    */
/* output is dropped from then on, as under mod_rexx.                       */
typedef struct { request_rec *r; const char *redirect; } modrexx_ctx;

static void modrexx_op(void *ctx, orx_conn *c, const char *op, const char *key,
                       const char *value)
{
    modrexx_ctx *m = ctx;
    (void)key;
    if (!strcmp(op, "+R")) {
        m->redirect = apr_pstrdup(m->r->pool, value);
        orx_conn_drop(c);
    } else
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, m->r,
                      "mod_orx: unknown update \"%s\" from oorexx-host", op);
}

/* What RexxStart made of a program's result, as mod_rexx used it: a whole  */
/* number that fits in a short, or 0 (no value, or any other value).        */
static int modrexx_status(const char *value)
{
    const char *p = value;
    long v = 0;
    int neg = 0, digits = 0;
    if (!p) return 0;
    while (*p == ' ') p++;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    for (; *p >= '0' && *p <= '9'; p++, digits++) {
        v = v * 10 + (*p - '0');
        if (v > 100000) return 0;
    }
    if (*p == '.') for (p++; *p == '0'; p++) ;
    while (*p == ' ') p++;
    if (!digits || *p) return 0;
    if (neg) v = -v;
    return v < -32768 || v > 32767 ? 0 : (int)v;
}

/* Run a program written for mod_rexx in one phase. In the content phase    */
/* (content != 0) its output is the response body; in any other, it goes   */
/* nowhere, and the body is read only if the program asks for it. Returns   */
/* the status for Apache: what the program returned, as mod_rexx took it.   */
static int modrexx_run(request_rec *r, int phase, const char *program,
                       apr_array_header_t *setvars, int content)
{
    modrexx_ctx m;
    orx_conn *c;
    orx_phase ph;
    apr_uint32_t status;
    const char *value;
    int ret;

    m.r = r; m.redirect = NULL;
    memset(&ph, 0, sizeof ph);
    ph.phase = phase;
    ph.klass = OORX_CLASS_MODREXX;
    ph.program = program;
    ph.vars = modrexx_snapshot(r, setvars);
    ph.body = content ? ORX_BODY_EAGER : ORX_BODY_LAZY;
    ph.allow = OORX_ALLOW_ALL;            /* mod_rexx let a program change    */
    ph.client_op = modrexx_op;            /* anything, in any phase           */
    ph.ctx = &m;
    if ((ret = orx_phase_open(r, &ph, &c)) != OK) return ret;

    /* The output is the body, as it comes; status and headers are what    */
    /* the program set. Apache holds a small response until the end, so    */
    /* what the program returns can still replace it (an error, say), as   */
    /* under mod_rexx. After WWWInternal_Redirect, it goes nowhere.         */
    if (!content) orx_phase_discard(c);
    else orx_phase_stream(c);
    if (!orx_conn_result(c, &status, &value)) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, errno, r,
                      "mod_orx: connection to oorexx-host lost while running %s", program);
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    if (status != 0) return HTTP_INTERNAL_SERVER_ERROR;   /* the host logged why */
    /* WWWInternal_Redirect, deferred to here.                              */
    if (m.redirect) {
        ap_internal_redirect(m.redirect, r);
        return OK;
    }
    /* 0 OK, -1 DECLINED, -2 DONE, or an HTTP status: mod_rexx's handlers   */
    /* return the number as it is, in every phase.                         */
    return modrexx_status(value);
}

/* The content handler, under mod_rexx's three names for it.                */
static int oorx_modrexx_handler(request_rec *r)
{
    rx_dir_cfg *dcfg;
    modrexx_set *set;

    if (strcmp(r->handler, "rexx-handler") && strcmp(r->handler, "rexx_handler") &&
        strcmp(r->handler, "application/x-httpd-rexx-script"))
        return DECLINED;

    /* mod_rexx answers a HEAD itself, without running the program.        */
    if (r->header_only) {
        ap_set_content_type(r, "text/html");
        return OK;
    }
    /* The requested file must exist, even when RexxHandler names the      */
    /* program to run, as under mod_rexx.                                   */
    if (r->finfo.filetype == APR_NOFILE) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_orx: program not found or unable to stat: %s", r->filename);
        return HTTP_NOT_FOUND;
    }
    dcfg = ap_get_module_config(r->per_dir_config, &orx_rexx_module);
    set = dcfg->rx;
    return modrexx_run(r, OORX_PHASE_CONTENT,
                       set && set->ph[MR_CONTENT] && *set->ph[MR_CONTENT] ?
                       set->ph[MR_CONTENT] : r->filename,
                       set ? set->setvars : NULL, 1);
}

/* The other phases. As under mod_rexx, nothing runs in a subrequest (the   */
/* hook declines), and what the program returns is the hook's status.       */
static int modrexx_hook(request_rec *r, int which, int phase)
{
    rx_dir_cfg *dcfg;
    modrexx_set *set;
    if (r->main) return DECLINED;
    dcfg = ap_get_module_config(r->per_dir_config, &orx_rexx_module);
    set = dcfg->rx;
    if (!set || !set->ph[which] || !*set->ph[which]) return DECLINED;
    return modrexx_run(r, phase, set->ph[which], set->setvars, 0);
}

#define MR_HOOK(fn, which, phase) \
    static int rx_##fn(request_rec *r) { return modrexx_hook(r, which, phase); }
MR_HOOK(post_read,     MR_POST_READ,     OORX_PHASE_POST_READ)
MR_HOOK(translate,     MR_TRANSLATE,     OORX_PHASE_TRANSLATE)
MR_HOOK(header_parser, MR_HEADER_PARSER, OORX_PHASE_HEADER_PARSER)
MR_HOOK(access,        MR_ACCESS,        OORX_PHASE_ACCESS)
MR_HOOK(authen,        MR_AUTHEN,        OORX_PHASE_AUTHN)
MR_HOOK(authz,         MR_AUTHZ,         OORX_PHASE_AUTHZ)
MR_HOOK(type,          MR_TYPE,          OORX_PHASE_TYPE)
MR_HOOK(fixup,         MR_FIXUP,         OORX_PHASE_FIXUP)
MR_HOOK(log,           MR_LOG,           OORX_PHASE_LOG)
#undef MR_HOOK

/* ------------------------------------------------------------------------ */
/* Startup                                                                  */
/* ------------------------------------------------------------------------ */

/* The core must be there, with the same interface; then the host must     */
/* load this layer's plugin. This runs before the core's post_config, which */
/* starts the host.                                                         */
static int rx_post_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp,
                          server_rec *s)
{
    (void)pconf; (void)plog; (void)ptemp;
    /* Apache runs optional_fn_retrieve only after post_config (measured;   */
    /* server/main.c), but the core registers its functions when it is     */
    /* loaded, so they can be fetched here already.                         */
    retrieve();
    if (!orx_abi || !orx_vars_new || !orx_vars_add || !orx_vars_related ||
        !orx_phase_open || !orx_phase_stream || !orx_phase_discard || !orx_conn_drop ||
        !orx_conn_result || !orx_register_plugin) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s,
                     "mod_orx_rexx: mod_orx is not loaded (LoadModule orx_module)");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    if (orx_abi() != ORX_MODULE_ABI) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s,
                     "mod_orx_rexx: built for another version of mod_orx "
                     "(interface %d, mod_orx has %d)", ORX_MODULE_ABI, orx_abi());
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    orx_register_plugin(OORX_CLASS_MODREXX, ORX_REXX_PLUGIN);
    return OK;
}

static void register_hooks(apr_pool_t *p)
{
    /* Before the core's own hooks, in every phase but Trans and Type (see   */
    /* the top).                                                             */
    static const char *const core[] = { "mod_orx.c", NULL };
    (void)p;
    ap_hook_optional_fn_retrieve(retrieve, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(rx_post_config, NULL, core, APR_HOOK_MIDDLE);
    ap_hook_handler(oorx_modrexx_handler, NULL, NULL, APR_HOOK_MIDDLE);
    /* At mod_rexx's positions: RexxAuthenHandler runs in auth_checker and  */
    /* RexxAuthzHandler in check_user_id, as under mod_rexx.                */
    ap_hook_post_read_request(rx_post_read, NULL, core, APR_HOOK_MIDDLE);
    ap_hook_translate_name(rx_translate, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_header_parser(rx_header_parser, NULL, core, APR_HOOK_MIDDLE);
    ap_hook_access_checker(rx_access, NULL, core, APR_HOOK_MIDDLE);
    ap_hook_check_user_id(rx_authz, NULL, core, APR_HOOK_MIDDLE);
    ap_hook_auth_checker(rx_authen, NULL, core, APR_HOOK_MIDDLE);
    ap_hook_type_checker(rx_type, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_fixups(rx_fixup, NULL, core, APR_HOOK_MIDDLE);
    ap_hook_log_transaction(rx_log, NULL, core, APR_HOOK_MIDDLE);
}

AP_DECLARE_MODULE(orx_rexx) = {
    STANDARD20_MODULE_STUFF,
    create_dir_cfg,
    merge_dir_cfg,
    NULL,
    NULL,
    rx_cmds,
    register_hooks
};
