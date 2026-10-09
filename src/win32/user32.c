/* USER32, GDI32, ADVAPI32, OLE32: one SDL window and a minimal message loop. */
#include "../runtime/rt.h"
#include <SDL2/SDL.h>
#include <ctype.h>

/* ---- printf family: format string and arguments live in guest memory ---- */

int rt_format(char *out, size_t n, const char *fmt, uint32_t args) {
    size_t len = 0;
#define EMIT(...) do { int _k = snprintf(out + len, len < n ? n - len : 0, __VA_ARGS__); if (_k > 0) len += _k; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { if (len + 1 < n) out[len] = *p; len++; continue; }
        char spec[32] = "%";
        int s = 1, wide64 = 0, isshort = 0;
        for (p++; *p && strchr("-+ #0", *p); p++) spec[s++] = *p;
        for (; *p == '*' || isdigit((unsigned char)*p) || *p == '.'; p++) {
            if (*p == '*') { s += snprintf(spec + s, sizeof spec - s, "%d", (int32_t)MEM32(args)); args += 4; }
            else spec[s++] = *p;
        }
        if (*p == 'l' && p[1] == 'l') { wide64 = 1; p += 2; }
        else if (!strncmp(p, "I64", 3)) { wide64 = 1; p += 3; }
        else if (*p == 'h') { isshort = 1; p++; }
        else if (*p == 'l' || *p == 'L' || *p == 'I') p++;
        char c = *p;
        if (!c) break;
        if (c == '%') { if (len + 1 < n) out[len] = '%'; len++; continue; }
        if (strchr("diouxXc", c)) {
            if (wide64) { spec[s++] = 'l'; spec[s++] = 'l'; }
            spec[s++] = c;
            spec[s] = 0;
            uint64_t v = MEM32(args) | (wide64 ? (uint64_t)MEM32(args + 4) << 32 : 0);
            args += wide64 ? 8 : 4;
            if (isshort) v = (c == 'd' || c == 'i') ? (uint64_t)(int16_t)v : (uint16_t)v;
            else if (!wide64 && (c == 'd' || c == 'i')) v = (uint64_t)(int64_t)(int32_t)v;
            if (wide64) EMIT(spec, (long long)v); else EMIT(spec, (int)v);
        } else if (strchr("eEfgGaA", c)) {
            double d;
            memcpy(&d, GPTR(args), 8);
            args += 8;
            spec[s++] = c;
            spec[s] = 0;
            EMIT(spec, d);
        } else if (c == 's' || c == 'S') {
            uint32_t sv = MEM32(args);
            args += 4;
            spec[s++] = 's';
            spec[s] = 0;
            EMIT(spec, sv ? GSTR(sv) : "(null)");
        } else if (c == 'p') {
            EMIT("%08X", MEM32(args));
            args += 4;
        } else if (c == 'n') {
            MEM32(MEM32(args)) = (uint32_t)len;
            args += 4;
        }
    }
    if (n) out[len < n ? len : n - 1] = 0;
    return (int)len;
#undef EMIT
}

#define HWND_MAIN 0x00010001u
#define WM_QUIT 0x12

SDL_Window *rt_window;
static uint32_t wndproc;
static int quit_posted;

void rt_pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) { rt_log("window closed"); quit_posted = 1; }
}

