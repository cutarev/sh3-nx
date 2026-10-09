/* DINPUT8: a keyboard and one gamepad, read from SDL. */
#include "../runtime/rt.h"
#include <SDL2/SDL.h>

enum { DEV_KEYBOARD = 1, DEV_MOUSE, DEV_PAD };
/* Device object: vtable, kind, data size from SetDataFormat, axis range. */
#define DEV_KIND(o) MEM32((o) + 4)
#define DEV_SIZE(o) MEM32((o) + 8)
#define DEV_MIN(o) MEM32((o) + 12)
#define DEV_MAX(o) MEM32((o) + 16)
#define DEV_FORMAT(o) MEM32((o) + 20)  /* the game's DIDATAFORMAT */

static SDL_GameController *pad;

static SDL_GameController *get_pad(void) {
    if (pad && !SDL_GameControllerGetAttached(pad)) { SDL_GameControllerClose(pad); pad = NULL; }
    for (int i = 0; !pad && i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);
    return pad;
}

static void fill_instance(uint32_t di, int kind) {
    memset(GPTR(di), 0, 580);
    MEM32(di) = 580;
    MEM32(di + 4) = 0x53480000u + kind;          /* guidInstance.Data1 */
    MEM32(di + 20) = 0x53480000u + kind;         /* guidProduct.Data1 */
    MEM32(di + 36) = kind == DEV_PAD ? 0x10215 : kind == DEV_KEYBOARD ? 0x13 : 0x12;
    const char *name = kind == DEV_PAD ? "Gamepad" : kind == DEV_KEYBOARD ? "Keyboard" : "Mouse";
    strcpy(GPTR(di + 40), name);
    strcpy(GPTR(di + 300), name);
}

/* ---- IDirectInputDevice8A ---- */
METHOD(dev_AddRef, 1) { return 1; }
METHOD(dev_Release, 1) { return 0; }
METHOD(dev_GetCapabilities, 2) {
    uint32_t c = ARG(1), kind = DEV_KIND(THIS);
    MEM32(c + 4) = 1 | (kind == DEV_PAD ? 0x100 : 0);  /* DIDC_ATTACHED | DIDC_FORCEFEEDBACK */
    MEM32(c + 8) = kind == DEV_PAD ? 0x10215 : 0x13;
    MEM32(c + 12) = kind == DEV_PAD ? 4 : 0;           /* axes */
    MEM32(c + 16) = kind == DEV_PAD ? 12 : 256;        /* buttons */
    MEM32(c + 20) = kind == DEV_PAD ? 1 : 0;           /* POVs */
    return 0;
}
/* The pad's objects as EnumObjects reports them: GUID Data1, offset in DIJOYSTATE2, DIDFT type.
 * GUIDs are the standard {A36D02xx-C9F3-11CF-BFC7-444553540000} family. */
typedef struct { uint32_t guid, ofs, type; const char *name; } PadObj;
static const PadObj pad_objs[] = {
    {0xA36D02E0u, 0, 0x002, "X Axis"}, {0xA36D02E1u, 4, 0x102, "Y Axis"},
    {0xA36D02E2u, 8, 0x202, "Z Axis"}, {0xA36D02E3u, 20, 0x502, "Z Rotation"},
    {0xA36D02F2u, 32, 0x010, "Hat Switch"},
    {0xA36D02F0u, 48, 0x004, "Button 0"}, {0xA36D02F0u, 49, 0x104, "Button 1"}, {0xA36D02F0u, 50, 0x204, "Button 2"},
    {0xA36D02F0u, 51, 0x304, "Button 3"}, {0xA36D02F0u, 52, 0x404, "Button 4"}, {0xA36D02F0u, 53, 0x504, "Button 5"},
    {0xA36D02F0u, 54, 0x604, "Button 6"}, {0xA36D02F0u, 55, 0x704, "Button 7"}, {0xA36D02F0u, 56, 0x804, "Button 8"},
    {0xA36D02F0u, 57, 0x904, "Button 9"}, {0xA36D02F0u, 58, 0xA04, "Button 10"}, {0xA36D02F0u, 59, 0xB04, "Button 11"},
};

