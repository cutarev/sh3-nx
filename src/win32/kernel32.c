/* KERNEL32 and WINMM: files, threads, sync objects, time, resources. */
#include "../runtime/rt.h"
#include "../runtime/host.h"
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <ctype.h>

#define INVALID_HANDLE 0xFFFFFFFFu
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_NO_MORE_FILES 18
#define WAIT_TIMEOUT 0x102

static __thread uint32_t last_error;
void rt_set_last_error(uint32_t e) { last_error = e; }

/* ---- paths ---- */

/* Finds name in dir ignoring case. Linux file systems are case sensitive, Windows (and the Switch SD card) not. */
static int match_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(*dir ? dir : ".");
    struct dirent *e;
    int found = 0;
    while (d && (e = readdir(d)))
        if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s", e->d_name); found = 1; break; }
    if (d) closedir(d);
    return found;
}

const char *rt_path(const char *guest, char *out, size_t n) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", guest);
    for (char *c = tmp; *c; c++) if (*c == '\\') *c = '/';
    const char *p = tmp;
    char cd[512];
    if ((p[0] == 'd' || p[0] == 'D') && p[1] == ':' && !strncasecmp(p + 2, "/movie/", 7)) {
        snprintf(cd, sizeof cd, "data%s", p + 2);  /* the virtual CD (GetDriveTypeA): its Movie folder is data/movie */
        p = cd;
    }
    if (p[0] && p[1] == ':') p += 2;               /* any drive is the game folder */
    if (!strncasecmp(p, "/SH3/", 5)) p += 5;       /* the folder GetFullPathNameA reports */
    while (*p == '/') p++;
    if (!strncmp(p, "./", 2)) p += 2;
    struct stat st;
    if (*p && stat(p, &st) == 0) {  /* exact spelling exists: no directory scans */
        snprintf(out, n, "%s", p);
        return out;
    }
    out[0] = 0;
    size_t len = 0;
    for (const char *s = p; *s;) {
        size_t cl = strcspn(s, "/");
        char comp[256], real[256];
        snprintf(comp, sizeof comp, "%.*s", (int)cl, s);
        if (!match_ci(out, comp, real, sizeof real)) snprintf(real, sizeof real, "%s", comp);
        len += snprintf(out + len, n - len, "%s%s", len ? "/" : "", real);
        s += cl;
        while (*s == '/') s++;
    }
    if (!len) snprintf(out, n, ".");
    return out;
}

/* ---- kernel objects ---- */
enum { H_FILE = 1, H_EVENT, H_MUTEX, H_THREAD, H_FIND, H_MAPPING, H_TIMER, H_SEM };
typedef struct Obj {
    int type;
    FILE *f;
    int signaled, manual, count, max;  /* events, mutexes (count = recursion), semaphores, threads (signaled = done) */
    int waiters, pulsed;               /* threads in wait_one; how many of them the last PulseEvent frees */
    uint32_t pulse;                    /* PulseEvent count, so a wait knows a pulse came after it began */
    HostThread *owner, *thread;        /* mutexes: who holds it; threads: the host thread */
    uint32_t exit_code, tid, suspended;
    int started;
    DIR *dir;                          /* FindFirstFile */
    char pattern[256], dirpath[512];
    int dots;                          /* "." and ".." listed so far */
    uint32_t map_file;                 /* CreateFileMapping */
} Obj;

/* ponytail: one lock and one condition variable for every waitable object. Fine for a game's
 * handful of threads; per-object conditions if wakeups ever show up in a profile. */
static pthread_mutex_t obj_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t obj_cv = PTHREAD_COND_INITIALIZER;
static Obj *objs[4096];

static uint32_t new_handle(int type) {
    pthread_mutex_lock(&obj_mx);
    for (int i = 1; i < 4096; i++)
        if (!objs[i]) {
            objs[i] = calloc(1, sizeof(Obj));
            objs[i]->type = type;
            pthread_mutex_unlock(&obj_mx);
            return 0x1000 + i * 4;
        }
    rt_fatal("out of handles");
}

static __thread uint32_t self_handle;  /* GetCurrentThread's pseudo handle resolves here */

static Obj *obj(uint32_t h) {
    if (h == 0xFFFFFFFEu) h = self_handle;
    uint32_t i = (h - 0x1000) / 4;
    return h >= 0x1000 && i < 4096 ? objs[i] : NULL;
}

static int close_handle(uint32_t h) {
    Obj *o = obj(h);
    if (!o) return 0;
    if (o->f) fclose(o->f);
    if (o->dir) closedir(o->dir);
    if (o->type != H_THREAD) {  /* a running thread still uses its object */
        pthread_mutex_lock(&obj_mx);
        objs[(h - 0x1000) / 4] = NULL;
        pthread_mutex_unlock(&obj_mx);
        free(o);
    }
    return 1;
}
WINAPI(CloseHandle, "CloseHandle", 1) { return close_handle(ARG(0)); }

/* ---- sync ---- */

static int ready(Obj *o) {
    if (o->type == H_MUTEX) return !o->count || o->owner == host_thread_self();
    if (o->type == H_SEM) return o->count > 0;
    return o->signaled;
}

static void acquire(Obj *o) {
    if (o->type == H_MUTEX) { o->owner = host_thread_self(); o->count++; }
    else if (o->type == H_EVENT && !o->manual) o->signaled = 0;
    else if (o->type == H_SEM) o->count--;
}

static uint32_t wait_one(uint32_t h, uint32_t ms) {
    Obj *o = obj(h);
    if (!o) return 0xFFFFFFFFu;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&obj_mx);
    uint32_t pulse = o->pulse;
    o->waiters++;
    while (!ready(o) && !(o->pulse != pulse && o->pulsed)) {
        if (ms == 0 || (ms != 0xFFFFFFFFu && pthread_cond_timedwait(&obj_cv, &obj_mx, &ts) == ETIMEDOUT)) {
            o->waiters--;
            pthread_mutex_unlock(&obj_mx);
            return WAIT_TIMEOUT;
        }
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&obj_cv, &obj_mx);
    }
    if (ready(o)) acquire(o);
    else o->pulsed--;
    o->waiters--;
    pthread_mutex_unlock(&obj_mx);
    return 0;
}

static void signal_obj(Obj *o, int state) {
    pthread_mutex_lock(&obj_mx);
    o->signaled = state;
    pthread_cond_broadcast(&obj_cv);
    pthread_mutex_unlock(&obj_mx);
}

void rt_signal_event(uint32_t h) { Obj *o = obj(h); if (o) signal_obj(o, 1); }

WINAPI(WaitForSingleObject, "WaitForSingleObject", 2) { return wait_one(ARG(0), ARG(1)); }

/* ponytail: PulseEvent's pulse counting is wait_one's only; a pulse while a thread waits here is lost. */
WINAPI(WaitForMultipleObjects, "WaitForMultipleObjects", 4) {
    uint32_t n = ARG(0), hs = ARG(1), all = ARG(2), ms = ARG(3);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&obj_mx);
    for (;;) {
        uint32_t hit = 0xFFFFFFFFu, n_ready = 0;
        for (uint32_t i = 0; i < n; i++) {
            Obj *o = obj(MEM32(hs + 4 * i));
            if (o && ready(o)) { n_ready++; if (hit == 0xFFFFFFFFu) hit = i; }
        }
        if (all ? n_ready == n : hit != 0xFFFFFFFFu) {
            for (uint32_t i = all ? 0 : hit; i < (all ? n : hit + 1); i++) acquire(obj(MEM32(hs + 4 * i)));
            pthread_mutex_unlock(&obj_mx);
            return all ? 0 : hit;
        }
        if (ms == 0 || (ms != 0xFFFFFFFFu && pthread_cond_timedwait(&obj_cv, &obj_mx, &ts) == ETIMEDOUT)) break;
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&obj_cv, &obj_mx);
    }
    pthread_mutex_unlock(&obj_mx);
    return WAIT_TIMEOUT;
}

WINAPI(CreateSemaphoreA, "CreateSemaphoreA", 4) {
    uint32_t h = new_handle(H_SEM);
    obj(h)->count = ARG(1);
    obj(h)->max = ARG(2);
    return h;
}
WINAPI(ReleaseSemaphore, "ReleaseSemaphore", 3) {
    Obj *o = obj(ARG(0));
    if (!o) return 0;
    pthread_mutex_lock(&obj_mx);
    if (ARG(2)) MEM32(ARG(2)) = o->count;
    int ok = o->count + (int)ARG(1) <= o->max;
    if (ok) o->count += ARG(1);
    pthread_cond_broadcast(&obj_cv);
    pthread_mutex_unlock(&obj_mx);
    return ok;
}

