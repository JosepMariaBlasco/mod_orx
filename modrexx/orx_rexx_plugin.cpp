/*
 * orx_rexx_plugin -- the mod_rexx layer of mod_orx, in oorexx-host: the
 * WWW* variables and functions of programs written for mod_rexx (the
 * modrexx class). Loaded by the host when mod_orx_rexx is (--plugin).
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 */

#include <oorexxapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include <string>
#include <vector>
#include <map>

extern "C" {
#include "../include/oorexx_protocol.h"
}
#include "../include/orx_host_plugin.h"

/* ------------------------------------------------------------------------ */
/* The modrexx class: programs written for mod_rexx (the mod_rexx layer)    */
/*                                                                          */
/* Written only against orx_host_plugin.h, as a plugin. The program gets,   */
/* instead of mod_rexx's request pointer, an opaque handle ("orx:" and 16   */
/* hex digits); WWWReqRecServer and WWWReqRecConnection give two more. The  */
/* instance has two context exits: RXINI sets the initial variables before  */
/* the program's first line, and RXOFNC answers the WWW* functions, which   */
/* are called without being declared, as they were under mod_rexx (internal */
/* routines and built-ins still come first). A read answers from the        */
/* snapshot: the core's, and the layer's own "+" keys of the module         */
/* (mod_orx.c): +v: the initial variables, +r: and +q:Nr: fields only       */
/* mod_rexx reads, +s: and +c: its server and connection. A write goes      */
/* through the host's update(). A handle that is not one of this phase's,   */
/* or a wrong number of arguments, is Error 40 in the program (mod_rexx     */
/* crashed the worker on a bad pointer).                                    */
/*                                                                          */
/* Behaviour measured against mod_rexx 2.4.0, function by function.        */
/* WWWReqRecNext, WWWReqRecPrev and WWWReqRecMain give handles to read-only */
/* snapshots of the related requests ("(nil)" if there is none). The        */
/* subrequest functions (WWWSub_Req_Lookup_URI, WWWSub_Req_Lookup_File,     */
/* WWWRun_Sub_Req) are not there, and a call to one is "routine not found": */
/* under mod_rexx no program could use them (they returned the pointer in   */
/* binary, which no other function took; measured).                         */
/* ------------------------------------------------------------------------ */

#define MODREXX_VERSION "Mod_Rexx/2.4.0"
#define M_GET  0                    /* Apache's method numbers */
#define M_POST 2

static const orx_host_api *rx_api;

typedef std::map<std::string, std::string> fields;

static struct {
    bool armed;                     /* the program is running             */
    int  phase;                     /* OORX_PHASE_*                       */
    std::string rh, sh, ch;         /* the handles                        */
    std::vector<std::string> relh;  /* related requests' handles ([0] "") */
    std::vector<std::pair<std::string, std::string> > init;   /* +v: */
    std::vector<fields> own;        /* +r:, +q:Nr: by request             */
    fields s, c;                    /* +s:, +c:                           */
} g_mr;

/* The number of the request whose handle h is (0: the phase's), or -1.     */
static int request_of(const std::string &h)
{
    if (h == g_mr.rh) return 0;
    for (size_t k = 1; k < g_mr.relh.size(); k++) if (h == g_mr.relh[k]) return (int)k;
    return -1;
}

/* A field of request q: the layer's own, or the core's.                    */
static std::string fld(int q, const std::string &key)
{
    if ((size_t)q < g_mr.own.size()) {
        fields::const_iterator f = g_mr.own[q].find(key);
        if (f != g_mr.own[q].end()) return f->second;
    }
    size_t n = 0;
    const char *v = rx_api->field(q, key.c_str(), &n);
    return v ? std::string(v, n) : std::string();
}

/* The first entry named key of table t (i x e n...) of request q, or "".    */
static std::string tbl(int q, char t, const std::string &key, bool *found = NULL)
{
    size_t n = 0;
    const char *v = rx_api->table_get(q, t, key.c_str(), &n);
    if (found) *found = v != NULL;
    return v ? std::string(v, n) : std::string();
}