/* DIDEVICEOBJECTINSTANCEA (316 bytes): size, guidType, dwOfs, dwType, dwFlags, tszName. */
static void pad_obj_fill(uint32_t inst, const PadObj *o) {
    memset(GPTR(inst), 0, 316);
    MEM32(inst) = 316;
    MEM32(inst + 4) = o->guid;
    *(uint16_t *)GPTR(inst + 8) = 0xC9F3;
    *(uint16_t *)GPTR(inst + 10) = 0x11CF;
    memcpy(GPTR(inst + 12), "\xBF\xC7\x44\x45\x53\x54\x00\x00", 8);
    MEM32(inst + 20) = o->ofs;
    MEM32(inst + 24) = o->type;
    snprintf(GPTR(inst + 32), 260, "%s", o->name);
}

/* EnumObjects(cb, ref, flags): BOOL CALLBACK cb(const DIDEVICEOBJECTINSTANCEA *, void *). */
METHOD(dev_EnumObjects, 4) {
    if (DEV_KIND(THIS) != DEV_PAD) return 0;
    uint32_t want = ARG(3) & 0xFF, inst = galloc(316);
    for (size_t i = 0; i < sizeof pad_objs / sizeof *pad_objs; i++) {
        const PadObj *o = &pad_objs[i];
        if (want && !(o->type & want & 0x1F)) continue;  /* DIDFT_AXIS / BUTTON / POV filters */
        pad_obj_fill(inst, o);
        uint32_t a[2] = {inst, ARG(2)};
        if (!guest_call(ARG(1), 2, a, 1)) break;  /* DIENUM_STOP */
    }
    gfree(inst);
    return 0;
}

/* GetObjectInfo(inst, obj, how): by offset (DIPH_BYOFFSET) or by id; DIERR_NOTFOUND for an object the device
 * lacks. SH3's Button Config probes the pad's offsets to learn which buttons and axes exist. */
METHOD(dev_GetObjectInfo, 4) {
    uint32_t obj = ARG(2), how = ARG(3);
    if (DEV_KIND(THIS) == DEV_KEYBOARD) {  /* every DIK code exists; the game names keys itself */
        if (how != 1 || obj > 255) return 0x80070002u;
        pad_obj_fill(ARG(1), &(PadObj){0x55728220u, obj, 0x0C | obj << 8, ""});  /* GUID_Key */
        return 0;
    }
    for (size_t i = 0; DEV_KIND(THIS) == DEV_PAD && i < sizeof pad_objs / sizeof *pad_objs; i++) {
        const PadObj *o = &pad_objs[i];
        if (how == 1 ? o->ofs == obj : how == 2 && ((o->type ^ obj) & 0xFFFF00) == 0 && (o->type & obj & 0x1F)) {
            pad_obj_fill(ARG(1), o);
            return 0;
        }
    }
    return 0x80070002u;  /* DIERR_NOTFOUND */
}

/* GetProperty(guid, DIPROPHEADER *): range, dead zone, saturation; values after the 16-byte header. */
METHOD(dev_GetProperty, 3) {
    uint32_t p = ARG(2);
    switch (ARG(1)) {
    case 4: MEM32(p + 16) = DEV_MIN(THIS); MEM32(p + 20) = DEV_MAX(THIS); return 0;  /* DIPROP_RANGE */
    case 5: MEM32(p + 16) = 0; return 0;                                             /* DIPROP_DEADZONE */
    case 6: MEM32(p + 16) = 10000; return 0;                                         /* DIPROP_SATURATION */
    case 2: MEM32(p + 16) = 0; return 0;                                             /* DIPROP_AXISMODE: absolute */
    case 1: MEM32(p + 16) = 0; return 0;                                             /* DIPROP_BUFFERSIZE */
    }
    return 0x80004001u;  /* E_NOTIMPL */
}
METHOD(dev_SetProperty, 3) {
    if (rt_trace && DEV_KIND(THIS) == DEV_PAD)
        rt_log("pad property %u: obj %u how %u values %d %d", ARG(1), MEM32(ARG(2) + 8), MEM32(ARG(2) + 12),
               (int32_t)MEM32(ARG(2) + 16), (int32_t)MEM32(ARG(2) + 20));
    if (ARG(1) == 4) {                                 /* DIPROP_RANGE: lMin, lMax after the 16-byte header */
        DEV_MIN(THIS) = MEM32(ARG(2) + 16);
        DEV_MAX(THIS) = MEM32(ARG(2) + 20);
    }
    return 0;
}
METHOD(dev_Acquire, 1) { return 0; }
METHOD(dev_Unacquire, 1) { return 0; }
METHOD(dev_SetDataFormat, 2) {
    DEV_SIZE(THIS) = MEM32(ARG(1) + 12);  /* DIDATAFORMAT.dwDataSize */
    DEV_FORMAT(THIS) = ARG(1);
    if (rt_trace && DEV_KIND(THIS) == DEV_PAD) {  /* what the game expects from a pad, for the log */
        uint32_t f = ARG(1), n = MEM32(f + 16), o = MEM32(f + 20);
        rt_log("pad data format: %u bytes, %u objects", MEM32(f + 12), n);
        for (uint32_t i = 0; i < n; i++, o += 16)
            rt_log("  guid %08X ofs %u type %08X flags %X", MEM32(o) ? MEM32(MEM32(o)) : 0, MEM32(o + 4),
                   MEM32(o + 8), MEM32(o + 12));
    }
    return 0;
}
METHOD(dev_SetEventNotification, 2) { return 0; }
METHOD(dev_SetCooperativeLevel, 3) { return 0; }
METHOD(dev_GetDeviceInfo, 2) { fill_instance(ARG(1), DEV_KIND(THIS)); return 0; }
METHOD(dev_Poll, 1) { return 0; }
/* ---- force feedback: every DirectInput effect becomes a rumble of the pad (Switch HD rumble via SDL) ----
 * An effect object keeps its GUID Data1, magnitude (0..10000 after gain) and duration (microseconds). */
