/* Hand-written replacements of sh3.exe functions, by address: tools/regen.sh passes this file to the
 * recompiler (--exclude-manual), which then leaves these functions out of src/recomp/gen. */
#include "../runtime/rt.h"
#include "../runtime/host.h"

/* The static CRT's memcpy (memmove semantics) and its second copy: their jump tables sit past the end
 * the disassembler finds and are indexed backwards, so the lifted copies cannot follow them. */
static void crt_memmove(void) {
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
    memmove((void *)XBOX_PTR(d), (const void *)XBOX_PTR(s), n);
    g_eax = d;
    g_esp += 4;  /* ret: cdecl */
}
void sub_00661D20(void) { crt_memmove(); }
void sub_0066F9C0(void) { crt_memmove(); }

/* ---- widescreen (ThirteenAG's SilentHill3.WidescreenFix, ported) ----
 * The display mode is always the screen's (resolution= in sh3.ini): the device is created and reset with it
 * whatever disp.ini or the options ask for. 0x72C780/84 is the mode the game wants, 0x72C788/8C the one it
 * lays the 2D out in; both feed D3DPRESENT_PARAMETERS at 0x72C664. */
extern uint32_t rt_res_w, rt_res_h;
static void force_mode(void) {
    MEM32(0x72C780) = MEM32(0x72C788) = rt_res_w;
    MEM32(0x72C784) = MEM32(0x72C78C) = rt_res_h;
}
extern void sub_004193A0_gen(void);
void sub_004193A0(void) { force_mode(); sub_004193A0_gen(); }  /* creates the device */
extern void sub_00419760_gen(void);
void sub_00419760(void) { force_mode(); sub_00419760_gen(); }  /* resets it after a mode change */
extern void sub_00416AE0_gen(void);
void sub_00416AE0(void) { ARG(0) = rt_res_w; ARG(1) = rt_res_h; sub_00416AE0_gen(); }  /* set mode (w, h) */
extern void sub_00416B70_gen(void);
void sub_00416B70(void) { ARG(0) = rt_res_w; ARG(1) = rt_res_h; sub_00416B70_gen(); }  /* set 2D size (w, h) */

/* The camera's projection scales, returned in ST(0): horizontal (0x43AB80) and vertical (0x43ABC0). Gameplay
 * widens the view (hor+); a cutscene (0x81FAB5) keeps its 4:3 width and loses some height instead. The
 * picked-up item and the mouse cursor (calls returning to 0x599C29, 0x4DDB98) keep the 4:3 width. */
#define ST0 g_fp_stack[g_fp_top & 7]
static double ws_aspect(void) { return (double)rt_res_w / rt_res_h; }
static double ws_fov(void) { return (4.0 / 3.0) / ws_aspect(); }  /* 1 at 4:3 */
extern void sub_0043AB80_gen(void);
void sub_0043AB80(void) {
    uint32_t from = MEM32(g_esp);
    sub_0043AB80_gen();
    if (!MEM8(0x81FAB5) && from != 0x599C29u && from != 0x4DDB98u) ST0 = (float)(ST0 * ws_fov());
}
extern void sub_0043ABC0_gen(void);
void sub_0043ABC0(void) { sub_0043ABC0_gen(); if (MEM8(0x81FAB5)) ST0 = (float)(ST0 / ws_fov()); }

/* float screen_x(int x) (0x682BA0): the 2D's x, in 1/16 units around 2048, to pixels. The 2D keeps a centered
 * 4:3 area; the elements meant to span the screen (ThirteenAG's list, by the call's return address) keep the
 * original mapping. */
static const uint32_t full_width_calls[] = {
    0x468B8A, 0x468BDF, 0x468C25, 0x468C71, 0x468E31, 0x468E78, 0x468EB7, 0x468F0C,
    0x47A684, 0x47A6D8, 0x47A726, 0x47A771, 0x47A7B9, 0x49FAF0, 0x5F6D27, 0x5F80F4,
};
void sub_00682BA0(void) {
    uint32_t from = MEM32(g_esp);
    int32_t x = (int32_t)ARG(0);
    int full = 0;
    for (size_t i = 0; i < sizeof full_width_calls / sizeof *full_width_calls; i++) full |= from == full_width_calls[i];
    double w = (int32_t)guest_call(0x402BA0, 0, NULL, 0);  /* the target's width */
    double left = full ? 256.0 : 192.0 * ws_aspect(), scale = full ? 1.0 / 512 : 1.0 / (384.0 * ws_aspect());
    rt_fpush(w * (x * MEMF(0x68E6E0) - MEMF(0x68E6DC) + left) * scale);
    g_esp += 4;  /* ret: cdecl */
}