WINAPI(CreateEventA, "CreateEventA", 4) {
    uint32_t h = new_handle(H_EVENT);
    obj(h)->manual = ARG(1);
    obj(h)->signaled = ARG(2);
    return h;
}
WINAPI(SetEvent, "SetEvent", 1) { Obj *o = obj(ARG(0)); if (o) signal_obj(o, 1); return o != NULL; }
WINAPI(ResetEvent, "ResetEvent", 1) { Obj *o = obj(ARG(0)); if (o) signal_obj(o, 0); return o != NULL; }
/* Frees the threads waiting right now (one, for an auto-reset event) and leaves the event reset.
 * Setting and resetting it instead loses the pulse whenever the reset wins the race with the woken
 * waiter: CRI's ADX server thread waits on one, pulsed by a 17 ms timer, and missing pulses left the
 * music silent, or looping its stream buffer. */
WINAPI(PulseEvent, "PulseEvent", 1) {
    Obj *o = obj(ARG(0));
    if (!o) return 0;
    pthread_mutex_lock(&obj_mx);
    if (o->waiters) {
        o->pulse++;
        o->pulsed = o->manual ? o->waiters : 1;
        pthread_cond_broadcast(&obj_cv);
    }
    o->signaled = 0;
    pthread_mutex_unlock(&obj_mx);
    return 1;
}

WINAPI(CreateMutexA, "CreateMutexA", 3) {
    uint32_t h = new_handle(H_MUTEX);
    if (ARG(1)) { obj(h)->owner = host_thread_self(); obj(h)->count = 1; }
    return h;
}
WINAPI(ReleaseMutex, "ReleaseMutex", 1) {
    Obj *o = obj(ARG(0));
    if (!o || !o->count) return 0;
    pthread_mutex_lock(&obj_mx);
    o->count--;
    pthread_cond_broadcast(&obj_cv);
    pthread_mutex_unlock(&obj_mx);
    return 1;
}

/* CRITICAL_SECTION lives in guest memory; its DebugInfo field holds our recursive mutex index + 1. */
static pthread_mutex_t cs_mx[2048];
static uint32_t n_cs;
static pthread_mutex_t *cs_get(uint32_t cs) {
    if (!MEM32(cs)) {
        pthread_mutex_lock(&obj_mx);
        if (!MEM32(cs)) {
            if (n_cs == 2048) rt_fatal("too many critical sections");
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&cs_mx[n_cs], &a);
            MEM32(cs) = ++n_cs;
        }
        pthread_mutex_unlock(&obj_mx);
    }
    return &cs_mx[MEM32(cs) - 1];
}
WINAPI(InitializeCriticalSection, "InitializeCriticalSection", 1) { MEM32(ARG(0)) = 0; cs_get(ARG(0)); return 0; }
WINAPI(InitializeCriticalSectionAndSpinCount, "InitializeCriticalSectionAndSpinCount", 2) { MEM32(ARG(0)) = 0; cs_get(ARG(0)); return 1; }
WINAPI(DeleteCriticalSection, "DeleteCriticalSection", 1) { return 0; }
WINAPI(EnterCriticalSection, "EnterCriticalSection", 1) { pthread_mutex_lock(cs_get(ARG(0))); return 0; }
WINAPI(LeaveCriticalSection, "LeaveCriticalSection", 1) { pthread_mutex_unlock(cs_get(ARG(0))); return 0; }

WINAPI(InterlockedExchange, "InterlockedExchange", 2) {
    return __atomic_exchange_n((uint32_t *)GPTR(ARG(0)), ARG(1), __ATOMIC_SEQ_CST);
}
WINAPI(InterlockedIncrement, "InterlockedIncrement", 1) { return __atomic_add_fetch((uint32_t *)GPTR(ARG(0)), 1, __ATOMIC_SEQ_CST); }
WINAPI(InterlockedDecrement, "InterlockedDecrement", 1) { return __atomic_sub_fetch((uint32_t *)GPTR(ARG(0)), 1, __ATOMIC_SEQ_CST); }
WINAPI(InterlockedExchangeAdd, "InterlockedExchangeAdd", 2) { return __atomic_fetch_add((uint32_t *)GPTR(ARG(0)), ARG(1), __ATOMIC_SEQ_CST); }
WINAPI(InterlockedCompareExchange, "InterlockedCompareExchange", 3) {
    uint32_t expected = ARG(2);
    __atomic_compare_exchange_n((uint32_t *)GPTR(ARG(0)), &expected, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;  /* the initial value either way */
}

/* ---- threads ---- */

static uint32_t next_tid = 0x100;

static void setup_guest_thread(uint32_t stack_size, uint32_t tid) {
    uint32_t stack = galloc(stack_size), tib = galloc(0x1000);
    MEM32(tib + 0x00) = 0xFFFFFFFFu;            /* SEH chain end */
    MEM32(tib + 0x04) = stack + stack_size;     /* stack base */
    MEM32(tib + 0x08) = stack;                  /* stack limit */
    MEM32(tib + 0x18) = tib;                    /* self */
    MEM32(tib + 0x20) = 0x1234;                 /* process id */
    MEM32(tib + 0x24) = tid;
    g_fs_base = tib;
    g_esp = stack + stack_size - 64;
    g_fp_top = 0;
    g_fp_control_word = 0x027F;
}

static void thread_started(Obj *o) {
    o->thread = host_thread_self();
    o->started = 1;
}

#ifndef __SWITCH__
/* SH3_DUMP_AFTER=<seconds>: every guest thread prints where it is, for hangs (Linux only). */
static void on_dump(int sig) {
    (void)sig;
    fprintf(stderr, "=== thread %u (handle %X)\n", MEM32(g_fs_base + 0x24), self_handle);
    rt_dump_thread();
}
static void dump_watchdog(void *arg) {
    sleep((unsigned)(uintptr_t)arg);
    for (int i = 1; i < 4096; i++)
        if (objs[i] && objs[i]->type == H_THREAD && objs[i]->started && !objs[i]->signaled) {
            host_thread_signal(objs[i]->thread, SIGUSR1);
            usleep(300000);
        }
}
#endif

/* Called on the thread that runs the game's entry point. */
void rt_thread_init_main(uint32_t stack_size) {
#ifndef __SWITCH__
    signal(SIGUSR1, on_dump);
    if (getenv("SH3_DUMP_AFTER"))
        host_thread_start(dump_watchdog, (void *)(uintptr_t)atoi(getenv("SH3_DUMP_AFTER")), 1u << 16);
#endif
    setup_guest_thread(stack_size, next_tid++);
    self_handle = new_handle(H_THREAD);
    obj(self_handle)->tid = MEM32(g_fs_base + 0x24);
    thread_started(obj(self_handle));
}

typedef struct { void (*fn)(void *); void *arg; uint32_t stack_size, handle; } Spawn;

static void spawn_main(void *p) {
    Spawn s = *(Spawn *)p;
    free(p);
    Obj *o = obj(s.handle);
    self_handle = s.handle;
    setup_guest_thread(s.stack_size, o->tid);
    pthread_mutex_lock(&obj_mx);
    while (o->suspended) pthread_cond_wait(&obj_cv, &obj_mx);  /* CREATE_SUSPENDED */
    thread_started(o);
    pthread_mutex_unlock(&obj_mx);
    s.fn(s.arg);
    signal_obj(o, 1);
}

static uint32_t spawn(void (*fn)(void *), void *arg, uint32_t stack_size, int suspended) {
    uint32_t h = new_handle(H_THREAD);
    Obj *o = obj(h);
    o->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_SEQ_CST);
    o->suspended = suspended;
    o->exit_code = 0x103;  /* STILL_ACTIVE */
    Spawn *s = malloc(sizeof *s);
    *s = (Spawn){fn, arg, stack_size ? stack_size : 1u << 20, h};
    host_thread_start(spawn_main, s, 8u << 20);  /* host stack: lifted code recurses as deep as the guest */
    return h;
}

void rt_spawn(void (*fn)(void *), void *arg, uint32_t stack_size) { spawn(fn, arg, stack_size, 0); }

typedef struct { uint32_t start, param; int stdcall; } GuestStart;
static void guest_thread(void *p) {
    GuestStart g = *(GuestStart *)p;
    free(p);
    uint32_t code = guest_call(g.start, 1, &g.param, g.stdcall);
    obj(self_handle)->exit_code = code;
}

WINAPI(CreateThread, "CreateThread", 6) {
    GuestStart *g = malloc(sizeof *g);
    *g = (GuestStart){ARG(2), ARG(3), 1};
    uint32_t h = spawn(guest_thread, g, ARG(1), (ARG(4) & 4) != 0);  /* CREATE_SUSPENDED */
    if (ARG(5)) MEM32(ARG(5)) = obj(h)->tid;
    return h;
}