static std::string new_handle(void)
{
    unsigned char b[8];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, sizeof b, f) != sizeof b) {
        for (size_t i = 0; i < sizeof b; i++) b[i] = (unsigned char)rand();
    }
    if (f) fclose(f);
    char h[24];
    snprintf(h, sizeof h, "orx:%02x%02x%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    return h;
}

/* The layer's "+" keys of the snapshot.                                    */
static void mr_key(void *, const char *key, size_t kl, const char *value, size_t vl)
{
    std::string k(key, kl), v(value, vl);
    if (k.size() < 2) return;
    if (k.compare(0, 2, "v:") == 0) { g_mr.init.push_back(std::make_pair(k.substr(2), v)); return; }
    if (k.compare(0, 2, "s:") == 0) { g_mr.s[k.substr(2)] = v; return; }
    if (k.compare(0, 2, "c:") == 0) { g_mr.c[k.substr(2)] = v; return; }
    size_t q = 0, p = 0;
    if (k.compare(0, 2, "q:") == 0) {
        p = 2;
        while (p < k.size() && isdigit((unsigned char)k[p])) q = q * 10 + (k[p++] - '0');
        if (q == 0 || q >= OORX_MAX_RELATED) return;
    }
    if (k.compare(p, 2, "r:") != 0) return;
    if (g_mr.own.size() <= q) g_mr.own.resize(q + 1);
    g_mr.own[q][k.substr(p + 2)] = v;
}

static void send_log_str(int level, const std::string &m) { rx_api->log(level, m.data(), m.size()); }

static std::string itos(long v)
{
    char b[32];
    snprintf(b, sizeof b, "%ld", v);
    return b;
}

/* Apache's ap_unescape_url after '+' -> ' ', as mod_rexx applied it: a     */
/* malformed escape stays as it is; a %00 ends the value (it was a C       */
/* string).                                                                 */
static std::string unescape(const std::string &in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        char ch = in[i];
        if (ch == '+') ch = ' ';
        else if (ch == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
                 isxdigit((unsigned char)in[i + 2])) {
            ch = (char)strtol(in.substr(i + 1, 2).c_str(), NULL, 16);
            i += 2;
        }
        if (ch == '\0') break;
        out += ch;
    }
    return out;
}

/* Apache's ap_getword: up to the next stop character, which is skipped.   */
static std::string getword(const std::string &s, size_t &pos, char stop)
{
    size_t e = s.find(stop, pos);
    std::string w = s.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    pos = e == std::string::npos ? s.size() : e + 1;
    return w;
}

/* A variable of the caller, set as mod_rexx set it: RXSHV_SET, the direct */
/* interface, with the name exactly as given. A compound name's tail is   */
/* taken literally, case included (an argument "user" is WWWARGS.user, and */
/* "k = 'user'; say wwwargs.k" finds it); a simple name in lowercase is a  */
/* variable no program can reach. SetContextVariable would be symbolic     */
/* instead (measured): it uppercases, and substitutes the tail's symbols.  */
static void setvar(RexxExitContext *, const std::string &name, const std::string &val)
{
    size_t z = name.find('\0');
    std::string n = z == std::string::npos ? name : name.substr(0, z);
    SHVBLOCK b;
    memset(&b, 0, sizeof b);
    b.shvcode = RXSHV_SET;
    b.shvname.strptr = n.c_str();  b.shvname.strlength = n.size();
    b.shvnamelen = n.size();
    b.shvvalue.strptr = (char *)val.data(); b.shvvalue.strlength = val.size();
    b.shvvaluelen = val.size();
    RexxVariablePool(&b);
}

/* WWWARGS. from a query string or a urlencoded body, as mod_rexx parsed it. */
static void parse_vars(RexxExitContext *c, const std::string &data)
{
    int num = 0;
    for (size_t pos = 0; pos < data.size(); ) {
        std::string val = getword(data, pos, '&');
        size_t vp = 0;
        std::string name = getword(val, vp, '=');
        std::string value = val.substr(vp);
        name = unescape(name); value = unescape(value);
        num++;
        setvar(c, "WWWARGS." + itos(num) + ".!NAME", name);
        setvar(c, "WWWARGS." + itos(num) + ".!VALUE", value);
        setvar(c, "WWWARGS." + name, value);
    }
    setvar(c, "WWWARGS.0", itos(num));
}