/* Cave 0x401ED8, called from 0x5F1D15 in place of `fstp [ecx-0x18]; fld st(0)`: the health indicator's
 * vertices keep their place in the 4:3 area at the right of the screen. */
void sub_00401ED8(void) {
    double x = rt_fpop();
    MEMF(g_ecx - 0x18) = (float)(x * ws_fov() + (rt_res_w - rt_res_h * 4.0 / 3.0));
    rt_fpush(ST0);
    g_esp += 4;
}

/* The pad's default bindings, (button << 8 | action) shorts at 0x7214E4 that the game copies when there is no
 * savedata/key.ini: laid out as in sh2-nx. Buttons are 0x0C + dinput.c's numbering (X A B Y LB RB LT RT Back
 * Start LS RS); actions 0 search view, 1 ready weapon, 2/3 strafe left/right, 4 map, 5 action, 6 dash, 7 light,
 * 8 pause, 9 zoom, 0x0A supplies, 0x0B inventory. The other 13 entries (stick, camera, menus) stay. */
static const uint16_t pad_defaults[12] = {
    0x0C06, 0x0D05, 0x0E07, 0x0F04,  /* Switch Y dash, A action, B light (and cancel), X map */
    0x1002, 0x1103, 0x1200, 0x1301,  /* L / R strafe, ZL search view, ZR ready weapon */
    0x150B, 0x1408, 0x160A, 0x1709,  /* + inventory, − pause (and skip), stick clicks supplies / zoom */
};

/* int can_do(what) (0x495B60): the pause menu (calls returning to 0x5E88E2 / 0x5E88F7) asks it whether Save (0x0D)
 * and Load (0x0E) are allowed, and it refuses while the screen fade (type 0x897E38, done flag 0x897E3C) runs. The
 * pause freezes the fade, so pausing during a fade-in (types 5, 9, 13: back from the save screen, a load or a door)
 * left Save / Load greyed out until the menu was closed and opened again. For the pause menu a fade-in counts as
 * done; fade-outs still refuse. */
extern void sub_00495B60_gen(void);
void sub_00495B60(void) {
    uint32_t from = MEM32(g_esp);
    uint8_t type = MEM8(0x897E38), done = MEM8(0x897E3C);
    int fade_in = type == 5 || type == 9 || type == 13;
    if (fade_in && (from == 0x5E88E2u || from == 0x5E88F7u)) MEM8(0x897E3C) = 1;
    sub_00495B60_gen();
    MEM8(0x897E3C) = done;
}

/* Called once after sh3.exe is loaded: the variables tools/patch_exe.py points code at. */
void game_init(void) {
    host_arena_commit(0x0EC00000u, 0x1000);
    MEMF(0x0EC00000u) = ws_aspect() > 1.5 ? 0.0f : 0.5f;  /* cutscene borders: none on a wide screen */
    memcpy(GPTR(0x7214E4), pad_defaults, sizeof pad_defaults);
}

/* Indirect calls and the dispatch table reach the generated bodies (sub_X_gen) of wrapped functions: calls
 * by address are routed here first. */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    switch (va) {
    case 0x00661D20u: return sub_00661D20;
    case 0x0066F9C0u: return sub_0066F9C0;
    case 0x004193A0u: return sub_004193A0;
    case 0x00419760u: return sub_00419760;
    case 0x00416AE0u: return sub_00416AE0;
    case 0x00416B70u: return sub_00416B70;
    case 0x0043AB80u: return sub_0043AB80;
    case 0x0043ABC0u: return sub_0043ABC0;
    case 0x00682BA0u: return sub_00682BA0;
    case 0x00401ED8u: return sub_00401ED8;
    case 0x00495B60u: return sub_00495B60;
    }
    return NULL;
}
