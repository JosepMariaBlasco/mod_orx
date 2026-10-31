/*
 * oorexx_protocol.h -- the wire protocol between mod_orx (the Apache
 * module) and oorexx-host (the process that runs Rexx).
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * One connection carries ONE phase of ONE request. Every message is a frame:
 *
 *     [type: 1 byte][flags: 1 byte][length: 4 bytes, big-endian][payload]
 *
 * Nothing is ever parsed as text: lengths come first, always. Key/value
 * tables are sequences of (u32 length, bytes) pairs.
 *
 * Module -> host:  BEGIN, [ARGS], VARS, BODY
 * Host -> module:  READ, UPDATE, DATA, LOG, RESULT
 *
 * This header is shared by the module (C) and the host (C++). It holds only
 * POSIX file-descriptor helpers; the Windows transport is still to be
 * decided.
 */

#ifndef OOREXX_PROTOCOL_H
#define OOREXX_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define OORX_PROTOCOL_VERSION 0

#define OORX_HEADER_SIZE      6
#define OORX_MAX_FRAME        (1u << 24)   /* 16 MB: a sanity bound */
#define OORX_CHUNK            16384        /* preferred DATA/BODY size */

/* Frame types */
enum {
    OORX_BEGIN   = 'B',  /* M->H  u8 version, u8 phase, u8 class,
                                  u32 timeout (s), then the program path  */
    OORX_ARGS    = 'A',  /* M->H  the program's arguments, as a sequence of
                                  (u32 length, bytes); optional, between
                                  BEGIN and VARS                           */
    OORX_VARS    = 'V',  /* M->H  key/value table: the CGI environment;
                                  with flag SNAPSHOT, the snapshot of the
                                  request (keys below)                    */
    OORX_BODY    = 'b',  /* M->H  request body chunk; length 0 = EOF      */
    OORX_READ    = 'R',  /* H->M  u32: please send up to n body bytes     */
    OORX_UPDATE  = 'U',  /* H->M  one change to the request, as three
                                  strings: operation, key, value (below)   */
    OORX_DATA    = 'D',  /* H->M  response bytes; flag FLUSH = flush now  */
    OORX_LOG     = 'L',  /* H->M  u8 level, then the message              */
    OORX_RESULT  = 'E'   /* H->M  u32 status (0 = OK), end of the phase;
                                  with flag VALUE, the bytes after it are
                                  what the program returned               */
};

/* Flags */
#define OORX_FLAG_FLUSH 0x01     /* DATA: flush now                        */
#define OORX_FLAG_VALUE 0x02     /* RESULT: the program returned a value   */
#define OORX_FLAG_SNAPSHOT 0x04  /* VARS: a snapshot, not a bare environment */

/* Phases: Apache's hooks (authn = check_user_id, authz = auth_checker) */
enum { OORX_PHASE_CONTENT = 1, OORX_PHASE_AUTHN = 2, OORX_PHASE_AUTHZ = 3,
       OORX_PHASE_LOG = 4, OORX_PHASE_POST_READ = 5, OORX_PHASE_TRANSLATE = 6,
       OORX_PHASE_HEADER_PARSER = 7, OORX_PHASE_ACCESS = 8,
       OORX_PHASE_TYPE = 9, OORX_PHASE_FIXUP = 10 };

/* Program classes. CGI and NATIVE are the core's; MODREXX is served by    */
/* the optional mod_rexx layer, through its plugin of the host.            */
enum { OORX_CLASS_CGI = 1, OORX_CLASS_MODREXX = 2, OORX_CLASS_NATIVE = 3 };

/* The snapshot (VARS with flag SNAPSHOT). Every key starts with what it is: */
/*                                                                          */
/*   r:NAME      a field of the request, by its name in request_rec:        */
/*               the_request protocol method unparsed_uri uri args filename */
/*               path_info hostname handler content_type content_encoding  */
/*               content_languages (comma separated) status bytes_sent user */
/*               auth_type useragent_ip server_name server_port; and next,  */
/*               prev, main: the number of that related request, or ""     */
/*   i: o: x: e: n:   an entry of headers_in, headers_out, err_headers_out, */
/*               subprocess_env, notes (all of them, repeated ones too)    */
/*   q:N then one of the above (q:2r:uri, q:2i:Host): related request N    */
/*               (next, prev and main, followed from the request: 1..24)   */
/*   E:NAME      the CGI environment, which becomes the program's own      */
/*   +...        the client's own: not the core's business; the host hands */
/*               these to the client's plugin, in their order              */
#define OORX_MAX_RELATED 24

/* UPDATE operations of the core. Fields: one letter, no key. Tables: an   */
/* action (s set, a add, u unset) and the table (i o x e n, as above), the */
/* key being the entry's name. An operation that starts with "+" is the    */
/* client's own. The index of each one is its bit in a phase's allow mask. */
static const char *const oorx_ops[] = {
    "S", "L", "M", "N", "A", "C",   /* status status_line method          */
                                    /* method_number allowed no_cache     */
    "T", "E", "H", "G",             /* content_type content_encoding      */
                                    /* handler content_languages (adds)   */
    "U", "F", "P", "Q",             /* uri filename path_info args        */
    "si", "so", "sx", "se", "sn",
    "ai", "ao", "ax", "ae", "an",
    "ui", "uo", "ux", "ue", "un",
    NULL
};
#define OORX_ALLOW_ALL 0xFFFFFFFFu

/* The index of a core operation, or -1. */
static inline int oorx_op_index(const char *op)
{
    int i;
    for (i = 0; oorx_ops[i]; i++) if (!strcmp(op, oorx_ops[i])) return i;
    return -1;
}

/* LOG levels: the numbers of Apache's APLOG_* levels */
enum { OORX_LOG_EMERG = 0, OORX_LOG_ERR = 3, OORX_LOG_WARNING = 4, OORX_LOG_INFO = 6 };

static inline void oorx_put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static inline uint32_t oorx_get_u32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Write exactly len bytes. Returns 0, or -1 on error. */
static inline int oorx_write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* Read exactly len bytes. Returns 0, or -1 on error or premature EOF. */
static inline int oorx_read_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* Send one frame. Returns 0, or -1 on error. */
static inline int oorx_send(int fd, int type, int flags,
                            const void *payload, uint32_t len)
{
    unsigned char h[OORX_HEADER_SIZE];
    h[0] = (unsigned char)type; h[1] = (unsigned char)flags;
    oorx_put_u32(h + 2, len);
    if (oorx_write_all(fd, h, sizeof h) < 0) return -1;
    return len ? oorx_write_all(fd, payload, len) : 0;
}

/* Read one frame header. Returns 0, or -1 on error or an oversized frame. */
static inline int oorx_recv_header(int fd, int *type, int *flags,
                                   uint32_t *len)
{
    unsigned char h[OORX_HEADER_SIZE];
    if (oorx_read_all(fd, h, sizeof h) < 0) return -1;
    *type = h[0]; *flags = h[1]; *len = oorx_get_u32(h + 2);
    return *len > OORX_MAX_FRAME ? -1 : 0;
}

#endif /* OOREXX_PROTOCOL_H */