WINAPI(LoadCursorA, "LoadCursorA", 2) { return 1; }
WINAPI(RegisterClassExA, "RegisterClassExA", 1) {
    wndproc = MEM32(ARG(0) + 8);  /* WNDCLASSEXA.lpfnWndProc */
    return 0xC001;
}
WINAPI(CreateWindowExA, "CreateWindowExA", 12) {
    if (rt_trace) rt_log("  CreateWindowExA(%s, %dx%d)", ARG(2) ? GSTR(ARG(2)) : "", ARG(6), ARG(7));
    if (!rt_window) {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  /* let SIGTERM/SIGINT stop the port */
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER))
            rt_fatal("SDL_Init: %s", SDL_GetError());
        /* A scripted run (SH3_KEYS) stays off screen, unless SH3_SHOW is set to watch it: either way it
         * never reads the real keyboard. */
        rt_window = SDL_CreateWindow("Silent Hill 3", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                         (getenv("SH3_KEYS") && !getenv("SH3_SHOW") ? SDL_WINDOW_HIDDEN : 0));
        if (!rt_window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    }
    return HWND_MAIN;
}
WINAPI(ShowWindow, "ShowWindow", 2) { return 1; }
WINAPI(UpdateWindow, "UpdateWindow", 1) { return 1; }
static uint32_t call_wndproc(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l) {
    uint32_t a[4] = {hwnd, msg, w, l};
    return wndproc ? guest_call(wndproc, 4, a, 1) : 0;
}
/* WM_CLOSE destroys the window, which tells the window procedure with WM_DESTROY. */
WINAPI(DefWindowProcA, "DefWindowProcA", 4) {
    if (ARG(1) == 0x10) call_wndproc(ARG(0), 2, 0, 0);
    return 0;
}

/* MSG: hwnd, message, wParam, lParam, time, pt. Input goes through DirectInput, so the loop only sees what
 * the game posts itself (PostMessageA) and WM_QUIT. */
static uint32_t posted[16][4];
static int n_posted;
WINAPI(PostMessageA, "PostMessageA", 4) {
    if (n_posted == 16) return 0;
    memcpy(posted[n_posted++], (uint32_t[4]){ARG(0), ARG(1), ARG(2), ARG(3)}, 16);
    return 1;
}
static int next_message(uint32_t m, int remove) {
    rt_pump_events();
    if (!n_posted && !quit_posted) return 0;
    memset(GPTR(m), 0, 28);
    if (n_posted) {
        memcpy(GPTR(m), posted[0], 16);
        if (remove) memmove(posted, posted + 1, --n_posted * sizeof *posted);
    } else {
        MEM32(m) = HWND_MAIN;
        MEM32(m + 4) = WM_QUIT;
    }
    return 1;
}
WINAPI(PeekMessageA, "PeekMessageA", 5) { return next_message(ARG(0), ARG(4) & 1); }
WINAPI(GetMessageA, "GetMessageA", 4) {
    while (!next_message(ARG(0), 1)) SDL_Delay(1);
    return MEM32(ARG(0) + 4) != WM_QUIT;
}
WINAPI(TranslateMessage, "TranslateMessage", 1) { return 0; }
WINAPI(DispatchMessageA, "DispatchMessageA", 1) {
    uint32_t m = ARG(0);
    return call_wndproc(MEM32(m), MEM32(m + 4), MEM32(m + 8), MEM32(m + 12));
}
WINAPI(SendMessageA, "SendMessageA", 4) { return call_wndproc(ARG(0), ARG(1), ARG(2), ARG(3)); }
WINAPI(PostQuitMessage, "PostQuitMessage", 1) { rt_log("PostQuitMessage(%d)", ARG(0)); quit_posted = 1; return 0; }
WINAPI(BeginPaint, "BeginPaint", 2) { memset(GPTR(ARG(1)), 0, 64); return 1; }
WINAPI(EndPaint, "EndPaint", 2) { return 1; }
WINAPI(MessageBoxA, "MessageBoxA", 4) {
    rt_log("MessageBox [%s]: %s", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : "");
    return 1;  /* IDOK */
}
WINAPI(wvsprintfA, "wvsprintfA", 3) {
    char tmp[1025];
    int k = rt_format(tmp, sizeof tmp, GSTR(ARG(1)), ARG(2));
    strcpy(GPTR(ARG(0)), tmp);
    return k;
}
CDECL(wsprintfA, "wsprintfA") {
    char tmp[1025];
    int k = rt_format(tmp, sizeof tmp, GSTR(ARG(1)), g_esp + 12);
    strcpy(GPTR(ARG(0)), tmp);
    return k;
}

WINAPI(DeleteObject, "DeleteObject", 1) { return 1; }
WINAPI(GetStockObject, "GetStockObject", 1) { return 0x00020000u + ARG(0); }
WINAPI(KillTimer, "KillTimer", 2) { return 1; }
WINAPI(RegisterWindowMessageA, "RegisterWindowMessageA", 1) {
    static uint32_t next = 0xC100;
    return next++;
}