#define EFF_GUID(o) MEM32((o) + 4)
#define EFF_MAG(o) MEM32((o) + 8)
#define EFF_DUR(o) MEM32((o) + 12)
#define EFF_ON(o) MEM32((o) + 16)
static int ff_muted;  /* DISFFC_STOPALL / SETACTUATORSOFF */

static void rumble(uint32_t mag, uint32_t dur_us) {
    SDL_GameController *c = get_pad();
    if (!c) return;
    if (ff_muted) mag = 0;
    uint32_t ms = dur_us == 0xFFFFFFFFu || dur_us == 0 ? 0xFFFFFFFFu : (dur_us + 999) / 1000;
    if (ms > 10000) ms = 10000;  /* "infinite" effects are refreshed by Start or stopped explicitly */
    uint16_t v = (uint16_t)(mag > 10000 ? 0xFFFF : mag * 0xFFFFu / 10000);
    SDL_GameControllerRumble(c, v, v, mag ? ms : 0);
}

/* Reads a DIEFFECT: duration, gain and the magnitude from the type-specific block
 * (DICONSTANTFORCE.lMagnitude, DIPERIODIC.dwMagnitude, DIRAMPFORCE start/end). */
static void eff_read(uint32_t e, uint32_t p, uint32_t flags) {
    if (!p) return;
    if (flags & 1) EFF_DUR(e) = MEM32(p + 8);                     /* DIEP_DURATION */
    uint32_t gain = (flags & 4) ? MEM32(p + 16) : 10000;          /* DIEP_GAIN */
    if (gain > 10000 || !(flags & 4)) gain = 10000;
    uint32_t cb = MEM32(p + 44), ts = MEM32(p + 48);
    if ((flags & 0x200) && ts && cb >= 4) {                        /* DIEP_TYPESPECIFICPARAMS */
        int32_t m;
        if (EFF_GUID(e) == 0x13541C21u && cb >= 8) {               /* ramp: the stronger end */
            int32_t a = (int32_t)MEM32(ts), b = (int32_t)MEM32(ts + 4);
            m = abs(a) > abs(b) ? a : b;
        } else if (EFF_GUID(e) >= 0x13541C22u && EFF_GUID(e) <= 0x13541C26u)
            m = (int32_t)MEM32(ts);                                /* periodic: dwMagnitude */
        else
            m = (int32_t)MEM32(ts);                                /* constant: lMagnitude */
        EFF_MAG(e) = (uint32_t)abs(m) * gain / 10000;
    }
}

void dinput_rumble_stop(void) { rumble(0, 0); }

METHOD(eff_AddRef, 1) { return 1; }
METHOD(eff_Release, 1) { if (EFF_ON(THIS)) rumble(0, 0); return 0; }
METHOD(eff_Initialize, 4) { return 0; }
METHOD(eff_GetEffectGuid, 2) { memset(GPTR(ARG(1)), 0, 16); MEM32(ARG(1)) = EFF_GUID(THIS); return 0; }
METHOD(eff_GetParameters, 3) { return 0; }
METHOD(eff_SetParameters, 3) {
    eff_read(THIS, ARG(1), ARG(2));
    if (EFF_ON(THIS) || (ARG(2) & 0x20000000u)) {                  /* DIEP_START */
        EFF_ON(THIS) = 1;
        rumble(EFF_MAG(THIS), EFF_DUR(THIS));
    }
    return 0;
}
METHOD(eff_Start, 3) { EFF_ON(THIS) = 1; rumble(EFF_MAG(THIS), EFF_DUR(THIS)); return 0; }
METHOD(eff_Stop, 1) { EFF_ON(THIS) = 0; rumble(0, 0); return 0; }
METHOD(eff_GetEffectStatus, 2) { MEM32(ARG(1)) = EFF_ON(THIS) ? 1 : 0; return 0; }
METHOD(eff_Download, 1) { return 0; }
METHOD(eff_Unload, 1) { return 0; }
METHOD(eff_Escape, 2) { return 0x80004001u; }

