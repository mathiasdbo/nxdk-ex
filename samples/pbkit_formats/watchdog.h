/*
 * Console test watchdog: a time-critical thread watches a heartbeat that the
 * test bumps at every step and frame. When it stops for WATCHDOG_TIMEOUT_MS,
 * the thread appends a line to WATCHDOG_LOG and resets the console through the
 * SMC. A reset keeps the hard disk powered, unlike holding the power button,
 * which cut power to the disk in the middle of its work during the earlier
 * lock-ups.
 *
 * It also resets as soon as pbkit records a GPU error: watchdog_start() puts
 * pbkit in record-only mode (pb_set_gpu_error_halt(false)), since pbkit's own
 * error screen halts inside its DPC and freezes every thread, this one too.
 *
 * Limits: the thread only runs while the scheduler does. A hang at raised IRQL
 * (in a DPC or an ISR) or a stalled bus access never lets it run; then nothing
 * is logged after the last step and the console still needs a power-off.
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <pbkit/pbkit.h>
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
    char line[256];
    const volatile pb_gpu_error *gpu = pb_get_gpu_error();

    (void)arg;
    for (;;) {
        Sleep(100);
        if (gpu->count) {
            snprintf(line, sizeof(line),
                     "WATCHDOG: GPU error after '%s': nsource %08lX method %04lX data %08lX %08lX "
                     "A %08lX B %08lX limit %08lX (count %lu), SMC reset\r\n",
                     wd_step, (unsigned long)gpu->nsource, (unsigned long)gpu->address,
                     (unsigned long)gpu->data_low, (unsigned long)gpu->data_high, (unsigned long)gpu->param_a,
                     (unsigned long)gpu->param_b, (unsigned long)gpu->limit_details, (unsigned long)gpu->count);
            DbgPrint("%s", line);
            watchdog_log(line);
            HalWriteSMBusValue(WATCHDOG_SMC_ADDRESS, WATCHDOG_SMC_REG_POWER, FALSE, WATCHDOG_SMC_RESET);
            for (;;) {
                Sleep(1000);
            }
        }
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
/* Read-only probe of the disk's sector 0 (the F: partition table, which has
 * been lost three times around these tests): writes "sector0 <when>: table
 * yes|NO, bytes ..." into line, so the log shows when it changes. */
static void watchdog_sector0 (const char *when, char *line, size_t size)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    LARGE_INTEGER off;
    HANDLE h;
    NTSTATUS st;
    unsigned char *s = MmAllocateContiguousMemory(4096);

    if (!s) {
        snprintf(line, size, "sector0 %s: no memory", when);
        return;
    }
    RtlInitAnsiString(&name, "\Device\Harddisk0\Partition0");
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = NtOpenFile(&h, SYNCHRONIZE | GENERIC_READ, &attr, &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NO_INTERMEDIATE_BUFFERING);
    if (st >= 0) {
        off.QuadPart = 0;
        st = NtReadFile(h, NULL, NULL, NULL, &io, s, 4096, &off);
        NtClose(h);
    }
    if (st < 0) {
        snprintf(line, size, "sector0 %s: read failed %08lX", when, (unsigned long)st);
    } else {
        snprintf(line, size, "sector0 %s: table %s, bytes %02X %02X %02X %02X %02X %02X %02X %02X, boot count %02X",
                 when, memcmp(s, "****PARTINFO****", 16) == 0 ? "yes" : "NO", s[0], s[1], s[2], s[3], s[4], s[5],
                 s[6], s[7], s[3 * 512 + 4]);
    }
    MmFreeContiguousMemory(s);
}

static void watchdog_start (void)
{
    HANDLE t;

    pb_set_gpu_error_halt(false);
    t = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(t);
    }
}

#endif