/* The registry holds one key, the folders SH3's installer records (GetFullPathNameA's C:\SH3 is the game
 * folder); every other key is missing, so the game uses its defaults. */
#define HKEY_SH3 0x53483300u
static uint32_t reg_open(uint32_t subkey, uint32_t out) {
    if (rt_trace) rt_log("  RegOpenKey(%s)", subkey ? GSTR(subkey) : "");
    if (!subkey || strcasecmp(GSTR(subkey), "SOFTWARE\\KONAMI\\SILENT HILL 3")) return 2;  /* ERROR_FILE_NOT_FOUND */
    MEM32(out) = HKEY_SH3;
    return 0;
}
WINAPI(RegOpenKeyA, "RegOpenKeyA", 3) { return reg_open(ARG(1), ARG(2)); }
WINAPI(RegOpenKeyExA, "RegOpenKeyExA", 5) { return reg_open(ARG(1), ARG(4)); }
/* RegQueryValueExA(key, name, reserved, type, data, size): REG_SZ folders. */
WINAPI(RegQueryValueExA, "RegQueryValueExA", 6) {
    const char *name = ARG(1) ? GSTR(ARG(1)) : "", *v = NULL;
    if (ARG(0) == HKEY_SH3 && (!strcasecmp(name, "data") || !strcasecmp(name, "movie") || !strcasecmp(name, "sound")))
        v = "C:\\SH3\\";
    if (ARG(0) == HKEY_SH3 && !strcasecmp(name, "save")) v = "C:\\SH3\\savedata\\";
    if (!v) return 2;
    uint32_t n = strlen(v) + 1, room = ARG(5) ? MEM32(ARG(5)) : 0;
    if (ARG(3)) MEM32(ARG(3)) = 1;  /* REG_SZ */
    if (ARG(5)) MEM32(ARG(5)) = n;
    if (!ARG(4)) return 0;
    if (room < n) return 234;  /* ERROR_MORE_DATA */
    memcpy(GPTR(ARG(4)), v, n);
    return 0;
}
WINAPI(RegCloseKey, "RegCloseKey", 1) { return 0; }

WINAPI(CoInitialize, "CoInitialize", 1) { return 0; }
WINAPI(CoUninitialize, "CoUninitialize", 0) { return 0; }
WINAPI(CoFreeUnusedLibraries, "CoFreeUnusedLibraries", 0) { return 0; }
WINAPI(CoTaskMemAlloc, "CoTaskMemAlloc", 1) { return galloc(ARG(0)); }
WINAPI(CoTaskMemFree, "CoTaskMemFree", 1) { gfree(ARG(0)); return 0; }
WINAPI(CoCreateInstance, "CoCreateInstance", 5) {
    uint32_t c = ARG(0);
    rt_log("CoCreateInstance(%08X-%04X): no such class", MEM32(c), *(uint16_t *)GPTR(c + 4));
    MEM32(ARG(4)) = 0;
    return 0x80040154u;  /* REGDB_E_CLASSNOTREG */
}
/* BSTR: a length prefix, then the UTF-16 string. */
WINAPI(SysAllocString, "OLEAUT32#2", 1) {
    uint32_t n = 0;
    while (*(uint16_t *)GPTR(ARG(0) + 2 * n)) n++;
    uint32_t b = galloc(4 + 2 * n + 2);
    MEM32(b) = 2 * n;
    memcpy(GPTR(b + 4), GPTR(ARG(0)), 2 * n);
    return b + 4;
}
WINAPI(SysFreeString, "OLEAUT32#6", 1) { if (ARG(0)) gfree(ARG(0) - 4); return 0; }

WINAPI(ImmGetContext, "ImmGetContext", 1) { return 0; }
WINAPI(ImmAssociateContext, "ImmAssociateContext", 2) { return 0; }