WINAPI(ExitThread, "ExitThread", 1) {
    obj(self_handle)->exit_code = ARG(0);
    signal_obj(obj(self_handle), 1);
    host_thread_exit();
}
WINAPI(GetExitCodeThread, "GetExitCodeThread", 2) {
    Obj *o = obj(ARG(0));
    if (!o) return 0;
    MEM32(ARG(1)) = o->signaled ? o->exit_code : 0x103;
    return 1;
}
WINAPI(ResumeThread, "ResumeThread", 1) {
    Obj *o = obj(ARG(0));
    if (!o) return 0xFFFFFFFFu;
    pthread_mutex_lock(&obj_mx);
    uint32_t prev = o->suspended;
    int wake = prev && --o->suspended == 0 && o->started;
    pthread_cond_broadcast(&obj_cv);  /* threads created suspended wait on the condition */
    pthread_mutex_unlock(&obj_mx);
    if (wake) host_thread_resume(o->thread);
    return prev;
}
WINAPI(SuspendThread, "SuspendThread", 1) {
    Obj *o = obj(ARG(0));
    if (!o) return 0xFFFFFFFFu;
    pthread_mutex_lock(&obj_mx);
    uint32_t prev = o->suspended++;
    int park = !prev && o->started && !o->signaled;
    pthread_mutex_unlock(&obj_mx);
    if (park) host_thread_pause(o->thread);
    return prev;
}
WINAPI(GetCurrentThread, "GetCurrentThread", 0) { return 0xFFFFFFFEu; }
WINAPI(GetCurrentThreadId, "GetCurrentThreadId", 0) { return MEM32(g_fs_base + 0x24); }
WINAPI(GetCurrentProcessId, "GetCurrentProcessId", 0) { return 0x1234; }
WINAPI(SetThreadPriority, "SetThreadPriority", 2) { return 1; }
WINAPI(GetThreadPriority, "GetThreadPriority", 1) { return 0; }
WINAPI(SetThreadPriorityBoost, "SetThreadPriorityBoost", 2) { return 1; }
WINAPI(SetThreadAffinityMask, "SetThreadAffinityMask", 2) { return 1; }
WINAPI(SetPriorityClass, "SetPriorityClass", 2) { return 1; }
WINAPI(GetCurrentProcess, "GetCurrentProcess", 0) { return 0xFFFFFFFFu; }
/* The usual use: a real handle for GetCurrentThread's pseudo handle. */
WINAPI(DuplicateHandle, "DuplicateHandle", 7) {
    MEM32(ARG(3)) = ARG(1) == 0xFFFFFFFEu ? self_handle : ARG(1);
    return 1;
}

/* ---- thread local storage: TlsAlloc indices, a value per host thread ---- */
static __thread uint32_t tls_slot[64];
static uint64_t tls_used;
WINAPI(TlsAlloc, "TlsAlloc", 0) {
    for (uint32_t i = 0; i < 64; i++)
        if (!(__atomic_fetch_or(&tls_used, 1ull << i, __ATOMIC_SEQ_CST) & (1ull << i))) return i;
    return 0xFFFFFFFFu;
}
WINAPI(TlsFree, "TlsFree", 1) { __atomic_fetch_and(&tls_used, ~(1ull << (ARG(0) & 63)), __ATOMIC_SEQ_CST); return 1; }
WINAPI(TlsGetValue, "TlsGetValue", 1) { last_error = 0; return tls_slot[ARG(0) & 63]; }
WINAPI(TlsSetValue, "TlsSetValue", 2) { tls_slot[ARG(0) & 63] = ARG(1); return 1; }
/* Sleep(0) sleeps 0.1 ms rather than yielding: SH2's frame limiters spun on it until a
 * QueryPerformanceCounter deadline, which otherwise kept two cores busy doing nothing. */
WINAPI(Sleep, "Sleep", 1) {
    usleep(ARG(0) ? ARG(0) * 1000u : 100);
    return 0;
}

