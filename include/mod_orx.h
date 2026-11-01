/*
 * mod_orx.h -- what the core of mod_orx offers to the other Apache modules
 * of mod_orx (today, the optional mod_rexx layer, mod_orx_rexx): a way to
 * run one phase of a request in oorexx-host.
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * The functions are APR optional functions: a client retrieves them in its
 * optional_fn_retrieve hook, and checks orx_abi() against ORX_MODULE_ABI.
 * This interface is INTERNAL to mod_orx: the core and its clients are built
 * together, and the version must match exactly. Nothing is promised to
 * third parties.
 *
 * A client that runs before the core in a hook names it "mod_orx.c", with
 * no directory: Apache strips the directory from a module's name before it
 * registers its hooks (measured; server/config.c, ap_add_module).
 *
 * Running a phase:
 *
 *     orx_vars *v = orx_vars_new(r, ORX_VARS_ENV | ORX_VARS_REQUEST);
 *     orx_vars_add(v, "mykey", "value");          -- travels as "+mykey"
 *     orx_phase ph = { ... v ... };
 *     if ((status = orx_phase_open(r, &ph, &c)) != OK) return status;
 *     orx_phase_stream(c);                         -- or orx_phase_discard(c)
 *     if (!orx_conn_result(c, &st, &value)) ...    -- the connection broke
 */

#ifndef MOD_ORX_H
#define MOD_ORX_H

#include "httpd.h"
#include "apr_optional.h"

#define ORX_MODULE_ABI 1

typedef struct orx_vars orx_vars;     /* the VARS frame being built          */
typedef struct orx_conn orx_conn;     /* a phase in progress                 */

/* What the core puts in VARS (orx_vars_new).                               */
#define ORX_VARS_ENV      1   /* the CGI environment, as the program's own  */
#define ORX_VARS_REQUEST  2   /* the snapshot of the request and of the     */
                              /* requests related to it (oorexx_protocol.h) */

/* How the request body is offered to the program.                          */
enum { ORX_BODY_NONE,         /* no body: every READ is answered with EOF   */
       ORX_BODY_EAGER,        /* set up before connecting (content phase)   */
       ORX_BODY_LAZY };       /* set up on the first READ, if one comes     */

/* An UPDATE operation of the client's own ("+..."): the core does not      */
/* interpret it, and hands it to the client.                                */
typedef void orx_client_op(void *ctx, orx_conn *c, const char *op,
                           const char *key, const char *value);

typedef struct {
    int         phase;            /* OORX_PHASE_*                           */
    int         klass;            /* OORX_CLASS_*                           */
    const char *program;
    int         has_args;         /* send ARGS (even with no argument)      */
    const char *const *args;
    int         nargs;
    orx_vars   *vars;
    int         body;             /* ORX_BODY_*                             */
    apr_uint32_t allow;           /* the core UPDATE operations allowed:    */
                                  /* OORX_ALLOW_* bits, or OORX_ALLOW_ALL   */
    orx_client_op *client_op;     /* the client's own operations, or NULL   */
    void       *ctx;              /* for client_op                          */
} orx_phase;

APR_DECLARE_OPTIONAL_FN(int, orx_abi, (void));
/* VARS for this request: ORX_VARS_* bits.                                  */
APR_DECLARE_OPTIONAL_FN(orx_vars *, orx_vars_new, (request_rec *r, int what));
/* A key of the client's own: it travels as "+" key, after the core's ones, */
/* in the order added, and the host hands it to the client's plugin.        */
APR_DECLARE_OPTIONAL_FN(void, orx_vars_add, (orx_vars *v, const char *key,
                                             const char *value));
/* The related requests of the snapshot, in its numbering: (*rel)[0] is the */
/* request itself. Returns how many there are (0 without ORX_VARS_REQUEST). */
APR_DECLARE_OPTIONAL_FN(int, orx_vars_related, (orx_vars *v, request_rec ***rel));
/* Connect and start the phase. OK, with the connection in *c; or the       */
/* status to answer with (the reason is logged): 503 when no host process   */
/* takes the phase within OoRexxQueueTimeout (in the logging phase,         */
/* OoRexxLogQueueTimeout).                                                  */
APR_DECLARE_OPTIONAL_FN(int, orx_phase_open, (request_rec *r, const orx_phase *ph,
                                              orx_conn **c));
/* The program's output: to the client as it comes, or nowhere. Both return */
/* when the phase ends.                                                     */
APR_DECLARE_OPTIONAL_FN(void, orx_phase_stream, (orx_conn *c));
APR_DECLARE_OPTIONAL_FN(void, orx_phase_discard, (orx_conn *c));
/* From now on, the output goes nowhere.                                    */
APR_DECLARE_OPTIONAL_FN(void, orx_conn_drop, (orx_conn *c));
/* 1 if the phase ended (RESULT), with its status (0 = the program ran) and */
/* what the program returned (NULL if nothing); 0 if the connection broke.  */
APR_DECLARE_OPTIONAL_FN(int, orx_conn_result, (orx_conn *c, apr_uint32_t *status,
                                               const char **value));
/* The host must load this plugin for this class: call it in post_config,  */
/* before the core's (whose post_config starts the host).                   */
APR_DECLARE_OPTIONAL_FN(void, orx_register_plugin, (int klass, const char *path));

#endif /* MOD_ORX_H */