static const ComEntry eff_vt[] = {
    {"IDirectInputEffect::QueryInterface", NULL}, {"IDirectInputEffect::AddRef", eff_AddRef},
    {"IDirectInputEffect::Release", eff_Release}, {"IDirectInputEffect::Initialize", eff_Initialize},
    {"IDirectInputEffect::GetEffectGuid", eff_GetEffectGuid}, {"IDirectInputEffect::GetParameters", eff_GetParameters},
    {"IDirectInputEffect::SetParameters", eff_SetParameters}, {"IDirectInputEffect::Start", eff_Start},
    {"IDirectInputEffect::Stop", eff_Stop}, {"IDirectInputEffect::GetEffectStatus", eff_GetEffectStatus},
    {"IDirectInputEffect::Download", eff_Download}, {"IDirectInputEffect::Unload", eff_Unload},
    {"IDirectInputEffect::Escape", eff_Escape},
};

/* CreateEffect(rguid, const DIEFFECT *, LPDIRECTINPUTEFFECT *, outer) */
METHOD(dev_CreateEffect, 5) {
    static uint32_t vt;
    if (DEV_KIND(THIS) != DEV_PAD) return 0x80004001u;
    if (!vt) vt = com_vtable(eff_vt, sizeof eff_vt / sizeof *eff_vt);
    uint32_t e = com_new(vt, 32);
    EFF_GUID(e) = ARG(1) ? MEM32(ARG(1)) : 0x13541C20u;
    EFF_MAG(e) = 10000;
    EFF_DUR(e) = 0xFFFFFFFFu;
    eff_read(e, ARG(2), 0xFFFFFFFFu & ~0x20000000u);
    if (rt_trace) rt_log("pad effect %08X created: magnitude %u duration %u us", EFF_GUID(e), EFF_MAG(e), EFF_DUR(e));
    MEM32(ARG(3)) = e;
    return 0;
}

/* EnumEffects(cb, ref, type): BOOL CALLBACK cb(const DIEFFECTINFOA *, void *) for constant and periodic forces. */
METHOD(dev_EnumEffects, 4) {
    static const struct { uint32_t guid, type; const char *name; } fx[] = {
        {0x13541C20u, 0x01, "Constant Force"}, {0x13541C21u, 0x02, "Ramp Force"},
        {0x13541C22u, 0x03, "Square"}, {0x13541C23u, 0x03, "Sine"}, {0x13541C24u, 0x03, "Triangle"},
        {0x13541C25u, 0x03, "Sawtooth Up"}, {0x13541C26u, 0x03, "Sawtooth Down"},
    };
    if (DEV_KIND(THIS) != DEV_PAD || !get_pad()) return 0;
    uint32_t want = ARG(3) & 0xFF, info = galloc(296);
    for (size_t i = 0; i < sizeof fx / sizeof *fx; i++) {
        if (want && want != fx[i].type) continue;
        memset(GPTR(info), 0, 296);
        MEM32(info) = 296;
        MEM32(info + 4) = fx[i].guid;                              /* GUID_xxx {13541C2x-8E33-11D0-9AD0-00A0C9A06E35} */
        *(uint16_t *)GPTR(info + 8) = 0x8E33;
        *(uint16_t *)GPTR(info + 10) = 0x11D0;
        memcpy(GPTR(info + 12), "\x9A\xD0\x00\xA0\xC9\xA0\x6E\x35", 8);
        MEM32(info + 20) = fx[i].type;                             /* dwEffType */
        MEM32(info + 24) = 0x1 | 0x4 | 0x200;                      /* static params: duration, gain, type-specific */
        MEM32(info + 28) = 0x1 | 0x4 | 0x200;
        snprintf(GPTR(info + 32), 260, "%s", fx[i].name);
        uint32_t a[2] = {info, ARG(2)};
        if (!guest_call(ARG(1), 2, a, 1)) break;
    }
    gfree(info);
    return 0;
}
METHOD(dev_EnumCreatedEffectObjects, 4) { return 0; }
METHOD(dev_SendForceFeedbackCommand, 2) {
    switch (ARG(1)) {
    case 0x01: case 0x10: ff_muted = 0; break;                     /* RESET, SETACTUATORSON */
    case 0x02: case 0x20: rumble(0, 0); ff_muted = ARG(1) == 0x20; break;  /* STOPALL, SETACTUATORSOFF */
    case 0x04: case 0x08: break;                                   /* PAUSE, CONTINUE */
    }
    return 0;
}
METHOD(dev_GetForceFeedbackState, 2) { MEM32(ARG(1)) = ff_muted ? 0x20 : 0x10; return 0; }
METHOD(dev_Escape, 2) { return 0x80004001u; }
METHOD(dev_GetDeviceData, 5) {
    static int logged;
    if (!logged++) rt_log("GetDeviceData on device kind %u (1 keyboard, 2 mouse, 3 pad), buffer %u", DEV_KIND(THIS), MEM32(ARG(3)));
    MEM32(ARG(3)) = 0;
    return 0;
}