static void get_args(RexxExitContext *c)
{
    long method = atol(fld(0, "method_number").c_str());
    if (method == M_GET) { parse_vars(c, fld(0, "args")); return; }
    if (method == M_POST) {
        bool found;
        std::string ct = tbl(0, 'i', "Content-Type", &found);
        if (!found || !strcasecmp(ct.c_str(), "application/x-www-form-urlencoded")) {
            std::string data;                       /* the whole body       */
            char buf[OORX_CHUNK];
            size_t n;
            while ((n = rx_api->read_body(buf, sizeof buf)) > 0) data.append(buf, n);
            parse_vars(c, data);
            setvar(c, "WWWPOST_STRING", data);
            return;
        }
    }
    /* Any other method or content type: mod_rexx left WWWARGS.0 unset,    */
    /* which made the usual "do i = 1 to wwwargs.0" fail (Error 41).       */
    setvar(c, "WWWARGS.0", "0");
}

static void get_cookies(RexxExitContext *c)
{
    bool found;
    std::string data = tbl(0, 'i', "Cookie", &found);
    int num = 0;
    if (found) {
        for (size_t pos = 0; pos < data.size(); ) {
            std::string pair = getword(data, pos, ';');
            if (pos < data.size() && data[pos] == ' ') pos++;
            size_t pp = 0;
            std::string name = getword(pair, pp, '=');
            while (pp < pair.size()) {
                std::string val = unescape(getword(pair, pp, '&'));
                num++;
                setvar(c, "WWWCOOKIES." + itos(num) + ".!NAME", name);
                setvar(c, "WWWCOOKIES." + itos(num) + ".!VALUE", val);
                setvar(c, "WWWCOOKIES." + name, val);
            }
        }
    }
    setvar(c, "WWWCOOKIES.0", itos(num));
}

/* Apache's ap_os_escape_path (as measured: what is kept unescaped).       */
static std::string escape_path(const std::string &path, int partial)
{
    static const char keep[] = "!$&'()*+,-./:;=@_~";
    std::string out;
    if (!partial) {
        size_t colon = path.find(':'), slash = path.find('/');
        if (colon != std::string::npos && (slash == std::string::npos || colon < slash))
            out = "./";
    }
    for (size_t i = 0; i < path.size(); i++) {
        unsigned char ch = (unsigned char)path[i];
        if (ch == '\0') break;
        if (isalnum(ch) || strchr(keep, ch)) out += (char)ch;
        else { char e[4]; snprintf(e, sizeof e, "%%%02x", ch); out += e; }
    }
    return out;
}

/* A field of the request that a program may read and, with a second       */
/* argument, write. ret_new: the write returns the new value (method and   */
/* the numeric ones) rather than "". numeric: the value is a number, as    */
/* atoi() leaves it.                                                        */
struct field { const char *fn, *key, *op; bool numeric, ret_new; };
static const field rw_fields[] = {
    { "WWWREQRECSTATUS_LINE",      "status_line",       "L", false, false },
    { "WWWREQRECSTATUS",           "status",            "S", true,  false },
    { "WWWREQRECMETHOD",           "method",            "M", false, true  },
    { "WWWREQRECMETHOD_NUMBER",    "method_number",     "N", true,  true  },
    { "WWWREQRECALLOWED",          "allowed",           "A", true,  true  },
    { "WWWREQRECNO_CACHE",         "no_cache",          "C", true,  true  },
    { "WWWREQRECCONTENT_TYPE",     "content_type",      "T", false, false },
    { "WWWREQRECCONTENT_ENCODING", "content_encoding",  "E", false, false },
    { "WWWREQRECHANDLER",          "handler",           "H", false, false },
    { "WWWREQRECCONTENT_LANGUAGES","content_languages", "G", false, false },
    { "WWWREQRECURI",              "uri",               "U", false, false },
    { "WWWREQRECFILENAME",         "filename",          "F", false, false },
    { "WWWREQRECPATH_INFO",        "path_info",         "P", false, false },
};
static const struct { const char *fn, *key; } ro_fields[] = {
    { "WWWREQRECISMAIN", "is_main" },           { "WWWREQRECTHE_REQUEST", "the_request" },
    { "WWWREQRECPROXYREQ", "proxyreq" },        { "WWWREQRECHEADER_ONLY", "header_only" },
    { "WWWREQRECPROTOCOL", "protocol" },        { "WWWREQRECBYTES_SENT", "bytes_sent" },
    { "WWWREQRECARGS", "args" },                { "WWWREQRECFINFO_STMODE", "finfo_stmode" },
    { "WWWREQRECUSER", "user" },                { "WWWREQRECAUTH_TYPE", "auth_type" },
};
/* Tables: (r, key) reads, (r, key, value) writes.                          */
static const struct { const char *fn; char table; const char *op; } rw_tables[] = {
    { "WWWREQRECHEADER_IN",      'i', "si" },
    { "WWWREQRECERR_HEADER_OUT", 'x', "sx" },
    { "WWWREQRECSUBPROCESS_ENV", 'e', "se" },
    { "WWWREQRECNOTES",          'n', "sn" },
};

