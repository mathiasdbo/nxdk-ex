/*
 * Console test watchdog: a time-critical thread watches a heartbeat that the
 * test bumps at every step and frame. When it stops for WATCHDOG_TIMEOUT_MS,
 * the thread appends a line to WATCHDOG_LOG and resets the console through the
 * SMC. A reset keeps the hard disk powered, unlike holding the power button,
 * which cut power to the disk in the middle of its work during the earlier
 * lock-ups.
 *
 * Limits: the thread only runs while the scheduler does. A hang at raised IRQL
 * (in a DPC or an ISR) or a stalled bus access never lets it run; then nothing
 * is logged after the last step and the console still needs a power-off.
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#ifndef WATCHDOG_TIMEOUT_MS
#define WATCHDOG_TIMEOUT_MS 10000
#endif
#ifndef WATCHDOG_LOG
#define WATCHDOG_LOG "E:\\pbkit_watchdog.txt"
#endif

#define WATCHDOG_SMC_ADDRESS   0x20
#define WATCHDOG_SMC_REG_POWER 0x02
#define WATCHDOG_SMC_RESET     0x01

static volatile ULONG wd_heartbeat;
static const char *volatile wd_step = "start";

/* Called by the test at every step and frame; step must be a string literal
 * or otherwise stay valid. */
static inline void watchdog_kick (const char *step)
{
    if (step) {
        wd_step = step;
    }
    wd_heartbeat++;
}

static void watchdog_log (const char *line)
{
    DWORD written;
    HANDLE h = CreateFileA(WATCHDOG_LOG, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
    CloseHandle(h);
}

static DWORD WINAPI watchdog_thread (LPVOID arg)
{
    ULONG last = wd_heartbeat;
    DWORD stalled = 0;
    char line[160];

    (void)arg;
    for (;;) {
        Sleep(100);
        if (wd_heartbeat != last) {
            last = wd_heartbeat;
            stalled = 0;
            continue;
        }
        stalled += 100;
        if (stalled >= WATCHDOG_TIMEOUT_MS) {
            break;
        }
    }
    snprintf(line, sizeof(line), "WATCHDOG: no heartbeat for %lu ms after '%s' (beat %lu), SMC reset\r\n",
             (unsigned long)stalled, wd_step, (unsigned long)last);
    DbgPrint("%s", line);
    watchdog_log(line);
    HalWriteSMBusValue(WATCHDOG_SMC_ADDRESS, WATCHDOG_SMC_REG_POWER, FALSE, WATCHDOG_SMC_RESET);
    for (;;) {
        Sleep(1000);
    }
    return 0;
}

/* Starts the watchdog thread; call once, after E: is mounted. */
static void watchdog_start (void)
{
    HANDLE t = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(t);
    }
}

#endif