/* SDL scancode -> DirectInput key code (PC scan code set 1). */
static const uint8_t dik[SDL_NUM_SCANCODES] = {
    [SDL_SCANCODE_ESCAPE] = 0x01, [SDL_SCANCODE_1] = 0x02, [SDL_SCANCODE_2] = 0x03, [SDL_SCANCODE_3] = 0x04,
    [SDL_SCANCODE_4] = 0x05, [SDL_SCANCODE_5] = 0x06, [SDL_SCANCODE_6] = 0x07, [SDL_SCANCODE_7] = 0x08,
    [SDL_SCANCODE_8] = 0x09, [SDL_SCANCODE_9] = 0x0A, [SDL_SCANCODE_0] = 0x0B, [SDL_SCANCODE_MINUS] = 0x0C,
    [SDL_SCANCODE_EQUALS] = 0x0D, [SDL_SCANCODE_BACKSPACE] = 0x0E, [SDL_SCANCODE_TAB] = 0x0F,
    [SDL_SCANCODE_Q] = 0x10, [SDL_SCANCODE_W] = 0x11, [SDL_SCANCODE_E] = 0x12, [SDL_SCANCODE_R] = 0x13,
    [SDL_SCANCODE_T] = 0x14, [SDL_SCANCODE_Y] = 0x15, [SDL_SCANCODE_U] = 0x16, [SDL_SCANCODE_I] = 0x17,
    [SDL_SCANCODE_O] = 0x18, [SDL_SCANCODE_P] = 0x19, [SDL_SCANCODE_LEFTBRACKET] = 0x1A,
    [SDL_SCANCODE_RIGHTBRACKET] = 0x1B, [SDL_SCANCODE_RETURN] = 0x1C, [SDL_SCANCODE_LCTRL] = 0x1D,
    [SDL_SCANCODE_A] = 0x1E, [SDL_SCANCODE_S] = 0x1F, [SDL_SCANCODE_D] = 0x20, [SDL_SCANCODE_F] = 0x21,
    [SDL_SCANCODE_G] = 0x22, [SDL_SCANCODE_H] = 0x23, [SDL_SCANCODE_J] = 0x24, [SDL_SCANCODE_K] = 0x25,
    [SDL_SCANCODE_L] = 0x26, [SDL_SCANCODE_SEMICOLON] = 0x27, [SDL_SCANCODE_APOSTROPHE] = 0x28,
    [SDL_SCANCODE_GRAVE] = 0x29, [SDL_SCANCODE_LSHIFT] = 0x2A, [SDL_SCANCODE_BACKSLASH] = 0x2B,
    [SDL_SCANCODE_Z] = 0x2C, [SDL_SCANCODE_X] = 0x2D, [SDL_SCANCODE_C] = 0x2E, [SDL_SCANCODE_V] = 0x2F,
    [SDL_SCANCODE_B] = 0x30, [SDL_SCANCODE_N] = 0x31, [SDL_SCANCODE_M] = 0x32, [SDL_SCANCODE_COMMA] = 0x33,
    [SDL_SCANCODE_PERIOD] = 0x34, [SDL_SCANCODE_SLASH] = 0x35, [SDL_SCANCODE_RSHIFT] = 0x36,
    [SDL_SCANCODE_KP_MULTIPLY] = 0x37, [SDL_SCANCODE_LALT] = 0x38, [SDL_SCANCODE_SPACE] = 0x39,
    [SDL_SCANCODE_CAPSLOCK] = 0x3A, [SDL_SCANCODE_F1] = 0x3B, [SDL_SCANCODE_F2] = 0x3C, [SDL_SCANCODE_F3] = 0x3D,
    [SDL_SCANCODE_F4] = 0x3E, [SDL_SCANCODE_F5] = 0x3F, [SDL_SCANCODE_F6] = 0x40, [SDL_SCANCODE_F7] = 0x41,
    [SDL_SCANCODE_F8] = 0x42, [SDL_SCANCODE_F9] = 0x43, [SDL_SCANCODE_F10] = 0x44, [SDL_SCANCODE_KP_7] = 0x47,
    [SDL_SCANCODE_KP_8] = 0x48, [SDL_SCANCODE_KP_9] = 0x49, [SDL_SCANCODE_KP_MINUS] = 0x4A,
    [SDL_SCANCODE_KP_4] = 0x4B, [SDL_SCANCODE_KP_5] = 0x4C, [SDL_SCANCODE_KP_6] = 0x4D,
    [SDL_SCANCODE_KP_PLUS] = 0x4E, [SDL_SCANCODE_KP_1] = 0x4F, [SDL_SCANCODE_KP_2] = 0x50,
    [SDL_SCANCODE_KP_3] = 0x51, [SDL_SCANCODE_KP_0] = 0x52, [SDL_SCANCODE_KP_PERIOD] = 0x53,
    [SDL_SCANCODE_F11] = 0x57, [SDL_SCANCODE_F12] = 0x58, [SDL_SCANCODE_KP_ENTER] = 0x9C,
    [SDL_SCANCODE_RCTRL] = 0x9D, [SDL_SCANCODE_KP_DIVIDE] = 0xB5, [SDL_SCANCODE_RALT] = 0xB8,
    [SDL_SCANCODE_HOME] = 0xC7, [SDL_SCANCODE_UP] = 0xC8, [SDL_SCANCODE_PAGEUP] = 0xC9,
    [SDL_SCANCODE_LEFT] = 0xCB, [SDL_SCANCODE_RIGHT] = 0xCD, [SDL_SCANCODE_END] = 0xCF,
    [SDL_SCANCODE_DOWN] = 0xD0, [SDL_SCANCODE_PAGEDOWN] = 0xD1, [SDL_SCANCODE_INSERT] = 0xD2,
    [SDL_SCANCODE_DELETE] = 0xD3,
};

