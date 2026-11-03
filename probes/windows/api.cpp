/* Probe: does the ooRexx installer for Windows carry a usable API?
 *
 * Builds against the installer's api/ folder (headers, rexx.lib) and does
 * what oorexx-host does: one anchor instance that stays, then a new
 * instance per "request", each running a small routine and terminated.
 * Prints how long each takes.
 *
 *   api.exe
 *
 * Copyright 2026 Josep Maria Blasco -- Apache License 2.0
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "oorexxapi.h"

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* Runs src in a new routine of tc; returns its result as a string. */
static const char *run(RexxThreadContext *tc, const char *src, char *out, size_t n)
{
    RexxRoutineObject r = tc->NewRoutine("probe", src, strlen(src));
    if (r == NULLOBJECT) { snprintf(out, n, "(NewRoutine failed)"); return out; }
    RexxObjectPtr res = tc->CallRoutine(r, NULL);
    if (tc->CheckCondition()) { snprintf(out, n, "(condition raised)"); tc->ClearCondition(); return out; }
    snprintf(out, n, "%s", res != NULLOBJECT ? tc->CString(res) : "(no result)");
    return out;
}

int main(void)
{
    char buf[512];
    int fails = 0;

    double t0 = now_ms();
    RexxInstance *anchor; RexxThreadContext *atc;
    if (!RexxCreateInterpreter(&anchor, &atc, NULL)) {
        printf("FAIL the anchor: RexxCreateInterpreter returned false\n");
        return 1;
    }
    double t1 = now_ms();
    printf("anchor: created in %.1f ms; interpreter version %08x\n", t1 - t0,
           (unsigned)anchor->InterpreterVersion());
    printf("anchor: %s\n", run(atc, "parse version v; return v", buf, sizeof buf));

    const int N = 200;
    double create = 0, call = 0, worst = 0;
    for (int i = 0; i < N; i++) {
        RexxInstance *inst; RexxThreadContext *tc;
        double a = now_ms();
        if (!RexxCreateInterpreter(&inst, &tc, NULL)) {
            printf("FAIL instance %d: RexxCreateInterpreter returned false\n", i);
            fails++;
            break;
        }
        double b = now_ms();
        run(tc, "return 6 * 7", buf, sizeof buf);
        if (strcmp(buf, "42") != 0) { printf("FAIL instance %d: got %s\n", i, buf); fails++; }
        inst->Terminate();
        double c = now_ms();
        create += b - a; call += c - b;
        if (c - a > worst) worst = c - a;
    }
    printf("per-request instances: %d, mean create %.3f ms, mean run+terminate %.3f ms, worst %.3f ms\n",
           N, create / N, call / N, worst);
    anchor->Terminate();
    printf("RESULT api %s: anchor %.1f ms, per-request instance %.3f ms\n",
           fails ? "FAIL" : "ok", t1 - t0, (create + call) / N);
    return fails ? 1 : 0;
}
