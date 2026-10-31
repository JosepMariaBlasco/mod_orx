/*
 * orx_host_plugin.h -- the interface between oorexx-host and a plugin that
 * serves a class of program (today, the optional mod_rexx layer, for the
 * modrexx class).
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * A plugin is a shared library with one exported symbol, orx_plugin_init.
 * The host loads it at start (--plugin CLASS:PATH), before it creates any
 * interpreter, and refuses to start (exit status 2, "configuration error")
 * if the library, the symbol, the ABI or the class does not match. This
 * interface is INTERNAL to mod_orx: the host and its plugins are built
 * together, and ORX_PLUGIN_ABI must match exactly. Nothing is promised to
 * third parties.
 *
 * For each phase of its class, the host: reads BEGIN, ARGS and VARS (the
 * snapshot of the request: keys in oorexx_protocol.h); calls begin(), which
 * fills in the policy of the instance; creates the instance with it and
 * runs the bootstrap; calls running(1), runs the program, calls running(0);
 * terminates the instance; and calls end(). In between, the plugin reads
 * the snapshot and changes the request through the functions it was given.
 */

#ifndef ORX_HOST_PLUGIN_H
#define ORX_HOST_PLUGIN_H

#include <stddef.h>
#include <oorexxapi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORX_PLUGIN_ABI 1

typedef void orx_kv_fn(void *ctx, const char *key, size_t kl, const char *value,
                       size_t vl);

typedef struct {                        /* the host, to the plugin            */
    int abi;
    /* The snapshot. q is a request: 0 the phase's, 1..related() the ones   */
    /* related to it. A field or an entry that is not there is NULL.        */
    const char *(*field)(int q, const char *name, size_t *len);
    const char *(*table_get)(int q, char table, const char *key, size_t *len);
                                        /* table: i o x e n; the first entry */
    void        (*table_each)(int q, char table, void *ctx, orx_kv_fn *fn);
    int         (*related)(void);
    void        (*client_each)(void *ctx, orx_kv_fn *fn);
                                        /* the "+" keys, without the "+", in */
                                        /* their order                       */
    /* A change to the request: an operation of oorexx_protocol.h, or the   */
    /* plugin's own ("+..."). A core one changes the snapshot too, so that a */
    /* later read sees it; then it goes to the module, which applies it.    */
    void        (*update)(const char *op, const char *key, const char *value,
                          size_t vl);
    /* A line for Apache's error log, at an APLOG_* level.                  */
    void        (*log)(int level, const char *msg, size_t len);
    /* Up to max bytes of the request body; 0 at its end. What the program  */
    /* has already read (through .input and the like) is not read again.    */
    size_t      (*read_body)(char *buf, size_t max);
} orx_host_api;

typedef struct {                        /* the plugin's instance, per phase   */
    RexxContextExit *exits;             /* DIRECT_EXITS, or NULL              */
    const char  *address;               /* INITIAL_ADDRESS_ENVIRONMENT, or    */
                                        /* NULL                               */
    const char  *cwd;                   /* NULL: the program's directory      */
    int          quiet_flush;           /* flush when the program goes quiet  */
    int          output_to_log;         /* the output goes to the error log   */
    size_t       argc;                  /* the program's arguments            */
    const char *const *argv;
    const size_t *argl;
} orx_instance_policy;

typedef struct {                        /* the plugin, to the host            */
    int          abi;
    int          klass;                 /* the OORX_CLASS_* it serves         */
    /* After VARS: fill in the policy. Nonzero refuses the phase (500).     */
    int  (*begin)(int phase, const char *program, orx_instance_policy *pol);
    /* Around the program itself (not the host's own bootstrap, which runs  */
    /* in the same instance before it).                                     */
    void (*running)(int on);
    void (*end)(void);                  /* the instance is gone               */
} orx_host_plugin;

typedef const orx_host_plugin *orx_plugin_init_fn(const orx_host_api *api);
#define ORX_PLUGIN_INIT "orx_plugin_init"

#ifdef __cplusplus
}
#endif

#endif /* ORX_HOST_PLUGIN_H */