static uint32_t axis(uint32_t dev, int v) {  /* SDL -32768..32767 to the range the game set */
    int32_t lo = (int32_t)DEV_MIN(dev), hi = (int32_t)DEV_MAX(dev);
    return (uint32_t)(lo + (int32_t)(((int64_t)v + 32768) * (hi - lo) / 65535));
}

/* The pad as DirectInput objects, numbered like SH2EE's Xidi on PC (StandardGamepad, face buttons
 * X A B Y): X/Y the left stick, Z/Rz the right one, a hat, and 12 buttons
 * X A B Y LB RB LT RT Back Start LS RS. SDL names buttons by position (Xbox layout), on the Switch too. */
enum { OBJ_AXIS, OBJ_BUTTON, OBJ_POV };
static const uint32_t axis_guid[4] = {0xA36D02E0u, 0xA36D02E1u, 0xA36D02E2u, 0xA36D02E3u};  /* X Y Z Rz */

static int32_t pad_axis(SDL_GameController *c, uint32_t dev, int i) {
    static const SDL_GameControllerAxis ax[4] = {SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
                                                 SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY};
    return (int32_t)axis(dev, c ? SDL_GameControllerGetAxis(c, ax[i]) : 0);
}

static int pad_button(SDL_GameController *c, int i) {
    static const SDL_GameControllerButton bt[12] = {
        SDL_CONTROLLER_BUTTON_X,
#ifdef __SWITCH__  /* Nintendo's way round: action on A (east), cancel on B (south) */
        SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A,
#else
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
#endif
        SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0, 0, SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK};
    if (!c || i >= 12) return 0;
    if (i == 6 || i == 7)
        return SDL_GameControllerGetAxis(c, i == 6 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
    return SDL_GameControllerGetButton(c, bt[i]);
}

static uint32_t pad_pov(SDL_GameController *c) {  /* hundredths of a degree clockwise from north */
    if (!c) return 0xFFFFFFFFu;
    int dx = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) - SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    int dy = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) - SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP);
    static const int pov[3][3] = {{31500, 0, 4500}, {27000, -1, 9000}, {22500, 18000, 13500}};
    return (dx || dy) ? (uint32_t)pov[dy + 1][dx + 1] : 0xFFFFFFFFu;
}