/* ---- window management: one window, always the size of the screen, never moved ---- */
WINAPI(AdjustWindowRectEx, "AdjustWindowRectEx", 4) { return 1; }
WINAPI(InvalidateRect, "InvalidateRect", 3) { return 1; }
WINAPI(RedrawWindow, "RedrawWindow", 4) { return 1; }
WINAPI(ClipCursor, "ClipCursor", 1) { return 1; }
WINAPI(SetForegroundWindow, "SetForegroundWindow", 1) { return 1; }
WINAPI(GetForegroundWindow, "GetForegroundWindow", 0) { return HWND_MAIN; }
WINAPI(SetFocus, "SetFocus", 1) { return HWND_MAIN; }
WINAPI(FindWindowA, "FindWindowA", 2) { return 0; }
WINAPI(LoadIconA, "LoadIconA", 2) { return 1; }
WINAPI(SetCursor, "SetCursor", 1) { return 0; }
WINAPI(IsIconic, "IsIconic", 1) { return 0; }
WINAPI(SetWindowPos, "SetWindowPos", 7) { return 1; }
WINAPI(AttachThreadInput, "AttachThreadInput", 3) { return 1; }
WINAPI(GetWindowThreadProcessId, "GetWindowThreadProcessId", 2) {
    if (ARG(1)) MEM32(ARG(1)) = 0x1234;
    return 0x100;  /* the game thread (kernel32.c numbers threads from 0x100) */
}
WINAPI(RegisterDeviceNotificationA, "RegisterDeviceNotificationA", 3) { return 1; }
WINAPI(UnregisterDeviceNotification, "UnregisterDeviceNotification", 1) { return 1; }
WINAPI(ChangeDisplaySettingsA, "ChangeDisplaySettingsA", 2) { return 0; }
WINAPI(SystemParametersInfoA, "SystemParametersInfoA", 4) { return 1; }
WINAPI(ClientToScreen, "ClientToScreen", 2) { return 1; }
WINAPI(ScreenToClient, "ScreenToClient", 2) { return 1; }
WINAPI(GetCursorPos, "GetCursorPos", 1) { MEM32(ARG(0)) = 0; MEM32(ARG(0) + 4) = 0; return 1; }
WINAPI(SetCursorPos, "SetCursorPos", 2) { return 1; }
WINAPI(GetKeyState, "GetKeyState", 1) { return 0; }
extern uint32_t rt_res_w, rt_res_h;
WINAPI(GetSystemMetrics, "GetSystemMetrics", 1) { return ARG(0) == 0 ? rt_res_w : ARG(0) == 1 ? rt_res_h : 0; }
WINAPI(GetWindowRect, "GetWindowRect", 2) {
    uint32_t r[4] = {0, 0, rt_res_w, rt_res_h};
    memcpy(GPTR(ARG(1)), r, sizeof r);
    return 1;
}
/* GWL_WNDPROC (-4) subclasses the window: messages then go to the new procedure. */
static uint32_t window_long[8];
WINAPI(SetWindowLongA, "SetWindowLongA", 3) {
    uint32_t i = ARG(1) & 7, old = (int32_t)ARG(1) == -4 ? wndproc : window_long[i];
    if ((int32_t)ARG(1) == -4) wndproc = ARG(2); else window_long[i] = ARG(2);
    return old;
}
WINAPI(GetWindowLongA, "GetWindowLongA", 2) { return (int32_t)ARG(1) == -4 ? wndproc : window_long[ARG(1) & 7]; }
WINAPI(GetClassLongA, "GetClassLongA", 2) { return 0; }
WINAPI(CallWindowProcA, "CallWindowProcA", 5) {
    uint32_t a[4] = {ARG(1), ARG(2), ARG(3), ARG(4)};
    return guest_call(ARG(0), 4, a, 1);
}
/* DEVMODEA: dmBitsPerPel @104, dmPelsWidth @108, dmPelsHeight @112, dmDisplayFrequency @120. */
WINAPI(EnumDisplaySettingsA, "EnumDisplaySettingsA", 3) {
    if (ARG(1) != 0xFFFFFFFFu && ARG(1) != 0xFFFFFFFEu && ARG(1) > 0) return 0;  /* one mode: the screen's */
    uint32_t m = ARG(2);
    MEM32(m + 104) = 32;
    MEM32(m + 108) = rt_res_w;
    MEM32(m + 112) = rt_res_h;
    MEM32(m + 120) = 60;
    return 1;
}
