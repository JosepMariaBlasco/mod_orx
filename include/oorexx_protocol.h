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
                                  for the modrexx class, the snapshot of
                                  the request (see oorexx-host.cpp)        */
    OORX_BODY    = 'b',  /* M->H  request body chunk; length 0 = EOF      */
    OORX_READ    = 'R',  /* H->M  u32: please send up to n body bytes     */
    OORX_UPDATE  = 'U',  /* H->M  modrexx class: one change to the request
                                  (status, a header, a field...), as three
                                  strings: operation, key, value           */
    OORX_DATA    = 'D',  /* H->M  response bytes; flag FLUSH = flush now  */
    OORX_LOG     = 'L',  /* H->M  u8 level, then the message              */
    OORX_RESULT  = 'E'   /* H->M  u32 status (0 = OK), end of the phase;
                                  with flag VALUE, the bytes after it are
                                  what the program returned               */
};

/* Flags */
#define OORX_FLAG_FLUSH 0x01     /* DATA: flush now                        */
#define OORX_FLAG_VALUE 0x02     /* RESULT: the program returned a value   */

/* Phases */
enum { OORX_PHASE_CONTENT = 1, OORX_PHASE_AUTHN = 2, OORX_PHASE_AUTHZ = 3,
       OORX_PHASE_LOG = 4 };

/* Program classes */
enum { OORX_CLASS_CGI = 1, OORX_CLASS_MODREXX = 2, OORX_CLASS_NATIVE = 3 };

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