/* Fills the state in the game's own DIDATAFORMAT: each DIOBJECTDATAFORMAT (pguid, dwOfs, dwType,
 * dwFlags) names an object by GUID and/or type and instance, and where its value goes. */
static void pad_state(uint32_t dev, uint32_t out, uint32_t size) {
    SDL_GameController *c = get_pad();
    memset(GPTR(out), 0, size);
    uint32_t fmt = DEV_FORMAT(dev), n = fmt ? MEM32(fmt + 16) : 0, objs = fmt ? MEM32(fmt + 20) : 0;
    int next[3] = {0, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = objs + 16 * i, guid = MEM32(o) ? MEM32(MEM32(o)) : 0, ofs = MEM32(o + 4), type = MEM32(o + 8);
        int kind = -1, inst;
        for (int a = 0; a < 4; a++) if (guid == axis_guid[a]) kind = OBJ_AXIS, inst = a;
        if (kind < 0) {
            if (guid == 0xA36D02F0u || (!guid && (type & 0xC))) kind = OBJ_BUTTON;
            else if (guid == 0xA36D02F2u || (!guid && (type & 0x10))) kind = OBJ_POV;
            else if (!guid && (type & 0x3)) kind = OBJ_AXIS;
            else {  /* Rx, Ry, sliders: not on this pad, so at rest (zero would be full deflection) */
                if (ofs + 4 <= size && (type & 0x3)) MEM32(out + ofs) = axis(dev, 0);
                continue;
            }
            inst = (type & 0xFFFF00u) == 0xFFFF00u ? next[kind]++ : (int)((type >> 8) & 0xFFFF);
        }
        if (ofs + 4 > size) continue;
        if (kind == OBJ_AXIS) MEM32(out + ofs) = inst < 4 ? (uint32_t)pad_axis(c, dev, inst) : axis(dev, 0);
        else if (kind == OBJ_BUTTON) *(uint8_t *)GPTR(out + ofs) = pad_button(c, inst) ? 0x80 : 0;
        else MEM32(out + ofs) = inst == 0 ? pad_pov(c) : 0xFFFFFFFFu;
    }
}

/* Testing: SH3_KEYS="12:Return,14.5:Down,20:Up+8" holds each key for 150 ms (or +seconds) at that many
 * seconds after start. */
static int scripted_key(int scancode) {
    static Uint64 t0;
    if (!t0) t0 = SDL_GetTicks64();
    const char *k = getenv("SH3_KEYS");
    double now = (SDL_GetTicks64() - t0) / 1000.0;
    while (k && *k) {
        char name[32] = "";
        double at = 0, hold = 0.15;
        int n = 0;
        if (sscanf(k, "%lf:%31[^,+]%n", &at, name, &n) < 2) break;
        k += n;
        if (*k == '+' && sscanf(k, "+%lf%n", &hold, &n) == 1) k += n;
        if (SDL_GetScancodeFromName(name) == scancode && now >= at && now < at + hold) return 1;
        if (*k == ',') k++;
    }
    return 0;
}

METHOD(dev_GetDeviceState, 3) {
    uint32_t kind = DEV_KIND(THIS), size = ARG(1), out = ARG(2);
    rt_pump_events();
    if (kind == DEV_KEYBOARD) {
        int n;
        const uint8_t *k = SDL_GetKeyboardState(&n);
        memset(GPTR(out), 0, size);
        int scripted = getenv("SH3_KEYS") != NULL;  /* a scripted run ignores the real keyboard */
        for (int i = 0; i < n; i++)
            if ((scripted ? scripted_key(i) : k[i]) && dik[i] && dik[i] < size) *(uint8_t *)GPTR(out + dik[i]) = 0x80;
    } else if (kind == DEV_PAD) pad_state(THIS, out, size);
    else memset(GPTR(out), 0, size);
    return 0;
}