static int RexxEntry modrexx_ini(RexxExitContext *c, int, int sub, PEXIT)
{
    if (sub != RXINIEXT || !g_mr.armed) return RXEXIT_NOT_HANDLED;
    for (size_t i = 0; i < g_mr.init.size(); i++)
        setvar(c, g_mr.init[i].first, g_mr.init[i].second);
    return RXEXIT_HANDLED;
}

static int RexxEntry modrexx_fnc(RexxExitContext *c, int, int sub, PEXIT parm)
{
    if (sub != RXOFNCCAL || !g_mr.armed) return RXEXIT_NOT_HANDLED;
    RXOFNCCAL_PARM *fp = (RXOFNCCAL_PARM *)parm;
    if (fp->rxfnc_name.strlength < 3 || strncasecmp(fp->rxfnc_name.strptr, "WWW", 3))
        return RXEXIT_NOT_HANDLED;
    std::string fn(fp->rxfnc_name.strptr, fp->rxfnc_name.strlength);
    for (size_t i = 0; i < fn.size(); i++) fn[i] = (char)toupper((unsigned char)fn[i]);

    /* The arguments as strings; an omitted one is absent.                  */
    size_t argc = fp->rxfnc_argc;
    std::vector<std::string> a(argc);
    std::vector<bool> has(argc, false);
    for (size_t i = 0; i < argc; i++) {
        if (fp->rxfnc_argv[i] == NULLOBJECT) continue;
        RexxStringObject so = c->ObjectToString(fp->rxfnc_argv[i]);
        a[i].assign(c->StringData(so), c->StringLength(so));
        has[i] = true;
    }
    bool bad = false;
    std::string ret;
    /* Reads take any request of the phase (its own, or a related one);     */
    /* writes, only its own.                                                */
    int q = argc > 0 && fp->rxfnc_argv[0] != NULLOBJECT ? request_of(a[0]) : -1;
    /* "Argument i is there", and "argument i is this handle".             */
    #define ARG(i)     ((i) < argc && has[i])
    #define IS(i, h)   (ARG(i) && a[i] == (h))
    #define RET(v)     do { ret = (v); goto done; } while (0)
    #define BAD()      do { bad = true; goto done; } while (0)

    if (fn == "WWWGETVERSION") {
        if (argc != 0) BAD();
        RET(MODREXX_VERSION " " __DATE__ " " __TIME__);
    }
    for (size_t i = 0; i < sizeof rw_fields / sizeof *rw_fields; i++) {
        const field &f = rw_fields[i];
        if (fn != f.fn) continue;
        if (argc > 2 || q < 0 || (argc == 2 && (!ARG(1) || q != 0))) BAD();
        if (argc == 2) {
            std::string v = f.numeric ? itos(atoi(a[1].c_str())) : a[1];
            /* A field of the layer's own is kept here; a core one, by the  */
            /* host's update().                                             */
            fields::iterator own = g_mr.own[0].find(f.key);
            if (own != g_mr.own[0].end()) own->second = v;
            rx_api->update(f.op, "", v.data(), v.size());
            if (!f.ret_new) RET("");
        }
        RET(fld(q, f.key));
    }
    for (size_t i = 0; i < sizeof ro_fields / sizeof *ro_fields; i++) {
        if (fn != ro_fields[i].fn) continue;
        if (argc > 1 || q < 0) BAD();
        RET(fld(q, ro_fields[i].key));
    }
    for (size_t i = 0; i < sizeof rw_tables / sizeof *rw_tables; i++) {
        if (fn != rw_tables[i].fn) continue;
        if (argc < 2 || argc > 3 || q < 0 || !ARG(1) || (argc == 3 && (!ARG(2) || q != 0)))
            BAD();
        if (argc == 2) RET(tbl(q, rw_tables[i].table, a[1]));
        rx_api->update(rw_tables[i].op, a[1].c_str(), a[2].data(), a[2].size());
        RET("");
    }
    if (fn == "WWWREQRECHEADER_OUT" || fn == "WWWSETHEADERVALUE") {
        if (argc != 3 || !IS(0, g_mr.rh) || !ARG(1) || !ARG(2)) BAD();
        rx_api->update("so", a[1].c_str(), a[2].data(), a[2].size());
        RET("");
    }
    if (fn == "WWWADDCOOKIE") {
        if (argc != 2 || !IS(0, g_mr.rh) || !ARG(1)) BAD();
        rx_api->update("ao", "Set-Cookie", a[1].data(), a[1].size());
        RET("");
    }
    if (fn == "WWWSENDHTTPHEADER") {
        /* Only the content type: the headers go with the first output that */
        /* reaches the client, or at the end.                               */
        if (argc != 2 || !IS(0, g_mr.rh) || !ARG(1)) BAD();
        rx_api->update("T", "", a[1].data(), a[1].size());
        RET("");
    }
    if (fn == "WWWLOGERROR" || fn == "WWWLOGWARNING" || fn == "WWWLOGINFO") {
        if (argc != 2 || !IS(0, g_mr.rh) || !ARG(1)) BAD();
        rx_api->log(fn == "WWWLOGERROR" ? OORX_LOG_ERR : fn == "WWWLOGWARNING" ?
                    OORX_LOG_WARNING : OORX_LOG_INFO, a[1].c_str(), strlen(a[1].c_str()));
        RET("");
    }
    if (fn == "WWWLOGREASON") {
        /* mod_rexx logs it with no level, which is Apache's "emerg".       */
        if (argc != 3 || !IS(0, g_mr.rh) || !ARG(1) || !ARG(2)) BAD();
        std::string m = "Access to " + std::string(a[2].c_str()) + " failed for " +
                        fld(0, "remote_name") + ". reason: " + a[1].c_str();
        send_log_str(OORX_LOG_EMERG, m);
        RET("");
    }
    if (fn == "WWWGETARGS" || fn == "WWWGETCOOKIES") {
        if (argc != 1 || !IS(0, g_mr.rh)) BAD();
        if (fn == "WWWGETARGS") get_args(c); else get_cookies(c);
        RET("");
    }
    if (fn == "WWWINTERNAL_REDIRECT") {
        /* Done by the module at the end of the phase. Only in the content  */
        /* phase: in any other, mod_rexx crashed Apache's process.          */
        if (argc != 2 || !IS(0, g_mr.rh) || !ARG(1)) BAD();
        if (g_mr.phase != OORX_PHASE_CONTENT) {
            static const char m[] = "oorexx-host: WWWInternal_Redirect is only "
                                    "available in the content phase";
            rx_api->log(OORX_LOG_ERR, m, sizeof m - 1);
            BAD();
        }
        rx_api->update("+R", "", a[1].data(), a[1].size());
        RET("");
    }
    if (fn == "WWWESCAPE_PATH") {
        if (argc != 3 || !IS(0, g_mr.rh) || !ARG(1) || !ARG(2)) BAD();
        RET(escape_path(a[1], atoi(a[2].c_str())));
    }
    if (fn == "WWWCONSTRUCT_URL") {
        if (argc != 2 || !IS(0, g_mr.rh) || !ARG(1)) BAD();
        RET(fld(0, "url_prefix") + a[1].c_str());
    }
    if (fn == "WWWHTTP_TIME") {
        if (argc != 1 || !IS(0, g_mr.rh)) BAD();
        char b[128];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(b, sizeof b, "%a %d %b %Y %T %Z", &tm);
        RET(b);
    }
    if (fn == "WWWREQRECNEXT" || fn == "WWWREQRECPREV" || fn == "WWWREQRECMAIN") {
        /* mod_rexx printed the pointer with %p: "(nil)" for none.         */
        if (argc > 1 || q < 0) BAD();
        std::string l = fld(q, fn == "WWWREQRECNEXT" ? "next" :
                               fn == "WWWREQRECPREV" ? "prev" : "main");
        if (l.empty()) RET("(nil)");
        size_t k = strtoul(l.c_str(), NULL, 10);
        RET(k == 0 ? g_mr.rh : k < g_mr.relh.size() ? g_mr.relh[k] : "(nil)");
    }
    if (fn == "WWWREQRECSERVER" || fn == "WWWREQRECCONNECTION") {
        if (argc > 1 || q < 0) BAD();
        RET(fn == "WWWREQRECSERVER" ? g_mr.sh : g_mr.ch);
    }
    if (fn == "WWWSRVRECSERVER_ADMIN" || fn == "WWWSRVRECSERVER_HOSTNAME" ||
        fn == "WWWSRVRECPORT" || fn == "WWWSRVRECIS_VIRTUAL") {
        if (argc > 1 || !IS(0, g_mr.sh)) BAD();
        RET(g_mr.s[fn == "WWWSRVRECSERVER_ADMIN" ? "server_admin" :
                   fn == "WWWSRVRECSERVER_HOSTNAME" ? "server_hostname" :
                   fn == "WWWSRVRECPORT" ? "port" : "is_virtual"]);
    }
    if (fn == "WWWCNXRECABORTED") {
        if (argc > 1 || !IS(0, g_mr.ch)) BAD();
        RET(g_mr.c["aborted"]);
    }
    return RXEXIT_NOT_HANDLED;          /* not ours: the search goes on      */

done:
    #undef ARG
    #undef IS
    #undef RET
    #undef BAD
    if (bad) fp->rxfnc_flags.rxfferr = 1;     /* Error 40 in the program    */
    else fp->rxfnc_retc = c->NewString(ret.data(), ret.size());
    return RXEXIT_HANDLED;
}