/* ---- time ---- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
WINAPI(GetTickCount, "GetTickCount", 0) { return (uint32_t)(now_ns() / 1000000); }
WINAPI(timeGetTime, "timeGetTime", 0) { return (uint32_t)(now_ns() / 1000000); }
WINAPI(timeBeginPeriod, "timeBeginPeriod", 1) { return 0; }
WINAPI(timeEndPeriod, "timeEndPeriod", 1) { return 0; }
WINAPI(QueryPerformanceFrequency, "QueryPerformanceFrequency", 1) {
    MEM32(ARG(0)) = 1000000000u;
    MEM32(ARG(0) + 4) = 0;
    return 1;
}
WINAPI(QueryPerformanceCounter, "QueryPerformanceCounter", 1) {
    uint64_t t = now_ns();
    MEM32(ARG(0)) = (uint32_t)t;
    MEM32(ARG(0) + 4) = (uint32_t)(t >> 32);
    return 1;
}

/* FILETIME: 100 ns ticks since 1601. SYSTEMTIME: 8 WORDs. */
static uint64_t filetime_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ull + ts.tv_nsec / 100 + 116444736000000000ull;
}
/* timegm, which newlib lacks: days from 1970-01-01 for a proleptic Gregorian date. */
static time_t utc_seconds(const struct tm *t) {
    int y = t->tm_year + 1900 - (t->tm_mon < 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int doy = (153 * (t->tm_mon + (t->tm_mon < 2 ? 10 : -2)) + 2) / 5 + t->tm_mday - 1;
    long days = era * 146097L + (yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    return days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static void put_systemtime(uint32_t st, time_t t, uint32_t ms) {
    struct tm tm;
    gmtime_r(&t, &tm);
    uint16_t w[8] = {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_wday, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms};
    memcpy(GPTR(st), w, 16);
}
WINAPI(GetSystemTimeAsFileTime, "GetSystemTimeAsFileTime", 1) {
    uint64_t t = filetime_now();
    MEM32(ARG(0)) = (uint32_t)t;
    MEM32(ARG(0) + 4) = (uint32_t)(t >> 32);
    return 0;
}
WINAPI(GetLocalTime, "GetLocalTime", 1) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    put_systemtime(ARG(0), utc_seconds(&tm), ts.tv_nsec / 1000000);
    return 0;
}
WINAPI(FileTimeToSystemTime, "FileTimeToSystemTime", 2) {
    uint64_t t = MEM32(ARG(0)) | (uint64_t)MEM32(ARG(0) + 4) << 32;
    put_systemtime(ARG(1), (time_t)((t - 116444736000000000ull) / 10000000), (t / 10000) % 1000);
    return 1;
}
/* The game stamps a save with the local time (GetLocalTime) and compares it with the file's time after this
 * conversion, so the console's UTC offset has to be applied here. */
WINAPI(FileTimeToLocalFileTime, "FileTimeToLocalFileTime", 2) {
    uint64_t t = MEM32(ARG(0)) | (uint64_t)MEM32(ARG(0) + 4) << 32;
    time_t utc = (time_t)((t - 116444736000000000ull) / 10000000);
    struct tm tm;
    localtime_r(&utc, &tm);
    t += (uint64_t)((int64_t)(utc_seconds(&tm) - utc) * 10000000);
    MEM32(ARG(1)) = (uint32_t)t;
    MEM32(ARG(1) + 4) = (uint32_t)(t >> 32);
    return 1;
}
WINAPI(SystemTimeToFileTime, "SystemTimeToFileTime", 2) {
    uint16_t w[8];
    memcpy(w, GPTR(ARG(0)), 16);
    struct tm tm = {.tm_year = w[0] - 1900, .tm_mon = w[1] - 1, .tm_mday = w[3],
                    .tm_hour = w[4], .tm_min = w[5], .tm_sec = w[6]};
    uint64_t t = (uint64_t)utc_seconds(&tm) * 10000000ull + w[7] * 10000ull + 116444736000000000ull;
    MEM32(ARG(1)) = (uint32_t)t;
    MEM32(ARG(1) + 4) = (uint32_t)(t >> 32);
    return 1;
}

/* timeSetEvent: callbacks void CALLBACK proc(id, msg, user, dw1, dw2), all run by one timer thread
 * (the game re-arms a one-shot timer every frame). */
typedef struct { uint32_t delay, proc, user, periodic; uint64_t due; int live; } MmTimer;
static MmTimer mm_timers[64];
static pthread_mutex_t mm_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mm_cv = PTHREAD_COND_INITIALIZER;
static int mm_thread_up;

static void mm_timer_thread(void *unused) {
    (void)unused;
    pthread_mutex_lock(&mm_mx);
    for (;;) {
        uint64_t now = now_ns(), next = now + 100000000ull;
        for (uint32_t i = 1; i < 64; i++) {
            MmTimer *t = &mm_timers[i];
            if (!t->live) continue;
            if (t->due <= now) {
                uint32_t a[5] = {i, 0, t->user, 0, 0}, proc = t->proc;
                if (t->periodic) t->due += t->delay * 1000000ull; else t->live = 0;
                pthread_mutex_unlock(&mm_mx);
                guest_call(proc, 5, a, 1);
                pthread_mutex_lock(&mm_mx);
                now = now_ns();
            }
            if (t->live && t->due < next) next = t->due;
        }
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t wait = next > now ? next - now : 0, abs_ns = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec + wait;
        ts.tv_sec = abs_ns / 1000000000ull;
        ts.tv_nsec = abs_ns % 1000000000ull;
        pthread_cond_timedwait(&mm_cv, &mm_mx, &ts);
    }
}
WINAPI(timeSetEvent, "timeSetEvent", 5) {
    pthread_mutex_lock(&mm_mx);
    if (!mm_thread_up) {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&mm_cv, &ca);
        mm_thread_up = 1;
        rt_spawn(mm_timer_thread, NULL, 256u << 10);
    }
    uint32_t id = 0;
    for (uint32_t i = 1; i < 64 && !id; i++)
        if (!mm_timers[i].live) {
            uint32_t d = ARG(0) ? ARG(0) : 1;
            mm_timers[i] = (MmTimer){d, ARG(2), ARG(3), ARG(4) & 1, now_ns() + d * 1000000ull, 1};
            id = i;
        }
    pthread_cond_signal(&mm_cv);
    pthread_mutex_unlock(&mm_mx);
    return id;
}
WINAPI(timeKillEvent, "timeKillEvent", 1) {
    pthread_mutex_lock(&mm_mx);
    uint32_t ok = ARG(0) && ARG(0) < 64 && mm_timers[ARG(0)].live;
    if (ok) mm_timers[ARG(0)].live = 0;
    pthread_mutex_unlock(&mm_mx);
    return ok ? 0 : 97;  /* MMSYSERR_INVALPARAM */
}

/* ---- files ---- */

/* Save folder activity is always logged (sh3.log), so a damaged-save report can be read afterwards. */
static int has_save(const char *s) {
    for (; *s; s++)
        if (!strncasecmp(s, "save", 4)) return 1;
    return 0;
}

static uint32_t open_file(const char *guest, uint32_t access, uint32_t disposition) {
    char path[512];
    rt_path(guest, path, sizeof path);
    int write = (access & 0x40000000u) != 0;  /* GENERIC_WRITE */
    struct stat st;
    int exists = stat(path, &st) == 0;
    /* SH3 empties savedata/disp.ini at start and fills it on a clean exit; found empty, it means "crashed" and
     * the game falls back to safe graphics. Leaving with HOME (or killing a test run) is not a crash. */
    if (exists && !st.st_size && !write && !strcasecmp(path + (strlen(path) > 8 ? strlen(path) - 8 : 0), "disp.ini")) {
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE;
    }
    const char *mode = write ? "r+b" : "rb";
    switch (disposition) {
    case 1: if (exists) { last_error = 80; return INVALID_HANDLE; } mode = "w+b"; break;  /* CREATE_NEW */
    case 2: mode = "w+b"; break;                                                           /* CREATE_ALWAYS */
    case 4: if (!exists) mode = "w+b"; break;                                              /* OPEN_ALWAYS */
    case 5: mode = "w+b"; break;                                                           /* TRUNCATE_EXISTING */
    }
    if (exists && S_ISDIR(st.st_mode)) { last_error = 5; return INVALID_HANDLE; }
    FILE *f = fopen(path, mode);
    if (rt_trace) rt_log("  open %s -> %s %s", guest, path, f ? "ok" : "FAILED");
    if (has_save(guest)) rt_log("save: open %s access %08X disposition %u -> %s", guest, access, disposition, f ? "ok" : "FAILED");
    if (!f && !write) rt_log("open: %s not found (%s)", guest, path);  /* the game's missing files */
    if (!f) { last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE; }
    uint32_t h = new_handle(H_FILE);
    obj(h)->f = f;
    return h;
}
WINAPI(CreateFileA, "CreateFileA", 7) { return open_file(GSTR(ARG(0)), ARG(1), ARG(4)); }
WINAPI(CreateFileW, "CreateFileW", 7) {
    char s[512];
    size_t i = 0;
    for (uint16_t *w = GPTR(ARG(0)); w[i] && i < sizeof s - 1; i++) s[i] = (char)w[i];
    s[i] = 0;
    return open_file(s, ARG(1), ARG(4));
}

/* OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent. Reads complete before returning. */
static uint32_t file_io(int write) {
    Obj *o = obj(ARG(0));
    uint32_t ov = ARG(4);
    if (!o || !o->f) { last_error = 6; return 0; }
    if (ov) fseeko(o->f, MEM32(ov + 8) | (off_t)MEM32(ov + 12) << 32, SEEK_SET);
    off_t at = ftello(o->f);
    size_t n;
    if (write) n = fwrite(GPTR(ARG(1)), 1, ARG(2), o->f);
    else {
        /* Never ask fread for more than the file holds: at the end of the file the C library zeroed the front of
         * the destination buffer, and the game reads a save a second time at EOF (expecting 0 bytes and its
         * buffer untouched), which turned every save into "damaged folders". */
        fseeko(o->f, 0, SEEK_END);
        off_t end = ftello(o->f);
        fseeko(o->f, at, SEEK_SET);
        size_t want = ARG(2);
        if (at >= end) want = 0;
        else if ((uint64_t)(end - at) < want) want = (size_t)(end - at);
        n = want ? fread(GPTR(ARG(1)), 1, want, o->f) : 0;
    }
    if (write) fflush(o->f);
    if (ARG(3)) MEM32(ARG(3)) = (uint32_t)n;
    if (ov) {
        MEM32(ov) = 0;
        MEM32(ov + 4) = (uint32_t)n;
        if (obj(MEM32(ov + 16))) signal_obj(obj(MEM32(ov + 16)), 1);
    }
    return 1;
}
WINAPI(ReadFile, "ReadFile", 5) { return file_io(0); }
WINAPI(WriteFile, "WriteFile", 5) { return file_io(1); }
WINAPI(GetOverlappedResult, "GetOverlappedResult", 4) {
    MEM32(ARG(2)) = MEM32(ARG(1) + 4);
    return 1;
}
WINAPI(SetFilePointer, "SetFilePointer", 4) {
    Obj *o = obj(ARG(0));
    if (!o || !o->f) return 0xFFFFFFFFu;
    off_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (off_t)(ARG(1) | (uint64_t)MEM32(ARG(2)) << 32);
    fseeko(o->f, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    off_t pos = ftello(o->f);
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)(pos >> 32);
    return (uint32_t)pos;
}
WINAPI(SetEndOfFile, "SetEndOfFile", 1) {
    Obj *o = obj(ARG(0));
    if (!o || !o->f) return 0;
    fflush(o->f);
    return ftruncate(fileno(o->f), ftello(o->f)) == 0;
}
WINAPI(FlushFileBuffers, "FlushFileBuffers", 1) { Obj *o = obj(ARG(0)); if (o && o->f) fflush(o->f); return 1; }
WINAPI(GetFileAttributesA, "GetFileAttributesA", 1) {
    char p[512];
    struct stat st;
    if (stat(rt_path(GSTR(ARG(0)), p, sizeof p), &st)) { last_error = ERROR_FILE_NOT_FOUND; return 0xFFFFFFFFu; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
WINAPI(GetFileSize, "GetFileSize", 2) {
    Obj *o = obj(ARG(0));
    if (!o || !o->f) return 0xFFFFFFFFu;
    struct stat st;
    fstat(fileno(o->f), &st);
    if (ARG(1)) MEM32(ARG(1)) = (uint32_t)(st.st_size >> 32);
    return (uint32_t)st.st_size;
}

static uint32_t path_op(int (*op)(const char *)) {
    char p[512];
    return op(rt_path(GSTR(ARG(0)), p, sizeof p)) == 0;
}
static int mkdir_755(const char *p) { return mkdir(p, 0755); }
WINAPI(DeleteFileA, "DeleteFileA", 1) { return path_op(unlink); }
WINAPI(RemoveDirectoryA, "RemoveDirectoryA", 1) { return path_op(rmdir); }
WINAPI(CreateDirectoryA, "CreateDirectoryA", 2) { return path_op(mkdir_755); }
WINAPI(SetFileAttributesA, "SetFileAttributesA", 2) { return 1; }
WINAPI(CopyFileA, "CopyFileA", 3) {
    char a[512], b[512];
    FILE *in = fopen(rt_path(GSTR(ARG(0)), a, sizeof a), "rb"), *out = NULL;
    struct stat st;
    if (in && !(ARG(2) && stat(rt_path(GSTR(ARG(1)), b, sizeof b), &st) == 0))
        out = fopen(rt_path(GSTR(ARG(1)), b, sizeof b), "wb");
    char buf[65536];
    size_t n;
    while (in && out && (n = fread(buf, 1, sizeof buf, in))) fwrite(buf, 1, n, out);
    if (in) fclose(in);
    if (out) fclose(out);
    return out != NULL;
}
WINAPI(GetFullPathNameA, "GetFullPathNameA", 4) {
    const char *in = GSTR(ARG(0));
    char full[512];
    if (in[0] && in[1] == ':') snprintf(full, sizeof full, "%s", in);
    else snprintf(full, sizeof full, "C:\\SH3\\%s", in + (in[0] == '\\'));
    uint32_t n = strlen(full);
    if (n + 1 > ARG(1)) return n + 1;
    strcpy(GPTR(ARG(2)), full);
    if (ARG(3)) {
        char *slash = strrchr(GPTR(ARG(2)), '\\');
        MEM32(ARG(3)) = slash ? GVA(slash + 1) : ARG(2);
    }
    return n;
}
/* D: is the game's CD (DRIVE_CDROM): the disc check opens D:\\Movie\\Open.bik, which rt_path finds in
 * data/movie. Every other drive is fixed. */
WINAPI(GetDriveTypeA, "GetDriveTypeA", 1) {
    const char *d = ARG(0) ? GSTR(ARG(0)) : "C";
    return (d[0] == 'd' || d[0] == 'D') ? 5 : 3;
}
WINAPI(GetLogicalDrives, "GetLogicalDrives", 0) { return 0x4 | 0x8; }  /* C: and D: */
WINAPI(GetVolumeInformationA, "GetVolumeInformationA", 8) {
    if (ARG(1) && ARG(2)) snprintf(GPTR(ARG(1)), ARG(2), "SH3");
    if (ARG(3)) MEM32(ARG(3)) = 0x53483300u;
    if (ARG(4)) MEM32(ARG(4)) = 255;
    if (ARG(5)) MEM32(ARG(5)) = 0;
    if (ARG(6) && ARG(7)) snprintf(GPTR(ARG(6)), ARG(7), "NTFS");
    return 1;
}
WINAPI(GetDiskFreeSpaceExA, "GetDiskFreeSpaceExA", 4) {
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) { MEM32(ARG(i)) = 0x40000000u; MEM32(ARG(i) + 4) = 0; }
    return 1;
}

/* Windows wildcards (* and ?), ignoring case. */
static int wildmatch(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') return wildmatch(p + 1, s) || (*s && wildmatch(p, s + 1));
    return *s && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) && wildmatch(p + 1, s + 1);
}

/* WIN32_FIND_DATAA: attributes @0, size @28/@32, cFileName @44. */
static void find_entry(uint32_t fd, const char *name, int dir, uint64_t size, time_t mtime) {
    memset(GPTR(fd), 0, 320);
    MEM32(fd) = dir ? 0x10 : 0x80;
    uint64_t ft = ((uint64_t)mtime + 11644473600ull) * 10000000ull;  /* creation, access, write: the file's mtime */
    for (int i = 0; i < 3; i++) {
        MEM32(fd + 4 + 8 * i) = (uint32_t)ft;
        MEM32(fd + 8 + 8 * i) = (uint32_t)(ft >> 32);
    }
    MEM32(fd + 28) = (uint32_t)(size >> 32);
    MEM32(fd + 32) = (uint32_t)size;
    snprintf((char *)GPTR(fd + 44), 260, "%s", name);
}

/* Windows lists "." and ".." first in any folder, and the save code counts them: a save folder is
 * whole when it holds 5 entries. Horizon's readdir has neither, so they are made up here, the same
 * on both hosts. */
static int find_fill(Obj *o, uint32_t fd) {
    while (o->dir && o->dots < 2) {
        const char *dot = o->dots++ ? ".." : ".";
        if (wildmatch(o->pattern, dot)) { find_entry(fd, dot, 1, 0, time(NULL)); return 1; }
    }
    struct dirent *e;
    while (o->dir && (e = readdir(o->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !wildmatch(o->pattern, e->d_name)) continue;
        char p[1024];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", o->dirpath, e->d_name);
        if (stat(rt_path(p, p, sizeof p), &st)) continue;  /* the size of what opening it reads (EE music) */
        find_entry(fd, e->d_name, S_ISDIR(st.st_mode), (uint64_t)st.st_size, st.st_mtime);
        if (has_save(o->dirpath)) rt_log("save: find %s [%s] -> %s%s %lld", o->dirpath, o->pattern, e->d_name, S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size);
        if (rt_trace) rt_log("  find %s [%s] -> %s%s %lld mtime %lld", o->dirpath, o->pattern, e->d_name, S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size, (long long)st.st_mtime);
        return 1;
    }
    last_error = ERROR_NO_MORE_FILES;
    return 0;
}
WINAPI(FindFirstFileA, "FindFirstFileA", 2) {
    char guest[512], *slash;
    snprintf(guest, sizeof guest, "%s", GSTR(ARG(0)));
    for (char *c = guest; *c; c++) if (*c == '\\') *c = '/';
    uint32_t h = new_handle(H_FIND);
    Obj *o = obj(h);
    if ((slash = strrchr(guest, '/'))) {
        *slash = 0;
        rt_path(guest, o->dirpath, sizeof o->dirpath);
        snprintf(o->pattern, sizeof o->pattern, "%s", slash + 1);
    } else {
        strcpy(o->dirpath, ".");
        snprintf(o->pattern, sizeof o->pattern, "%s", guest);
    }
    if (!strcmp(o->pattern, "*.*")) strcpy(o->pattern, "*");
    o->dir = opendir(o->dirpath);
    if (rt_trace) rt_log("  FindFirstFile %s [%s] %s", o->dirpath, o->pattern, o->dir ? "" : "(no such folder)");
    if (find_fill(o, ARG(1))) return h;
    close_handle(h);
    last_error = ERROR_FILE_NOT_FOUND;
    return INVALID_HANDLE;
}
WINAPI(FindNextFileA, "FindNextFileA", 2) { Obj *o = obj(ARG(0)); return o && find_fill(o, ARG(1)); }
WINAPI(FindClose, "FindClose", 1) { return close_handle(ARG(0)); }

/* File mappings are copies: MapViewOfFile reads the file into guest memory. */
WINAPI(CreateFileMappingA, "CreateFileMappingA", 6) {
    uint32_t h = new_handle(H_MAPPING);
    obj(h)->map_file = ARG(0);
    return h;
}
WINAPI(MapViewOfFile, "MapViewOfFile", 5) {
    Obj *m = obj(ARG(0)), *f = m ? obj(m->map_file) : NULL;
    if (!f || !f->f) return 0;
    struct stat st;
    fstat(fileno(f->f), &st);
    off_t off = ARG(3) | (off_t)ARG(2) << 32;
    uint32_t n = ARG(4) ? ARG(4) : (uint32_t)(st.st_size - off);
    uint32_t va = galloc(n);
    fseeko(f->f, off, SEEK_SET);
    if (fread(GPTR(va), 1, n, f->f) != n) rt_log("MapViewOfFile: short read");
    return va;
}
WINAPI(UnmapViewOfFile, "UnmapViewOfFile", 1) { gfree(ARG(0)); return 1; }

/* ---- WINMM mmio: RIFF reading (DirectX's CWaveFile) ----
 * A file is read whole into guest memory, so it reads like a 'MEM ' buffer; both are one I/O buffer that
 * mmioGetInfo hands out. ponytail: read only, 64 open at once; mmioWrite/mmioCreateChunk stop the port. */
typedef struct { uint32_t base, size, pos, owned; } Mmio;
static Mmio mmios[64];
#define MMIO_OF(h) ((h) && (h) < 64 && mmios[h].base ? &mmios[h] : NULL)

WINAPI(mmioOpenA, "mmioOpenA", 3) {
    uint32_t info = ARG(1), base = 0, size = 0, owned = 0;
    if (info && MEM32(info + 4) == 0x204D454Du) {  /* 'MEM ': cchBuffer, pchBuffer */
        size = MEM32(info + 20);
        base = MEM32(info + 24);
    } else if (ARG(0)) {
        char p[512];
        FILE *f = fopen(rt_path(GSTR(ARG(0)), p, sizeof p), "rb");
        if (rt_trace) rt_log("  mmioOpen %s -> %s", GSTR(ARG(0)), f ? "ok" : "FAILED");
        if (!f) { if (info) MEM32(info + 12) = 257; return 0; }  /* MMIOERR_CANNOTOPEN */
        fseek(f, 0, SEEK_END);
        size = (uint32_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        base = galloc(size ? size : 1);
        owned = 1;
        if (fread(GPTR(base), 1, size, f) != size) rt_log("mmioOpen: short read");
        fclose(f);
    }
    for (uint32_t h = 1; base && h < 64; h++)
        if (!mmios[h].base) { mmios[h] = (Mmio){base, size, 0, owned}; return h; }
    if (owned) gfree(base);
    return 0;
}
WINAPI(mmioClose, "mmioClose", 2) {
    Mmio *m = MMIO_OF(ARG(0));
    if (m && m->owned) gfree(m->base);
    if (m) m->base = 0;
    return 0;
}
WINAPI(mmioRead, "mmioRead", 3) {
    Mmio *m = MMIO_OF(ARG(0));
    if (!m) return 0xFFFFFFFFu;
    uint32_t n = ARG(2) < m->size - m->pos ? ARG(2) : m->size - m->pos;
    memcpy(GPTR(ARG(1)), GPTR(m->base + m->pos), n);
    m->pos += n;
    return n;
}
WINAPI(mmioSeek, "mmioSeek", 3) {
    Mmio *m = MMIO_OF(ARG(0));
    if (!m) return 0xFFFFFFFFu;
    int64_t to = (int32_t)ARG(1) + (ARG(2) == 1 ? (int64_t)m->pos : ARG(2) == 2 ? (int64_t)m->size : 0);
    if (to < 0 || to > m->size) return 0xFFFFFFFFu;
    return m->pos = (uint32_t)to;
}
/* MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags. FIND flags: 0x10 chunk, 0x20 RIFF, 0x40 LIST. */
WINAPI(mmioDescend, "mmioDescend", 4) {
    Mmio *m = MMIO_OF(ARG(0));
    uint32_t ck = ARG(1), parent = ARG(2), flags = ARG(3);
    if (!m) return 5;  /* MMSYSERR_INVALHANDLE */
    uint32_t end = parent ? MEM32(parent + 12) + MEM32(parent + 4) : m->size, want = MEM32(ck), want_type = MEM32(ck + 8);
    for (uint32_t pos = m->pos; pos + 8 <= end && pos + 8 <= m->size;) {
        uint32_t id = MEM32(m->base + pos), size = MEM32(m->base + pos + 4), list = id == 0x46464952u || id == 0x5453494Cu;
        uint32_t type = list && pos + 12 <= m->size ? MEM32(m->base + pos + 8) : 0;
        int hit = (flags & 0x20) ? id == 0x46464952u && type == want_type
                : (flags & 0x40) ? id == 0x5453494Cu && type == want_type
                : (flags & 0x10) ? id == want : 1;
        if (hit) {
            MEM32(ck) = id;
            MEM32(ck + 4) = size;
            MEM32(ck + 8) = type;
            MEM32(ck + 12) = pos + 8;
            MEM32(ck + 16) = 0;
            m->pos = pos + 8 + (list ? 4 : 0);
            return 0;
        }
        pos += 8 + size + (size & 1);
    }
    return 265;  /* MMIOERR_CHUNKNOTFOUND */
}
WINAPI(mmioAscend, "mmioAscend", 3) {
    Mmio *m = MMIO_OF(ARG(0));
    if (!m) return 5;
    uint32_t end = MEM32(ARG(1) + 12) + MEM32(ARG(1) + 4);
    m->pos = (end + (end & 1)) < m->size ? end + (end & 1) : m->size;
    return 0;
}
/* MMIOINFO: cchBuffer @20, pchBuffer @24, pchNext @28, pchEndRead @32, pchEndWrite @36, lBufOffset @40,
 * lDiskOffset @44, hmmio @68. The whole file is the buffer, so mmioAdvance has nothing more to read. */
WINAPI(mmioGetInfo, "mmioGetInfo", 3) {
    Mmio *m = MMIO_OF(ARG(0));
    if (!m) return 5;
    uint32_t i = ARG(1);
    memset(GPTR(i), 0, 72);
    MEM32(i + 20) = m->size;
    MEM32(i + 24) = m->base;
    MEM32(i + 28) = m->base + m->pos;
    MEM32(i + 32) = MEM32(i + 36) = m->base + m->size;
    MEM32(i + 44) = m->size;
    MEM32(i + 68) = ARG(0);
    return 0;
}
WINAPI(mmioSetInfo, "mmioSetInfo", 3) {
    Mmio *m = MMIO_OF(ARG(0));
    if (!m) return 5;
    m->pos = MEM32(ARG(1) + 28) - m->base;
    return 0;
}
WINAPI(mmioAdvance, "mmioAdvance", 3) { return MMIO_OF(ARG(0)) ? 0 : 5; }

/* ---- memory ---- */
WINAPI(HeapCreate, "HeapCreate", 3) { return 0x00F00000u; }
WINAPI(HeapDestroy, "HeapDestroy", 1) { return 1; }
WINAPI(HeapAlloc, "HeapAlloc", 3) { return galloc(ARG(2)); }
WINAPI(HeapFree, "HeapFree", 3) { gfree(ARG(2)); return 1; }
WINAPI(HeapReAlloc, "HeapReAlloc", 4) { return grealloc(ARG(2), ARG(3)); }
WINAPI(HeapSize, "HeapSize", 3) { return gsize(ARG(2)); }
WINAPI(GetProcessHeap, "GetProcessHeap", 0) { return 0x00F00000u; }

/* Reserving commits too (the guest heap is zeroed memory already), so a later MEM_COMMIT inside the block
 * has nothing to do. Blocks are page aligned; the heap block behind each is remembered for MEM_RELEASE.
 * ponytail: linear table of 1024 live blocks; a map if the game keeps more. */
static uint32_t va_block[1024][2];  /* aligned address, heap block */
WINAPI(VirtualAlloc, "VirtualAlloc", 4) {
    if (ARG(0)) return ARG(0) & ~0xFFFu;
    uint32_t raw = galloc(ARG(1) + 0x1000), va = (raw + 0xFFF) & ~0xFFFu;
    pthread_mutex_lock(&obj_mx);
    for (int i = 0; i < 1024; i++)
        if (!va_block[i][0]) { va_block[i][0] = va; va_block[i][1] = raw; break; }
    pthread_mutex_unlock(&obj_mx);
    return va;
}
WINAPI(VirtualFree, "VirtualFree", 3) {
    if (!(ARG(2) & 0x8000)) return 1;  /* MEM_DECOMMIT: the memory stays */
    pthread_mutex_lock(&obj_mx);
    for (int i = 0; i < 1024; i++)
        if (va_block[i][0] == ARG(0)) { gfree(va_block[i][1]); va_block[i][0] = 0; break; }
    pthread_mutex_unlock(&obj_mx);
    return 1;
}
/* MEMORY_BASIC_INFORMATION for the page: committed, read-write. */
WINAPI(VirtualQuery, "VirtualQuery", 3) {
    uint32_t m = ARG(1), page = ARG(0) & ~0xFFFu, v[7] = {page, page, 4, 0x1000, 0x1000, 4, 0x20000};
    memcpy(GPTR(m), v, sizeof v);
    return sizeof v;
}
WINAPI(VirtualProtect, "VirtualProtect", 4) { if (ARG(3)) MEM32(ARG(3)) = 4; return 1; }
WINAPI(IsBadReadPtr, "IsBadReadPtr", 2) { return ARG(0) < 0x10000; }
WINAPI(IsBadWritePtr, "IsBadWritePtr", 2) { return ARG(0) < 0x10000; }
WINAPI(IsBadCodePtr, "IsBadCodePtr", 1) { return ARG(0) < 0x10000; }
WINAPI(GetSystemInfo, "GetSystemInfo", 1) {
    uint32_t v[9] = {0, 0x1000, 0x10000, 0x7FFEFFFF, 1, 1, 586, 0x10000, 6 | 0x0801u << 16};
    memcpy(GPTR(ARG(0)), v, sizeof v);
    return 0;
}
WINAPI(GlobalMemoryStatus, "GlobalMemoryStatus", 1) {
    uint32_t m = ARG(0), v[8] = {32, 10, 512u << 20, 384u << 20, 1024u << 20, 900u << 20, 2047u << 20, 1800u << 20};
    memcpy(GPTR(m), v, sizeof v);
    return 0;
}

/* ---- process, modules, resources ---- */
WINAPI(GetLastError, "GetLastError", 0) { return last_error; }
WINAPI(SetLastError, "SetLastError", 1) { last_error = ARG(0); return 0; }
WINAPI(TerminateProcess, "TerminateProcess", 2) { rt_log("TerminateProcess(%u)", ARG(1)); exit(ARG(1)); }
WINAPI(FatalAppExitA, "FatalAppExitA", 2) { rt_fatal("FatalAppExit: %s", GSTR(ARG(1))); }
WINAPI(SetUnhandledExceptionFilter, "SetUnhandledExceptionFilter", 1) { return 0; }
WINAPI(UnhandledExceptionFilter, "UnhandledExceptionFilter", 1) { return 1; }
WINAPI(SetConsoleCtrlHandler, "SetConsoleCtrlHandler", 2) { return 1; }
WINAPI(SetErrorMode, "SetErrorMode", 1) { return 0; }
WINAPI(FreeLibrary, "FreeLibrary", 1) { return 1; }

/* The static C runtime's start: command line, environment (empty) and standard handles (none: GetFileType
 * reports FILE_TYPE_UNKNOWN, so the CRT leaves stdin/stdout/stderr closed). */
WINAPI(GetCommandLineA, "GetCommandLineA", 0) {
    static uint32_t s;
    if (!s) s = gstrdup("sh3.exe");
    return s;
}
WINAPI(GetEnvironmentStrings, "GetEnvironmentStrings", 0) { return galloc(4); }
WINAPI(GetEnvironmentStringsW, "GetEnvironmentStringsW", 0) { return galloc(4); }
WINAPI(FreeEnvironmentStringsA, "FreeEnvironmentStringsA", 1) { return 1; }
WINAPI(FreeEnvironmentStringsW, "FreeEnvironmentStringsW", 1) { return 1; }
WINAPI(SetEnvironmentVariableA, "SetEnvironmentVariableA", 2) { return 1; }
WINAPI(GetStdHandle, "GetStdHandle", 1) { return 0xFFFFFFFFu; }
WINAPI(SetStdHandle, "SetStdHandle", 2) { return 1; }
WINAPI(GetFileType, "GetFileType", 1) { Obj *o = obj(ARG(0)); return o && o->f ? 1 : 0; }  /* FILE_TYPE_DISK */
WINAPI(SetHandleCount, "SetHandleCount", 1) { return ARG(0); }
WINAPI(GetModuleFileNameA, "GetModuleFileNameA", 3) {
    snprintf(GPTR(ARG(1)), ARG(2), "C:\\SH3\\sh3.exe");
    return strlen(GSTR(ARG(1)));
}
WINAPI(GetCurrentDirectoryA, "GetCurrentDirectoryA", 2) {
    if (ARG(0) < 8) return 8;
    strcpy(GPTR(ARG(1)), "C:\\SH3");
    return 6;
}
WINAPI(SetCurrentDirectoryA, "SetCurrentDirectoryA", 1) { return 1; }
WINAPI(ExitProcess, "ExitProcess", 1) { rt_log("ExitProcess(%u)", ARG(0)); exit(ARG(0)); }
WINAPI(OutputDebugStringA, "OutputDebugStringA", 1) { rt_log("debug: %s", GSTR(ARG(0))); return 0; }
WINAPI(IsProcessorFeaturePresent, "IsProcessorFeaturePresent", 1) { return 0; }
WINAPI(GetStartupInfoA, "GetStartupInfoA", 1) {
    memset(GPTR(ARG(0)), 0, 68);
    MEM32(ARG(0)) = 68;
    return 0;
}
WINAPI(GetVersionExA, "GetVersionExA", 1) {
    uint32_t v = ARG(0);
    MEM32(v + 4) = 5;      /* Windows XP 5.1.2600 */
    MEM32(v + 8) = 1;
    MEM32(v + 12) = 2600;
    MEM32(v + 16) = 2;     /* VER_PLATFORM_WIN32_NT */
    *(char *)GPTR(v + 20) = 0;
    return 1;
}
WINAPI(GetModuleHandleA, "GetModuleHandleA", 1) { return ARG(0) ? 0x0F000000u : 0x00400000u; }
WINAPI(LoadLibraryA, "LoadLibraryA", 1) {
    if (rt_trace) rt_log("  LoadLibraryA(%s)", GSTR(ARG(0)));
    return 0x0F000000u;
}
WINAPI(GetProcAddress, "GetProcAddress", 2) {
    if (ARG(1) < 0x10000) return 0;
    extern const Bridge __start_game_bridges[], __stop_game_bridges[];
    for (const Bridge *b = __start_game_bridges; b < __stop_game_bridges; b++)
        if (!strcmp(b->name, GSTR(ARG(1)))) return rt_host_fn(b->name, b->fn);
    if (rt_trace) rt_log("GetProcAddress(%s): not bridged", GSTR(ARG(1)));
    return 0;
}

/* Resource directory walk: type -> name -> first language. Names are UTF-16, compared ignoring case. */
static int res_ch(uint32_t s, int i, int wide) { return wide ? *(uint16_t *)GPTR(s + 2 * i) : *(uint8_t *)GPTR(s + i); }
static int res_id_matches(uint32_t entry, uint32_t rsrc, uint32_t want, int wide) {
    uint32_t id = MEM32(entry);
    if (want < 0x10000) return !(id & 0x80000000u) && id == want;
    if (!(id & 0x80000000u)) return 0;
    uint32_t s = rsrc + (id & 0x7FFFFFFF);
    uint16_t len = *(uint16_t *)GPTR(s);
    for (uint32_t i = 0; i <= len; i++) {
        int a = i < len ? *(uint16_t *)GPTR(s + 2 + 2 * i) : 0, b = res_ch(want, i, wide);
        if ((a < 128 ? tolower(a) : a) != (b < 128 ? tolower(b) : b)) return 0;
    }
    return 1;
}
static uint32_t res_find(uint32_t dir, uint32_t rsrc, uint32_t want, int wide) {
    if (want >= 0x10000 && want != 0xFFFFFFFFu && res_ch(want, 0, wide) == '#') {  /* "#123" names an id */
        uint32_t v = 0;
        for (int i = 1, c; (c = res_ch(want, i, wide)) >= '0' && c <= '9'; i++) v = v * 10 + c - '0';
        want = v;
    }
    uint32_t n = *(uint16_t *)GPTR(dir + 12) + *(uint16_t *)GPTR(dir + 14);
    for (uint32_t i = 0, e = dir + 16; i < n; i++, e += 8)
        if (want == 0xFFFFFFFFu || res_id_matches(e, rsrc, want, wide)) return MEM32(e + 4);
    return 0;
}
static uint32_t find_resource(uint32_t name, uint32_t type, int wide) {
    uint32_t rsrc = 0x400000 + MEM32(0x400000 + MEM32(0x40003C) + 24 + 96 + 16), sub;
    if (!(sub = res_find(rsrc, rsrc, type, wide)) || !(sub & 0x80000000u)) return 0;
    if (!(sub = res_find(rsrc + (sub & 0x7FFFFFFF), rsrc, name, wide)) || !(sub & 0x80000000u)) return 0;
    if (!(sub = res_find(rsrc + (sub & 0x7FFFFFFF), rsrc, 0xFFFFFFFFu, wide)) || (sub & 0x80000000u)) return 0;
    return rsrc + sub;  /* IMAGE_RESOURCE_DATA_ENTRY: OffsetToData (RVA), Size */
}
WINAPI(FindResourceA, "FindResourceA", 3) { return find_resource(ARG(1), ARG(2), 0); }
WINAPI(FindResourceW, "FindResourceW", 3) { return find_resource(ARG(1), ARG(2), 1); }
WINAPI(LoadResource, "LoadResource", 2) { return ARG(1) ? 0x400000 + MEM32(ARG(1)) : 0; }
WINAPI(LockResource, "LockResource", 1) { return ARG(0); }
WINAPI(SizeofResource, "SizeofResource", 2) { return ARG(1) ? MEM32(ARG(1) + 4) : 0; }
/* RT_STRING: block id/16 + 1 holds 16 strings, each a UTF-16 length then its characters. */
WINAPI(LoadStringA, "LoadStringA", 4) {
    uint32_t e = find_resource(ARG(1) / 16 + 1, 6, 0), n = 0;
    if (!e || !ARG(3)) return 0;
    uint32_t p = 0x400000 + MEM32(e);
    for (uint32_t i = 0; i < ARG(1) % 16; i++) p += 2 + 2 * *(uint16_t *)GPTR(p);
    uint32_t len = *(uint16_t *)GPTR(p);
    for (; n < len && n + 1 < ARG(3); n++) *(uint8_t *)GPTR(ARG(2) + n) = (uint8_t)*(uint16_t *)GPTR(p + 2 + 2 * n);
    *(uint8_t *)GPTR(ARG(2) + n) = 0;
    return n;
}
/* C++ throw (0xE06D7363) and the SEH unwinding behind it are not supported: stop with what was raised. */
WINAPI(RaiseException, "RaiseException", 4) {
    rt_fatal("RaiseException(%08X, flags %X, %u args) from %08X", ARG(0), ARG(1), ARG(2), MEM32(g_esp));
}

/* ---- strings and locale ---- */
WINAPI(lstrlenA, "lstrlenA", 1) { return ARG(0) ? strlen(GSTR(ARG(0))) : 0; }
WINAPI(lstrcpyA, "lstrcpyA", 2) { strcpy(GPTR(ARG(0)), GSTR(ARG(1))); return ARG(0); }
WINAPI(lstrcatA, "lstrcatA", 2) { strcat(GPTR(ARG(0)), GSTR(ARG(1))); return ARG(0); }
WINAPI(lstrcmpiA, "lstrcmpiA", 2) { return strcasecmp(GSTR(ARG(0)), GSTR(ARG(1))); }
WINAPI(GetACP, "GetACP", 0) { return 1252; }
WINAPI(GetOEMCP, "GetOEMCP", 0) { return 437; }
WINAPI(IsValidCodePage, "IsValidCodePage", 1) { return 1; }
WINAPI(IsValidLocale, "IsValidLocale", 2) { return 1; }
WINAPI(GetUserDefaultLCID, "GetUserDefaultLCID", 0) { return 0x409; }
WINAPI(GetUserDefaultLangID, "GetUserDefaultLangID", 0) { return 0x409; }
WINAPI(EnumSystemLocalesA, "EnumSystemLocalesA", 2) { return 1; }
WINAPI(GetTimeZoneInformation, "GetTimeZoneInformation", 1) { memset(GPTR(ARG(0)), 0, 172); return 0; }
/* CPINFO: one byte per character, default character '?', no lead bytes. */
WINAPI(GetCPInfo, "GetCPInfo", 2) {
    memset(GPTR(ARG(1)), 0, 20);
    MEM32(ARG(1)) = 1;
    *(uint8_t *)GPTR(ARG(1) + 4) = '?';
    return 1;
}
WINAPI(MulDiv, "MulDiv", 3) {
    int64_t p = (int64_t)(int32_t)ARG(0) * (int32_t)ARG(1), d = (int32_t)ARG(2);
    if (!d) return 0xFFFFFFFFu;
    return (uint32_t)(int32_t)((p + (((p < 0) != (d < 0)) ? -d / 2 : d / 2)) / d);
}

/* Character classes and case for Latin-1, the code page the guest sees: CT_CTYPE1 bits
 * (upper 1, lower 2, digit 4, space 8, punct 0x10, cntrl 0x20, blank 0x40, xdigit 0x80, alpha 0x100). */
static uint16_t ctype1(unsigned c) {
    if (c > 255) return 0x100;
    uint16_t t = 0;
    if (isupper(c) || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) t |= 0x101;
    if (islower(c) || (c >= 0xDF && c != 0xF7)) t |= 0x102;
    if (isdigit(c)) t |= 4;
    if (isspace(c)) t |= 8;
    if (ispunct(c) || (c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7) t |= 0x10;
    if (iscntrl(c)) t |= 0x20;
    if (c == ' ' || c == '\t') t |= 0x40;
    if (isxdigit(c)) t |= 0x80;
    return t;
}
static unsigned case_map(unsigned c, uint32_t flags) {
    if (c > 255) return c;
    if ((flags & 0x100) && (ctype1(c) & 2) && c != 0xDF && c != 0xFF) return c < 128 ? (unsigned)toupper(c) : c - 0x20;  /* LCMAP_UPPERCASE */
    if ((flags & 0x200) && (ctype1(c) & 1)) return c < 128 ? (unsigned)tolower(c) : c + 0x20;  /* LCMAP_LOWERCASE */
    return c;
}
static unsigned str_ch(uint32_t s, int i, int wide) { return wide ? *(uint16_t *)GPTR(s + 2 * i) : *(uint8_t *)GPTR(s + i); }
static int str_len(uint32_t s, int32_t n, int wide) {
    if (n >= 0) return n;
    int i = 0;
    while (str_ch(s, i, wide)) i++;
    return i + 1;
}
/* GetStringType(A: lcid first, W: no lcid) */
static uint32_t string_type(uint32_t type, uint32_t s, int32_t n, uint32_t out, int wide) {
    if (type != 1) return 0;
    n = str_len(s, n, wide);
    for (int i = 0; i < n; i++) *(uint16_t *)GPTR(out + 2 * i) = ctype1(str_ch(s, i, wide));
    return 1;
}
WINAPI(GetStringTypeA, "GetStringTypeA", 5) { return string_type(ARG(1), ARG(2), ARG(3), ARG(4), 0); }
WINAPI(GetStringTypeW, "GetStringTypeW", 4) { return string_type(ARG(0), ARG(1), ARG(2), ARG(3), 1); }
static uint32_t lcmap(uint32_t flags, uint32_t s, int32_t n, uint32_t out, uint32_t cap, int wide) {
    n = str_len(s, n, wide);
    if (!cap) return n;
    if ((uint32_t)n > cap) { last_error = 122; return 0; }
    for (int i = 0; i < n; i++) {
        unsigned c = case_map(str_ch(s, i, wide), flags);
        if (wide) *(uint16_t *)GPTR(out + 2 * i) = c; else *(uint8_t *)GPTR(out + i) = c;
    }
    return n;
}
WINAPI(LCMapStringA, "LCMapStringA", 6) { return lcmap(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 0); }
WINAPI(LCMapStringW, "LCMapStringW", 6) { return lcmap(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 1); }
static uint32_t compare(uint32_t flags, uint32_t a, int32_t na, uint32_t b, int32_t nb, int wide) {
    na = str_len(a, na, wide);
    nb = str_len(b, nb, wide);
    for (int i = 0; i < na && i < nb; i++) {
        unsigned x = str_ch(a, i, wide), y = str_ch(b, i, wide);
        if (flags & 1) x = case_map(x, 0x200), y = case_map(y, 0x200);  /* NORM_IGNORECASE */
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;  /* CSTR_LESS_THAN, CSTR_EQUAL, CSTR_GREATER_THAN */
}
WINAPI(CompareStringA, "CompareStringA", 6) { return compare(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 0); }
WINAPI(CompareStringW, "CompareStringW", 6) { return compare(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 1); }
WINAPI(GetThreadLocale, "GetThreadLocale", 0) { return 0x409; }
WINAPI(GetLocaleInfoA, "GetLocaleInfoA", 4) {
    const char *v = (ARG(1) & 0xFFF) == 0x1004 ? "1252" : (ARG(1) & 0xFFF) == 0x0B ? "437" : "";
    if (!ARG(3)) return strlen(v) + 1;
    snprintf(GPTR(ARG(2)), ARG(3), "%s", v);
    return strlen(v) + 1;
}
WINAPI(GetLocaleInfoW, "GetLocaleInfoW", 4) {
    const char *v = (ARG(1) & 0xFFF) == 0x1004 ? "1252" : (ARG(1) & 0xFFF) == 0x0B ? "437" : "";
    uint32_t n = strlen(v) + 1;
    for (uint32_t i = 0; ARG(3) && i < n && i < ARG(3); i++) *(uint16_t *)GPTR(ARG(2) + 2 * i) = v[i];
    return n;
}
/* Code pages as Latin-1: byte value == code point. */
WINAPI(MultiByteToWideChar, "MultiByteToWideChar", 6) {
    const uint8_t *s = GPTR(ARG(2));
    int n = (int32_t)ARG(3) < 0 ? (int)strlen((const char *)s) + 1 : (int)ARG(3);
    if (!ARG(5)) return n;
    if (n > (int)ARG(5)) n = ARG(5);
    for (int i = 0; i < n; i++) ((uint16_t *)GPTR(ARG(4)))[i] = s[i];
    return n;
}
WINAPI(WideCharToMultiByte, "WideCharToMultiByte", 8) {
    const uint16_t *s = GPTR(ARG(2));
    int n = (int)ARG(3);
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    if (!ARG(5)) return n;
    if (n > (int)ARG(5)) n = ARG(5);
    for (int i = 0; i < n; i++) ((uint8_t *)GPTR(ARG(4)))[i] = s[i] < 256 ? s[i] : '?';
    return n;
}