static const ComEntry dev_vt[] = {
    {"IDirectInputDevice8::QueryInterface", NULL}, {"IDirectInputDevice8::AddRef", dev_AddRef},
    {"IDirectInputDevice8::Release", dev_Release}, {"IDirectInputDevice8::GetCapabilities", dev_GetCapabilities},
    {"IDirectInputDevice8::EnumObjects", dev_EnumObjects}, {"IDirectInputDevice8::GetProperty", dev_GetProperty},
    {"IDirectInputDevice8::SetProperty", dev_SetProperty}, {"IDirectInputDevice8::Acquire", dev_Acquire},
    {"IDirectInputDevice8::Unacquire", dev_Unacquire}, {"IDirectInputDevice8::GetDeviceState", dev_GetDeviceState},
    {"IDirectInputDevice8::GetDeviceData", dev_GetDeviceData}, {"IDirectInputDevice8::SetDataFormat", dev_SetDataFormat},
    {"IDirectInputDevice8::SetEventNotification", dev_SetEventNotification},
    {"IDirectInputDevice8::SetCooperativeLevel", dev_SetCooperativeLevel},
    {"IDirectInputDevice8::GetObjectInfo", dev_GetObjectInfo}, {"IDirectInputDevice8::GetDeviceInfo", dev_GetDeviceInfo},
    {"IDirectInputDevice8::RunControlPanel", NULL}, {"IDirectInputDevice8::Initialize", NULL},
    {"IDirectInputDevice8::CreateEffect", dev_CreateEffect},{"IDirectInputDevice8::EnumEffects", dev_EnumEffects},
    {"IDirectInputDevice8::GetEffectInfo", NULL}, {"IDirectInputDevice8::GetForceFeedbackState", dev_GetForceFeedbackState},
    {"IDirectInputDevice8::SendForceFeedbackCommand", dev_SendForceFeedbackCommand},
    {"IDirectInputDevice8::EnumCreatedEffectObjects", dev_EnumCreatedEffectObjects},
    {"IDirectInputDevice8::Escape", dev_Escape}, {"IDirectInputDevice8::Poll", dev_Poll},
    {"IDirectInputDevice8::SendDeviceData", NULL}, {"IDirectInputDevice8::EnumEffectsInFile", NULL},
    {"IDirectInputDevice8::WriteEffectToFile", NULL}, {"IDirectInputDevice8::BuildActionMap", NULL},
    {"IDirectInputDevice8::SetActionMap", NULL}, {"IDirectInputDevice8::GetImageInfo", NULL},
};

/* ---- IDirectInput8A ---- */
static uint32_t dev_vtable;

METHOD(di_AddRef, 1) { return 1; }
METHOD(di_Release, 1) { return 0; }
METHOD(di_CreateDevice, 4) {
    uint32_t data1 = MEM32(ARG(1));
    int kind = data1 == 0x6F1D2B61u ? DEV_KEYBOARD : data1 == 0x6F1D2B60u ? DEV_MOUSE : DEV_PAD;
    if (!dev_vtable) dev_vtable = com_vtable(dev_vt, sizeof dev_vt / sizeof *dev_vt);
    uint32_t d = com_new(dev_vtable, 32);
    DEV_KIND(d) = kind;
    DEV_MAX(d) = 65535;
    MEM32(ARG(2)) = d;
    return 0;
}
/* Offers the gamepad to a game-controller enumeration: BOOL CALLBACK cb(const DIDEVICEINSTANCEA *, void *).
 * Force-feedback enumerations included: its effects rumble the pad (dev_CreateEffect). */
METHOD(di_EnumDevices, 5) {
    uint32_t type = ARG(1) & 0xFF;
    if ((type == 0 || type == 4 || type == 0x14 || type == 0x15) && get_pad()) {
        uint32_t di = galloc(580), a[2] = {di, ARG(3)};
        fill_instance(di, DEV_PAD);
        guest_call(ARG(2), 2, a, 1);
        gfree(di);
    }
    return 0;
}
METHOD(di_GetDeviceStatus, 2) { return 0; }

static const ComEntry di_vt[] = {
    {"IDirectInput8::QueryInterface", NULL}, {"IDirectInput8::AddRef", di_AddRef},
    {"IDirectInput8::Release", di_Release}, {"IDirectInput8::CreateDevice", di_CreateDevice},
    {"IDirectInput8::EnumDevices", di_EnumDevices}, {"IDirectInput8::GetDeviceStatus", di_GetDeviceStatus},
    {"IDirectInput8::RunControlPanel", NULL}, {"IDirectInput8::Initialize", NULL},
    {"IDirectInput8::FindDevice", NULL}, {"IDirectInput8::EnumDevicesBySemantics", NULL},
    {"IDirectInput8::ConfigureDevices", NULL},
};

WINAPI(DirectInput8Create, "DirectInput8Create", 5) {
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    MEM32(ARG(3)) = com_new(com_vtable(di_vt, sizeof di_vt / sizeof *di_vt), 8);
    return 0;
}