/* The plugin.                                                              */
static RexxContextExit rx_exits[3] = { { modrexx_ini, RXINI }, { modrexx_fnc, RXOFNC },
                                       { NULL, 0 } };
static const char *rx_argv[1];
static size_t rx_argl[1];

static int rx_begin(int phase, const char *, orx_instance_policy *pol)
{
    /* Every phase there is, as under mod_rexx.                             */
    if (phase < OORX_PHASE_CONTENT || phase > OORX_PHASE_FIXUP) return 1;
    g_mr.phase = phase;
    g_mr.armed = false;
    g_mr.init.clear(); g_mr.own.assign(1, fields()); g_mr.s.clear(); g_mr.c.clear();
    rx_api->client_each(NULL, mr_key);
    g_mr.relh.assign(rx_api->related() + 1, std::string());
    for (size_t k = 1; k < g_mr.relh.size(); k++) g_mr.relh[k] = new_handle();
    g_mr.rh = new_handle(); g_mr.sh = new_handle(); g_mr.ch = new_handle();
    pol->exits = rx_exits;
    pol->address = "WWW";            /* mod_rexx's: no such environment, so  */
                                     /* a command fails (RC -3) instead of   */
                                     /* reaching a shell                     */
    pol->cwd = "/";                  /* Apache's directory, as under mod_rexx */
    pol->quiet_flush = 0;            /* mod_rexx could not flush             */
    pol->output_to_log = 0;          /* outside the content phase, the module */
                                     /* drops it                             */
    rx_argv[0] = g_mr.rh.c_str(); rx_argl[0] = g_mr.rh.size();
    pol->argc = 1; pol->argv = rx_argv; pol->argl = rx_argl;   /* its request */
    return 0;
}

static void rx_running(int on) { g_mr.armed = on != 0; }
static void rx_end(void) { }

static const orx_host_plugin rx_plugin = {
    ORX_PLUGIN_ABI, OORX_CLASS_MODREXX, rx_begin, rx_running, rx_end
};

extern "C" const orx_host_plugin *orx_plugin_init(const orx_host_api *api)
{
    if (!api || api->abi != ORX_PLUGIN_ABI) return NULL;
    rx_api = api;
    return &rx_plugin;
}

