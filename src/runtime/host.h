/* Platform layer (host.c): Linux and Horizon. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct HostThread HostThread;
/* Runs fn(arg) on a new thread with a host stack of `stack` bytes. */
HostThread *host_thread_start(void (*fn)(void *), void *arg, size_t stack);
void host_use_single_core(int core);   /* confines every guest thread to one core (Switch); -1 spreads them */
HostThread *host_thread_adopt(void);   /* registers the calling thread */
HostThread *host_thread_self(void);
void host_thread_exit(void) __attribute__((noreturn));
/* Stops / restarts a thread the way SuspendThread does (a thread may pause itself). */
void host_thread_pause(HostThread *t);
void host_thread_resume(HostThread *t);
void host_thread_signal(HostThread *t, int sig);  /* Linux debugging (SH3_DUMP_AFTER) */

void *host_arena_reserve(void);                     /* 4 GB of address space for the guest */
void host_arena_commit(uint32_t va, uint32_t len);  /* backs [va, va+len) with zeroed memory */
void host_crash_handlers(void (*on_crash)(uintptr_t addr));
void host_backtrace(int fd);  /* host frames, where the platform can name them */
