/* IDirect3D8 and IDirect3DDevice8 on OpenGL. */
#include "d3d8.h"
#include <math.h>

Dev *dev;
static uint32_t d3d_obj;

/* What a state block holds: everything for CreateStateBlock, only what was set while recording for
 * Begin/EndStateBlock (SH3 records a few render states; restoring its whole snapshot brought back the
 * backbuffer's viewport while drawing into a 512x512 target, and stale textures and shaders). */
typedef struct { uint8_t rs[RS_COUNT], tss[MAX_STAGES][TSS_COUNT], xf[14], vsc[96], psc[8], material, light[8],
                 light_on[8], tex[MAX_STAGES], stream[16], ib, vs, ps, vp; } Mask;
static Mask *rec_mask;  /* while recording: Set* calls go to a scratch device and are marked here */
#define MARK(f) do { if (rec_mask) rec_mask->f = 1; } while (0)

static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t u32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ---- IDirect3D8 ---- */

extern uint32_t rt_res_w, rt_res_h;  /* the resolution sh3.ini asks for (rt_core.c): always offered as the last mode */
static uint32_t modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 960},
                                    {1280, 1024}, {1600, 900}, {1920, 1080}, {960, 720}, {1440, 1080}, {0, 0}};
/* The last two are 4:3 at the Switch's 720p and 1080p heights. */
#define N_MODES (sizeof modes / sizeof *modes)
static void sync_modes(void) { modes[N_MODES - 1][0] = rt_res_w; modes[N_MODES - 1][1] = rt_res_h; }

static void put_mode(uint32_t m, int i, uint32_t fmt) {
    sync_modes();
    MEM32(m) = modes[i][0];
    MEM32(m + 4) = modes[i][1];
    MEM32(m + 8) = 60;
    MEM32(m + 12) = fmt;
}

METHOD(d3d_AddRef, 1) { return 1; }
METHOD(d3d_Release, 1) { return 0; }
METHOD(d3d_GetAdapterCount, 1) { return 1; }
METHOD(d3d_GetAdapterIdentifier, 4) {
    uint32_t id = ARG(3);
    memset(GPTR(id), 0, 1068);
    strcpy(GPTR(id), "sh3-nx");
    strcpy(GPTR(id + 512), "OpenGL");
    MEM32(id + 1024) = 0x00010001;  /* DriverVersion */
    MEM32(id + 1028) = 0x0006000E;
    MEM32(id + 1032) = 0x10DE;      /* an NVIDIA vendor id keeps vendor-specific paths on their safe side */
    MEM32(id + 1036) = 0x0200;
    return 0;
}
/* Two formats per resolution: X8R8G8B8 then R5G6B5. */
METHOD(d3d_GetAdapterModeCount, 2) { return N_MODES * 2; }
METHOD(d3d_EnumAdapterModes, 4) {
    uint32_t i = ARG(2);
    if (i >= N_MODES * 2) return D3DERR_INVALIDCALL;
    put_mode(ARG(3), i % N_MODES, i < N_MODES ? FMT_X8R8G8B8 : FMT_R5G6B5);
    return 0;
}
METHOD(d3d_GetAdapterDisplayMode, 3) { put_mode(ARG(2), 3, FMT_X8R8G8B8); return 0; }
METHOD(d3d_CheckDeviceType, 6) { return 0; }
METHOD(d3d_CheckDeviceFormat, 7) {
    uint32_t fmt = ARG(6);
    switch (fmt) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5:
    case FMT_A4R4G4B4: case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_DXT1: case FMT_DXT2:
    case FMT_DXT3: case FMT_DXT4: case FMT_DXT5: case FMT_D16: case FMT_D24S8: case FMT_D24X8:
    case FMT_D32: case FMT_D16_LOCKABLE: case FMT_P8:
        return 0;
    }
    return D3DERR_NOTAVAILABLE;
}
METHOD(d3d_CheckDeviceMultiSampleType, 6) { return ARG(5) ? D3DERR_NOTAVAILABLE : 0; }
METHOD(d3d_CheckDepthStencilMatch, 6) { return 0; }

/* A GeForce3/4-class card with vs/ps 1.1, the hardware the PC version was made for. */
static void fill_caps(uint32_t c) {
    memset(GPTR(c), 0, 212);
    uint32_t v[53] = {0};
    v[0] = 1;                     /* D3DDEVTYPE_HAL */
    v[2] = 0x20000;               /* Caps: D3DCAPS_READ_SCANLINE */
    v[3] = 0x20000000 | 0x40000000;
    v[5] = 0x80000000u | 1 | 2;   /* PresentationIntervals: IMMEDIATE, ONE, TWO */
    v[7] = 0x10 | 0x40 | 0x80 | 0x200 | 0x400 | 0x2000 | 0x4000 | 0x10000 | 0x80000 | 0x100000;  /* DevCaps */
    v[8] = 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x400 | 0x800;              /* PrimitiveMiscCaps */
    v[9] = 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x8000 | 0x10000 | 0x20000 | 0x100000 | 0x200000;
    v[10] = v[13] = 0xFF;         /* ZCmpCaps, AlphaCmpCaps */
    v[11] = v[12] = 0x1FFF;       /* Src/DestBlendCaps */
    v[14] = 0x8 | 0x200 | 0x4000 | 0x80000;
    /* TextureCaps. No D3DPTEXTURECAPS_POW2 (0x2): GL takes any size, and with it the game rounds a
     * texture up to a power of two (the Enhanced Edition's 2732x2048 title became 4096x2048 with the
     * art misplaced). The modern GPUs EE runs on report no POW2 either. */
    /* D3DPTEXTURECAPS_ALPHAPALETTE (0x80): a P8 palette's flags byte is the alpha (d3d8_res.c). */
    v[15] = 0x1 | 0x4 | 0x8 | 0x40 | 0x80 | 0x400 | 0x800 | 0x4000 | 0x8000 | 0x10000;
    v[16] = v[17] = v[18] = 0x0100 | 0x0200 | 0x0400 | 0x010000 | 0x020000 | 0x01000000 | 0x02000000;
    v[19] = v[20] = 0x1F;         /* TextureAddressCaps */
    v[21] = 0x1F;
    v[22] = v[23] = 4096;
    v[24] = 512;
    v[25] = 8192;
    v[26] = 4096;
    v[27] = 8;
    v[28] = u32(1e10f);
    v[29] = u32(-1e8f); v[30] = u32(-1e8f); v[31] = u32(1e8f); v[32] = u32(1e8f);
    v[34] = 0xFF;                 /* StencilCaps */
    v[35] = 8 | 0x80000;          /* FVFCaps: 8 texcoords, PSIZE */
    v[36] = 0x3FFFFFF;            /* TextureOpCaps */
    v[37] = 8;                    /* MaxTextureBlendStages */
    v[38] = 4;                    /* MaxSimultaneousTextures */
    v[39] = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x40;
    v[40] = 8;                    /* MaxActiveLights */
    v[41] = 6;
    v[42] = 4;
    v[43] = 0;
    v[44] = u32(64.0f);
    v[45] = 0xFFFFF;
    v[46] = 0xFFFFF;
    v[47] = 16;
    v[48] = 256;
    v[49] = 0xFFFE0101;           /* vs_1_1 */
    v[50] = 96;
    v[51] = 0xFFFF0101;           /* ps_1_1 */
    v[52] = u32(8.0f);
    memcpy(GPTR(c), v, 212);
}
METHOD(d3d_GetDeviceCaps, 4) { fill_caps(ARG(3)); return 0; }
METHOD(d3d_GetAdapterMonitor, 2) { return 1; }

static uint32_t device_vtable(void);
static void device_reset(uint32_t pp);

/* The GL context, made by the launcher or the first CreateDevice. */
void gl_context_open(void) {
    static SDL_GLContext ctx;
    if (ctx) return;
    /* 4.5 core has glClipControl; older contexts are tried so the log says what the driver offers. */
    static const int versions[][2] = {{4, 6}, {4, 5}, {4, 3}};
    for (int i = 0; i < 3 && !ctx; i++) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, versions[i][0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, versions[i][1]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        ctx = SDL_GL_CreateContext(rt_window);
        if (!ctx) rt_log("GL %d.%d core: %s", versions[i][0], versions[i][1], SDL_GetError());
    }
    if (!ctx) rt_fatal("no OpenGL 4.3+ core context: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(1);
    gl_load();
}

METHOD(d3d_CreateDevice, 7) {
    uint32_t pp = ARG(5);
    rt_log("CreateDevice %ux%u fmt %u windowed %u depth %u/%u flags %08X", MEM32(pp), MEM32(pp + 4),
           MEM32(pp + 8), MEM32(pp + 28), MEM32(pp + 32), MEM32(pp + 36), ARG(4));
    if (!dev) {
        gl_context_open();
        rt_log("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        GLint n_ext = 0, clip_control = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n_ext);
        for (GLint i = 0; i < n_ext; i++)
            if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, i), "GL_ARB_clip_control")) clip_control = 1;
        if (!clip_control) rt_log("GL: no ARB_clip_control, the image will be upside down");
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);  /* D3D's y-down rows and [0,1] depth */
        dev = calloc(1, sizeof *dev);
        dev->obj = com_new(device_vtable(), 16);
        res_init_vtables();
        glGenFramebuffers(1, &dev->fbo);
        glGenVertexArrays(1, &dev->vao);
        glBindVertexArray(dev->vao);
        glGenBuffers(1, &dev->stream_buf);
        glGenBuffers(1, &dev->index_buf);
    }
    device_reset(pp);
    /* Like D3D8 on Windows: without D3DCREATE_FPU_PRESERVE the creating thread's x87 is switched to
     * single precision, and the whole game ran its float logic that way. */
    if (!(ARG(4) & 2)) g_fp_control_word &= ~0x300u;
    MEM32(ARG(6)) = dev->obj;
    return 0;
}

static const ComEntry d3d_vt[] = {
    {"IDirect3D8::QueryInterface", NULL}, {"IDirect3D8::AddRef", d3d_AddRef},
    {"IDirect3D8::Release", d3d_Release}, {"IDirect3D8::RegisterSoftwareDevice", NULL},
    {"IDirect3D8::GetAdapterCount", d3d_GetAdapterCount}, {"IDirect3D8::GetAdapterIdentifier", d3d_GetAdapterIdentifier},
    {"IDirect3D8::GetAdapterModeCount", d3d_GetAdapterModeCount}, {"IDirect3D8::EnumAdapterModes", d3d_EnumAdapterModes},
    {"IDirect3D8::GetAdapterDisplayMode", d3d_GetAdapterDisplayMode}, {"IDirect3D8::CheckDeviceType", d3d_CheckDeviceType},
    {"IDirect3D8::CheckDeviceFormat", d3d_CheckDeviceFormat},
    {"IDirect3D8::CheckDeviceMultiSampleType", d3d_CheckDeviceMultiSampleType},
    {"IDirect3D8::CheckDepthStencilMatch", d3d_CheckDepthStencilMatch}, {"IDirect3D8::GetDeviceCaps", d3d_GetDeviceCaps},
    {"IDirect3D8::GetAdapterMonitor", d3d_GetAdapterMonitor}, {"IDirect3D8::CreateDevice", d3d_CreateDevice},
};

WINAPI(Direct3DCreate8, "Direct3DCreate8", 1) {
    if (!d3d_obj) d3d_obj = com_new(com_vtable(d3d_vt, sizeof d3d_vt / sizeof *d3d_vt), 8);
    return d3d_obj;
}

/* ---- device state ---- */

static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static void set_defaults(int autodepth) {
    memset(dev->rs, 0, sizeof dev->rs);
    uint32_t *rs = dev->rs;
    rs[7] = autodepth;            /* ZENABLE */
    rs[8] = 3;                    /* FILLMODE solid */
    rs[9] = 2;                    /* SHADEMODE gouraud */
    rs[14] = 1;                   /* ZWRITEENABLE */
    rs[16] = 1;                   /* LASTPIXEL */
    rs[19] = 2; rs[20] = 1;       /* SRCBLEND one, DESTBLEND zero */
    rs[22] = 3;                   /* CULLMODE ccw */
    rs[23] = 4;                   /* ZFUNC lessequal */
    rs[25] = 8;                   /* ALPHAFUNC always */
    rs[37] = u32(1.0f);           /* FOGEND */
    rs[38] = u32(1.0f);           /* FOGDENSITY */
    rs[53] = rs[54] = rs[55] = 1; /* stencil ops keep */
    rs[56] = 8;                   /* STENCILFUNC always */
    rs[58] = rs[59] = 0xFFFFFFFFu;
    rs[60] = 0xFFFFFFFFu;         /* TEXTUREFACTOR */
    rs[136] = 1;                  /* CLIPPING */
    rs[137] = 1;                  /* LIGHTING */
    rs[141] = 1;                  /* COLORVERTEX */
    rs[142] = 1;                  /* LOCALVIEWER */
    rs[145] = 1;                  /* DIFFUSEMATERIALSOURCE color1 */
    rs[146] = 2;                  /* SPECULARMATERIALSOURCE color2 */
    rs[154] = u32(1.0f);          /* POINTSIZE */
    rs[158] = u32(1.0f);          /* POINTSCALE_A */
    rs[162] = 0xFFFFFFFFu;        /* MULTISAMPLEMASK */
    rs[166] = u32(64.0f);         /* POINTSIZE_MAX */
    rs[168] = 0xF;                /* COLORWRITEENABLE */
    rs[171] = 1;                  /* BLENDOP add */
    for (int s = 0; s < MAX_STAGES; s++) {
        uint32_t *t = dev->tss[s];
        memset(t, 0, TSS_COUNT * 4);
        t[1] = s ? 1 : 4;         /* COLOROP: modulate on stage 0, disable after */
        t[2] = 2; t[3] = 1;       /* COLORARG1 texture, COLORARG2 current */
        t[4] = s ? 1 : 2;         /* ALPHAOP: selectarg1 on stage 0 */
        t[5] = 2; t[6] = 1;
        t[11] = s;                /* TEXCOORDINDEX */
        t[13] = t[14] = t[25] = 1;/* ADDRESS wrap */
        t[16] = t[17] = 1;        /* MAG/MINFILTER point */
        t[21] = 1;                /* MAXANISOTROPY */
        t[26] = t[27] = 1;        /* ARG0 current */
        t[28] = 1;                /* RESULTARG current */
        memcpy(dev->texm[s], identity, 64);
    }
    for (int i = 0; i < 4; i++) memcpy(dev->world[i], identity, 64);
    memcpy(dev->view, identity, 64);
    memcpy(dev->proj, identity, 64);
    memset(dev->material, 0, sizeof dev->material);
    memset(dev->light_on, 0, sizeof dev->light_on);
    memset(dev->tex, 0, sizeof dev->tex);
    memset(dev->stream, 0, sizeof dev->stream);
    dev->ib = dev->vs = dev->ps = 0;
}

static void device_reset(uint32_t pp) {
    uint32_t w = MEM32(pp), h = MEM32(pp + 4);
    if (!w || !h) { w = 1280; h = 720; }
    dev->bb_w = w;
    dev->bb_h = h;
    if (dev->rt) res_release(dev->rt);
    if (dev->ds) res_release(dev->ds);
    if (dev->backbuffer) res_release(dev->backbuffer);
    if (dev->autodepth) res_release(dev->autodepth);
    dev->backbuffer = res_surface(w, h, FMT_A8R8G8B8, 1, 0);
    dev->autodepth = MEM32(pp + 32) ? res_surface(w, h, MEM32(pp + 36), 0, 1) : 0;
    /* Bound as the render target and depth surface too: each binding holds its own reference. */
    dev->rt = dev->backbuffer;
    res_addref(dev->rt);
    dev->ds = dev->autodepth;
    if (dev->ds) res_addref(dev->ds);
    set_defaults(dev->autodepth != 0);
    dev->vp_x = dev->vp_y = 0;
    dev->vp_w = w;
    dev->vp_h = h;
    dev->vp_minz = 0;
    dev->vp_maxz = 1;
    res_bind_targets();
}

/* ---- IDirect3DDevice8 ---- */

METHOD(dev_QueryInterface, 3) { return E_NOINTERFACE; }
METHOD(dev_AddRef, 1) { return 1; }
METHOD(dev_Release, 1) { return 0; }
METHOD(dev_TestCooperativeLevel, 1) { return 0; }
METHOD(dev_GetAvailableTextureMem, 1) { return 256u << 20; }
METHOD(dev_ResourceManagerDiscardBytes, 2) { return 0; }
METHOD(dev_GetDirect3D, 2) { MEM32(ARG(1)) = d3d_obj; return 0; }
METHOD(dev_GetDeviceCaps, 2) { fill_caps(ARG(1)); return 0; }
METHOD(dev_GetDisplayMode, 2) {
    uint32_t m = ARG(1);
    MEM32(m) = dev->bb_w; MEM32(m + 4) = dev->bb_h; MEM32(m + 8) = 60; MEM32(m + 12) = FMT_X8R8G8B8;
    return 0;
}
METHOD(dev_GetCreationParameters, 2) {
    uint32_t p = ARG(1);
    MEM32(p) = 0; MEM32(p + 4) = 1; MEM32(p + 8) = 0x00010001; MEM32(p + 12) = 0x40;
    return 0;
}
METHOD(dev_SetCursorProperties, 4) { return 0; }
METHOD(dev_SetCursorPosition, 4) { return 0; }
METHOD(dev_ShowCursor, 2) { return 0; }
METHOD(dev_Reset, 2) { device_reset(ARG(1)); return 0; }

METHOD(dev_Present, 5) {
    int ww, wh;
    SDL_GL_GetDrawableSize(rt_window, &ww, &wh);
    Res *bb = res_of(dev->backbuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, dev->fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bb->tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* Keep the game's aspect ratio inside the window; rows flip because the window's row 0 is its bottom. */
    float s = fminf((float)ww / bb->w, (float)wh / bb->h);
    int dw = bb->w * s, dh = bb->h * s, dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBlitFramebuffer(0, 0, bb->w, bb->h, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    res_snapshot_front();
    static int frame;
    rt_frame++;
    /* debugging: SH3_SHOT=dir writes frames 0-9, then every SH3_SHOT_EVERY-th (30), as PPM */
    const char *shot = getenv("SH3_SHOT"), *every = getenv("SH3_SHOT_EVERY");
    if (shot && (frame++ < 10 || frame % (every ? atoi(every) : 30) == 1)) {
        char path[512];
        snprintf(path, sizeof path, "%s/frame%05d.ppm", shot, frame - 1);
        uint8_t *px = malloc(bb->w * bb->h * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, bb->w, bb->h, GL_RGB, GL_UNSIGNED_BYTE, px);
        FILE *f = fopen(path, "wb");
        if (f) { fprintf(f, "P6 %u %u 255\n", bb->w, bb->h); fwrite(px, 3, bb->w * bb->h, f); fclose(f); }
        free(px);
    }
    SDL_GL_SwapWindow(rt_window);
    static Uint64 stat_t0;
    static int stat_frames;
    if (getenv("SH3_STATS") && ++stat_frames == 120) {  /* debugging: FPS and guest heap */
        Uint64 now = SDL_GetTicks64();
        if (stat_t0) rt_log("stats: %.1f fps, guest heap %u MB", 120000.0 / (now - stat_t0), rt_heap_used() >> 20);
        stat_t0 = now;
        stat_frames = 0;
    }
    /* debugging: SH3_POKE=frame:va=value writes a guest dword at that frame */
    const char *poke = getenv("SH3_POKE");
    unsigned pf, pva, pval;
    if (poke && sscanf(poke, "%u:%x=%x", &pf, &pva, &pval) == 3 && rt_frame >= (int)pf && rt_frame < (int)pf + 30) {
        MEM32(pva) = pval;
        if (rt_frame == (int)pf) rt_log("poke %08X = %08X", pva, pval);
    }
    /* debugging: SH3_PEEK=va,va,... logs those guest dwords every 60 frames */
    const char *peek = getenv("SH3_PEEK");
    if (peek && rt_frame % 60 == 0) {
        char line[512];
        int n = snprintf(line, sizeof line, "peek frame %d:", rt_frame);
        for (const char *p = peek; *p && n < (int)sizeof line - 24; p += strcspn(p, ","), p += *p == ',') {
            uint32_t va = (uint32_t)strtoul(p, NULL, 16);
            n += snprintf(line + n, sizeof line - n, " %08X=%08X", va, MEM32(va));
        }
        rt_log("%s", line);
    }
    rt_pump_events();
    res_bind_targets();
    return 0;
}
METHOD(dev_GetBackBuffer, 4) { res_addref(dev->backbuffer); MEM32(ARG(3)) = dev->backbuffer; return 0; }
METHOD(dev_GetRasterStatus, 2) { MEM32(ARG(1)) = 1; MEM32(ARG(1) + 4) = 0; return 0; }
METHOD(dev_SetGammaRamp, 3) { return 0; }
METHOD(dev_GetGammaRamp, 2) {
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) *(uint16_t *)GPTR(ARG(1) + c * 512 + i * 2) = i * 257;
    return 0;
}
METHOD(dev_CreateTexture, 8) {
    MEM32(ARG(7)) = res_texture(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0);
    return MEM32(ARG(7)) ? 0 : D3DERR_INVALIDCALL;
}
METHOD(dev_CreateCubeTexture, 7) {
    MEM32(ARG(6)) = res_texture(ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 1);
    return MEM32(ARG(6)) ? 0 : D3DERR_INVALIDCALL;
}
METHOD(dev_CreateVertexBuffer, 6) { MEM32(ARG(5)) = res_buffer(RES_VB, ARG(1), ARG(2), ARG(3), ARG(4)); return 0; }
METHOD(dev_CreateIndexBuffer, 6) { MEM32(ARG(5)) = res_buffer(RES_IB, ARG(1), ARG(2), ARG(3), ARG(4)); return 0; }
METHOD(dev_CreateRenderTarget, 7) { MEM32(ARG(6)) = res_surface(ARG(1), ARG(2), ARG(3), 1, 0); return 0; }
METHOD(dev_CreateDepthStencilSurface, 6) { MEM32(ARG(5)) = res_surface(ARG(1), ARG(2), ARG(3), 0, 1); return 0; }
METHOD(dev_CreateImageSurface, 5) { MEM32(ARG(4)) = res_surface(ARG(1), ARG(2), ARG(3), 0, 0); return 0; }
METHOD(dev_SetRenderTarget, 3) {
    if (ARG(1)) { res_addref(ARG(1)); if (dev->rt) res_release(dev->rt); dev->rt = ARG(1); }
    if (ARG(2)) res_addref(ARG(2));
    if (dev->ds) res_release(dev->ds);
    dev->ds = ARG(2);
    res_bind_targets();
    Res *r = res_of(dev->rt);  /* D3D8 resets the viewport to the new target */
    dev->vp_x = dev->vp_y = 0;
    dev->vp_w = r->w;
    dev->vp_h = r->h;
    return 0;
}
METHOD(dev_GetRenderTarget, 2) { res_addref(dev->rt); MEM32(ARG(1)) = dev->rt; return 0; }
METHOD(dev_GetDepthStencilSurface, 2) {
    if (dev->ds) res_addref(dev->ds);
    MEM32(ARG(1)) = dev->ds;
    return dev->ds ? 0 : 0x88760870u;  /* D3DERR_NOTFOUND */
}
METHOD(dev_BeginScene, 1) { return 0; }
METHOD(dev_EndScene, 1) { return 0; }

static void apply_viewport_scissor(int scissor);

METHOD(dev_Clear, 7) {
    uint32_t flags = ARG(3), c = ARG(4);
    GLbitfield mask = 0;
    if (flags & 1) {
        glClearColor((c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f);
        glColorMask(1, 1, 1, 1);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if ((flags & 2) && dev->ds) { glClearDepth(f32(ARG(5))); glDepthMask(1); mask |= GL_DEPTH_BUFFER_BIT; }
    if ((flags & 4) && dev->ds) { glClearStencil(ARG(6)); glStencilMask(0xFF); mask |= GL_STENCIL_BUFFER_BIT; }
    apply_viewport_scissor(1);
    if (ARG(1) && ARG(2))
        for (uint32_t i = 0; i < ARG(1); i++) {
            int32_t *r = GPTR(ARG(2) + 16 * i);  /* D3DRECT x1 y1 x2 y2 */
            glScissor(r[0], r[1], r[2] - r[0], r[3] - r[1]);
            glClear(mask);
        }
    else glClear(mask);
    return 0;
}

/* Transforms in state block masks: view, projection, texture 0-7, world 0-3. */
static int transform_id(uint32_t t) {
    return t == 2 ? 0 : t == 3 ? 1 : t >= 16 && t < 24 ? 2 + (int)(t - 16) : t >= 256 && t < 260 ? 10 + (int)(t - 256) : -1;
}
static float *transform_slot(uint32_t t) {
    if (t == 2) return dev->view;
    if (t == 3) return dev->proj;
    if (t >= 16 && t < 24) return dev->texm[t - 16];
    if (t >= 256 && t < 260) return dev->world[t - 256];
    return NULL;
}
METHOD(dev_SetTransform, 3) {
    float *m = transform_slot(ARG(1));
    if (m) { memcpy(m, GPTR(ARG(2)), 64); MARK(xf[transform_id(ARG(1))]); }
    return 0;
}
METHOD(dev_GetTransform, 3) {
    float *m = transform_slot(ARG(1));
    memcpy(GPTR(ARG(2)), m ? m : identity, 64);
    return 0;
}
METHOD(dev_MultiplyTransform, 3) {
    float *m = transform_slot(ARG(1)), r[16];
    const float *b = GPTR(ARG(2));
    if (!m) return 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = m[i * 4] * b[j] + m[i * 4 + 1] * b[4 + j] + m[i * 4 + 2] * b[8 + j] + m[i * 4 + 3] * b[12 + j];
    memcpy(m, r, 64);
    MARK(xf[transform_id(ARG(1))]);
    return 0;
}
METHOD(dev_SetViewport, 2) {
    uint32_t v = ARG(1);
    dev->vp_x = MEM32(v); dev->vp_y = MEM32(v + 4); dev->vp_w = MEM32(v + 8); dev->vp_h = MEM32(v + 12);
    dev->vp_minz = MEMF(v + 16); dev->vp_maxz = MEMF(v + 20);
    MARK(vp);
    return 0;
}
METHOD(dev_GetViewport, 2) {
    uint32_t v = ARG(1);
    MEM32(v) = dev->vp_x; MEM32(v + 4) = dev->vp_y; MEM32(v + 8) = dev->vp_w; MEM32(v + 12) = dev->vp_h;
    MEMF(v + 16) = dev->vp_minz; MEMF(v + 20) = dev->vp_maxz;
    return 0;
}
METHOD(dev_SetMaterial, 2) { memcpy(dev->material, GPTR(ARG(1)), 68); MARK(material); return 0; }
METHOD(dev_GetMaterial, 2) { memcpy(GPTR(ARG(1)), dev->material, 68); return 0; }
/* D3DLIGHT8: Type, Diffuse, Specular, Ambient, Position, Direction, Range, Falloff, Att0-2, Theta, Phi. */
METHOD(dev_SetLight, 3) {
    if (ARG(1) < 8) {
        MARK(light[ARG(1)]);
        Light *l = &dev->light[ARG(1)];
        uint32_t p = ARG(2);
        l->type = MEM32(p);
        memcpy(l->diffuse, GPTR(p + 4), 16);
        memcpy(l->specular, GPTR(p + 20), 16);
        memcpy(l->ambient, GPTR(p + 36), 16);
        memcpy(l->pos, GPTR(p + 52), 12);
        memcpy(l->dir, GPTR(p + 64), 12);
        memcpy(&l->range, GPTR(p + 76), 28);
    }
    return 0;
}
METHOD(dev_LightEnable, 3) { if (ARG(1) < 8) { dev->light_on[ARG(1)] = ARG(2) != 0; MARK(light_on[ARG(1)]); } return 0; }
METHOD(dev_GetLightEnable, 3) { MEM32(ARG(2)) = ARG(1) < 8 && dev->light_on[ARG(1)]; return 0; }
METHOD(dev_SetClipPlane, 3) { return 0; }
METHOD(dev_SetRenderState, 3) { if (ARG(1) < RS_COUNT) { dev->rs[ARG(1)] = ARG(2); MARK(rs[ARG(1)]); } return 0; }
METHOD(dev_GetRenderState, 3) { MEM32(ARG(2)) = ARG(1) < RS_COUNT ? dev->rs[ARG(1)] : 0; return 0; }

/* ---- state blocks ---- */
typedef struct { uint32_t rs[RS_COUNT], tss[MAX_STAGES][TSS_COUNT]; float world[4][16], view[16], proj[16],
                 texm[MAX_STAGES][16], vsc[96][4], psc[8][4], material[17]; Light light[8]; int light_on[8];
                 uint32_t tex[MAX_STAGES], stream[16], stride[16], ib, base_vertex, vs, ps;
                 float vp[6]; Mask m; } Block;
static Block *blocks[1024];

/* Copies the block's marked states from the device (capture) or to it (apply). */
#define XFER(on, d, b) do { if (on) { if (apply) memcpy(&(d), &(b), sizeof(d)); else memcpy(&(b), &(d), sizeof(d)); } } while (0)
static void block_xfer(Block *b, int apply) {
    const Mask *m = &b->m;
    for (int i = 0; i < RS_COUNT; i++) XFER(m->rs[i], dev->rs[i], b->rs[i]);
    for (int s = 0; s < MAX_STAGES; s++) {
        for (int i = 0; i < TSS_COUNT; i++) XFER(m->tss[s][i], dev->tss[s][i], b->tss[s][i]);
        XFER(m->xf[2 + s], dev->texm[s], b->texm[s]);
        XFER(m->tex[s], dev->tex[s], b->tex[s]);
    }
    XFER(m->xf[0], dev->view, b->view);
    XFER(m->xf[1], dev->proj, b->proj);
    for (int i = 0; i < 4; i++) XFER(m->xf[10 + i], dev->world[i], b->world[i]);
    for (int i = 0; i < 96; i++) XFER(m->vsc[i], dev->vsc[i], b->vsc[i]);
    for (int i = 0; i < 8; i++) {
        XFER(m->psc[i], dev->psc[i], b->psc[i]);
        XFER(m->light[i], dev->light[i], b->light[i]);
        XFER(m->light_on[i], dev->light_on[i], b->light_on[i]);
    }
    XFER(m->material, dev->material, b->material);
    for (int i = 0; i < 16; i++) { XFER(m->stream[i], dev->stream[i], b->stream[i]); XFER(m->stream[i], dev->stride[i], b->stride[i]); }
    XFER(m->ib, dev->ib, b->ib);
    XFER(m->ib, dev->base_vertex, b->base_vertex);
    XFER(m->vs, dev->vs, b->vs);
    XFER(m->ps, dev->ps, b->ps);
    float vp[6] = {dev->vp_x, dev->vp_y, dev->vp_w, dev->vp_h, dev->vp_minz, dev->vp_maxz};
    XFER(m->vp, vp, b->vp);
    dev->vp_x = vp[0]; dev->vp_y = vp[1]; dev->vp_w = vp[2]; dev->vp_h = vp[3]; dev->vp_minz = vp[4]; dev->vp_maxz = vp[5];
}
#undef XFER

static uint32_t block_new(const Mask *m) {
    for (uint32_t i = 1; i < 1024; i++)
        if (!blocks[i]) {
            blocks[i] = calloc(1, sizeof(Block));
            if (m) blocks[i]->m = *m;
            else memset(&blocks[i]->m, 1, sizeof(Mask));  /* ponytail: every D3DSBT type captures everything */
            block_xfer(blocks[i], 0);
            return i;
        }
    rt_fatal("too many state blocks");
}
/* Recording leaves the device as it was: the Set* calls in between go to a scratch copy of it. */
static Dev rec_scratch, *rec_real;
static Mask rec_set;
METHOD(dev_BeginStateBlock, 1) {
    rec_scratch = *dev;
    rec_real = dev;
    dev = &rec_scratch;
    memset(&rec_set, 0, sizeof rec_set);
    rec_mask = &rec_set;
    return 0;
}
METHOD(dev_EndStateBlock, 2) {
    if (!rec_mask) return D3DERR_INVALIDCALL;
    MEM32(ARG(1)) = block_new(&rec_set);
    rec_mask = NULL;
    dev = rec_real;
    return 0;
}
METHOD(dev_CreateStateBlock, 3) { MEM32(ARG(2)) = block_new(NULL); return 0; }
METHOD(dev_ApplyStateBlock, 2) { if (ARG(1) < 1024 && blocks[ARG(1)]) block_xfer(blocks[ARG(1)], 1); return 0; }
METHOD(dev_CaptureStateBlock, 2) { if (ARG(1) < 1024 && blocks[ARG(1)]) block_xfer(blocks[ARG(1)], 0); return 0; }
METHOD(dev_DeleteStateBlock, 2) { if (ARG(1) < 1024) { free(blocks[ARG(1)]); blocks[ARG(1)] = NULL; } return 0; }

METHOD(dev_SetClipStatus, 2) { return 0; }
METHOD(dev_GetClipStatus, 2) { memset(GPTR(ARG(1)), 0, 8); return 0; }
METHOD(dev_GetTexture, 3) {
    uint32_t t = ARG(1) < MAX_STAGES ? dev->tex[ARG(1)] : 0;
    if (t) res_addref(t);
    MEM32(ARG(2)) = t;
    return 0;
}
METHOD(dev_SetTexture, 3) {
    if (ARG(1) < MAX_STAGES) MARK(tex[ARG(1)]);
    if (ARG(1) < MAX_STAGES && dev->tex[ARG(1)] != ARG(2)) {
        if (ARG(2) && !rec_mask) res_addref(ARG(2));  /* a recorded block holds no references */
        if (dev->tex[ARG(1)] && !rec_mask) res_release(dev->tex[ARG(1)]);
        dev->tex[ARG(1)] = ARG(2);
    }
    return 0;
}
METHOD(dev_GetTextureStageState, 4) {
    MEM32(ARG(3)) = ARG(1) < MAX_STAGES && ARG(2) < TSS_COUNT ? dev->tss[ARG(1)][ARG(2)] : 0;
    return 0;
}
METHOD(dev_SetTextureStageState, 4) {
    if (ARG(1) < MAX_STAGES && ARG(2) < TSS_COUNT) { dev->tss[ARG(1)][ARG(2)] = ARG(3); MARK(tss[ARG(1)][ARG(2)]); }
    return 0;
}
METHOD(dev_ValidateDevice, 2) { MEM32(ARG(1)) = 1; return 0; }
/* Palette numbers go up to 65535 in D3D8. The entries are device data, not state: they are kept on the
 * real device while a state block records. */
METHOD(dev_SetPaletteEntries, 3) {
    uint32_t n = ARG(1);
    Dev *d = rec_real ? rec_real : dev;
    if (n >= 65536) return D3DERR_INVALIDCALL;
    if (!d->palette[n]) d->palette[n] = malloc(1024);
    memcpy(d->palette[n], GPTR(ARG(2)), 1024);
    d->pal_version[n]++;
    return 0;
}
METHOD(dev_GetPaletteEntries, 3) {
    uint32_t n = ARG(1);
    if (n >= 65536) return D3DERR_INVALIDCALL;
    if (dev->palette[n]) memcpy(GPTR(ARG(2)), dev->palette[n], 1024);
    else memset(GPTR(ARG(2)), 0, 1024);
    return 0;
}
METHOD(dev_SetCurrentTexturePalette, 2) { if (ARG(1) >= 65536) return D3DERR_INVALIDCALL; dev->cur_palette = ARG(1); return 0; }
METHOD(dev_GetCurrentTexturePalette, 2) { MEM32(ARG(1)) = dev->cur_palette; return 0; }

/* ---- drawing ---- */

static GLenum blend_factor(uint32_t b) {
    static const GLenum t[] = {GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                               GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                               GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE};
    return b < 12 ? t[b] : GL_ONE;
}
static GLenum stencil_op(uint32_t o) {
    static const GLenum t[] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    return o < 9 ? t[o] : GL_KEEP;
}
static GLenum cmp_func(uint32_t c) { return c >= 1 && c <= 8 ? GL_NEVER + c - 1 : GL_ALWAYS; }

static void apply_viewport_scissor(int scissor) {
    glViewport(dev->vp_x, dev->vp_y, dev->vp_w, dev->vp_h);
    glDepthRange(dev->vp_minz, dev->vp_maxz);
    if (scissor) { glEnable(GL_SCISSOR_TEST); glScissor(dev->vp_x, dev->vp_y, dev->vp_w, dev->vp_h); }
    else glDisable(GL_SCISSOR_TEST);
}

static void apply_render_states(void) {
    uint32_t *rs = dev->rs;
    apply_viewport_scissor(0);
    if (rs[7] && dev->ds) { glEnable(GL_DEPTH_TEST); glDepthFunc(cmp_func(rs[23])); }
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(rs[14] && dev->ds);
    if (rs[27]) {
        glEnable(GL_BLEND);
        uint32_t s = rs[19], d = rs[20];
        if (s == 12) { s = 5; d = 6; }        /* BOTHSRCALPHA */
        else if (s == 13) { s = 6; d = 5; }   /* BOTHINVSRCALPHA */
        glBlendFuncSeparate(blend_factor(s), blend_factor(d), blend_factor(s), blend_factor(d));
        static const GLenum op[] = {GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
        glBlendEquation(rs[171] < 6 ? op[rs[171]] : GL_FUNC_ADD);
    } else glDisable(GL_BLEND);
    if (rs[22] == 1) glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CW);
        glCullFace(rs[22] == 2 ? GL_FRONT : GL_BACK);  /* CULL_CW drops clockwise (front) faces */
    }
    if (rs[52] && dev->ds) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(cmp_func(rs[56]), rs[57], rs[58]);
        glStencilOpSeparate(GL_FRONT_AND_BACK, stencil_op(rs[53]), stencil_op(rs[54]), stencil_op(rs[55]));
        glStencilMask(rs[59]);
    } else glDisable(GL_STENCIL_TEST);
    uint32_t cw = rs[168];
    glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    glPolygonMode(GL_FRONT_AND_BACK, rs[8] == 1 ? GL_POINT : rs[8] == 2 ? GL_LINE : GL_FILL);
    if (rs[47]) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(0, -(float)rs[47]); }  /* ZBIAS */
    else glDisable(GL_POLYGON_OFFSET_FILL);
}

static GLenum prim_mode(uint32_t t) {
    static const GLenum m[] = {GL_POINTS, GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
    return t < 7 ? m[t] : GL_TRIANGLES;
}
static uint32_t prim_verts(uint32_t t, uint32_t n) {
    switch (t) {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    default: return n + 2;
    }
}

/* Vertex attribute locations: D3DVSDE numbering for the fixed pipeline (0 position, 1 blend weights,
 * 2 blend indices, 3 normal, 4 point size, 5 diffuse, 6 specular, 7-14 texcoords); v# for shaders. */
static void attrib(int loc, int type, GLsizei stride, uintptr_t off) {
    glEnableVertexAttribArray(loc);
    switch (type) {
    case 0: case 1: case 2: case 3: glVertexAttribPointer(loc, type + 1, GL_FLOAT, 0, stride, (void *)off); break;
    case 4: glVertexAttribPointer(loc, GL_BGRA, GL_UNSIGNED_BYTE, 1, stride, (void *)off); break;  /* D3DCOLOR */
    case 5: glVertexAttribPointer(loc, 4, GL_UNSIGNED_BYTE, 0, stride, (void *)off); break;        /* UBYTE4 */
    case 6: glVertexAttribPointer(loc, 2, GL_SHORT, 0, stride, (void *)off); break;
    case 7: glVertexAttribPointer(loc, 4, GL_SHORT, 0, stride, (void *)off); break;
    }
}

/* Sets up attributes for stream 0 (FVF) or the declaration's streams. base: GL buffer offsets of each
 * stream's vertex 0; up_stride: stride for DrawPrimitiveUP. Returns 1 when vertices are pretransformed. */
static int setup_attribs(const uintptr_t *base, const GLuint *bufs, uint32_t up_stride, int *present) {
    for (int i = 0; i < 16; i++) { glDisableVertexAttribArray(i); present[i] = 0; }
    if (dev->vs < 0xF0000000u) {
        uint32_t fvf = dev->vs, stride = up_stride ? up_stride : dev->stride[0], off = 0;
        if (!bufs[0]) return 0;
        glBindBuffer(GL_ARRAY_BUFFER, bufs[0]);
        uint32_t pos = fvf & 0xE;
        int rhw = pos == 4;
        attrib(0, rhw ? 3 : 2, stride, base[0]);
        present[0] = 1;
        off = rhw ? 16 : 12;
        if (pos >= 6 && pos <= 0xE) {
            int nb = (pos - 4) / 2;  /* XYZB1..B5 */
            if (fvf & 0x1000) nb--;  /* last beta is UBYTE4 indices */
            if (nb > 0) { attrib(1, nb - 1, stride, base[0] + off); present[1] = 1; off += 4 * nb; }
            if (fvf & 0x1000) { attrib(2, 5, stride, base[0] + off); present[2] = 1; off += 4; }
        }
        if (fvf & 0x10) { attrib(3, 2, stride, base[0] + off); present[3] = 1; off += 12; }
        if (fvf & 0x20) { attrib(4, 0, stride, base[0] + off); present[4] = 1; off += 4; }
        if (fvf & 0x40) { attrib(5, 4, stride, base[0] + off); present[5] = 1; off += 4; }
        if (fvf & 0x80) { attrib(6, 4, stride, base[0] + off); present[6] = 1; off += 4; }
        int ntex = (fvf >> 8) & 0xF;
        for (int t = 0; t < ntex && t < 8; t++) {
            static const int size[4] = {2, 3, 4, 1};
            int n = size[(fvf >> (16 + 2 * t)) & 3];
            attrib(7 + t, n - 1, stride, base[0] + off);
            present[7 + t] = 1;
            off += 4 * n;
        }
        return rhw;
    }
    const VsInput *in = shader_vs_inputs(dev->vs);
    for (int r = 0; r < 16; r++) {
        if (in[r].stream < 0 || !bufs[in[r].stream]) continue;
        int s = in[r].stream;
        glBindBuffer(GL_ARRAY_BUFFER, bufs[s]);
        attrib(r, in[r].type, up_stride ? up_stride : dev->stride[s], base[s] + in[r].offset);
        present[r] = 1;
    }
    return 0;
}

static void bind_textures(void) {
    for (int s = 0; s < MAX_STAGES; s++) {
        glActiveTexture(GL_TEXTURE0 + s);
        Res *t = res_of(dev->tex[s]);
        if (t) res_upload_dirty(t);
        glBindTexture(t && t->kind == RES_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, t ? t->tex : 0);
    }
}

static void draw_setup(const uintptr_t *base, const GLuint *bufs, uint32_t up_stride) {
    int present[16];
    apply_render_states();
    bind_textures();
    int rhw = setup_attribs(base, bufs, up_stride, present);
    /* Screen-space vertices are never clipped by depth in D3D; UI quads sit at z = 0 or 1 exactly,
     * which nouveau's rounding pushes just outside the clip volume. */
    if (rhw) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP);
    shader_bind_for_draw(rhw, dev->vs, present);
}

/* The GL buffer of a vertex or index buffer, created and refreshed from the guest copy on the render
 * thread. */
static GLuint gl_buffer(Res *b, GLenum target) {
    if (!b->buf) { glGenBuffers(1, &b->buf); b->dirty = 1; }
    glBindBuffer(target, b->buf);
    if (b->dirty) {
        glBufferData(target, b->size, GPTR(b->mem), GL_DYNAMIC_DRAW);
        b->dirty = 0;
    }
    return b->buf;
}

static void stream_bases(uintptr_t *base, GLuint *bufs) {
    for (int s = 0; s < 16; s++) {
        Res *vb = res_of(dev->stream[s]);
        base[s] = 0;
        bufs[s] = vb ? gl_buffer(vb, GL_ARRAY_BUFFER) : 0;
    }
}

METHOD(dev_DrawPrimitive, 4) {
    uintptr_t base[16];
    GLuint bufs[16];
    stream_bases(base, bufs);
    draw_setup(base, bufs, 0);
    glDrawArrays(prim_mode(ARG(1)), ARG(2), prim_verts(ARG(1), ARG(3)));
    return 0;
}

METHOD(dev_DrawIndexedPrimitive, 6) {
    Res *ib = res_of(dev->ib);
    if (!ib) return D3DERR_INVALIDCALL;
    uintptr_t base[16];
    GLuint bufs[16];
    stream_bases(base, bufs);
    draw_setup(base, bufs, 0);
    gl_buffer(ib, GL_ELEMENT_ARRAY_BUFFER);
    int is32 = ib->fmt == FMT_INDEX32;
    glDrawElementsBaseVertex(prim_mode(ARG(1)), prim_verts(ARG(1), ARG(5)), is32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                             (void *)(uintptr_t)(ARG(4) * (is32 ? 4 : 2)), dev->base_vertex);
    return 0;
}

/* The UP draws stream vertices straight from guest memory; D3D8 then unbinds stream 0 and the indices. */
static void up_upload(uint32_t data, uint32_t bytes) {
    glBindBuffer(GL_ARRAY_BUFFER, dev->stream_buf);
    glBufferData(GL_ARRAY_BUFFER, bytes, GPTR(data), GL_STREAM_DRAW);
}
METHOD(dev_DrawPrimitiveUP, 5) {
    uint32_t n = prim_verts(ARG(1), ARG(2)), stride = ARG(4);
    up_upload(ARG(3), n * stride);
    uintptr_t base[16] = {0};
    GLuint bufs[16] = {dev->stream_buf};
    draw_setup(base, bufs, stride);
    glDrawArrays(prim_mode(ARG(1)), 0, n);
    if (dev->stream[0]) res_release(dev->stream[0]);
    dev->stream[0] = 0;
    return 0;
}
METHOD(dev_DrawIndexedPrimitiveUP, 9) {
    uint32_t nidx = prim_verts(ARG(1), ARG(4)), isz = ARG(6) == FMT_INDEX32 ? 4 : 2, stride = ARG(8);
    up_upload(ARG(7), (ARG(2) + ARG(3)) * stride);
    uintptr_t base[16] = {0};
    GLuint bufs[16] = {dev->stream_buf};
    draw_setup(base, bufs, stride);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dev->index_buf);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, nidx * isz, GPTR(ARG(5)), GL_STREAM_DRAW);
    glDrawElements(prim_mode(ARG(1)), nidx, isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, 0);
    if (dev->stream[0]) res_release(dev->stream[0]);
    dev->stream[0] = 0;
    if (dev->ib) res_release(dev->ib);
    dev->ib = 0;
    return 0;
}

METHOD(dev_CreateVertexShader, 5) { MEM32(ARG(3)) = shader_create_vs(ARG(1), ARG(2)); return 0; }
METHOD(dev_SetVertexShader, 2) { dev->vs = ARG(1); MARK(vs); return 0; }
METHOD(dev_GetVertexShader, 2) { MEM32(ARG(1)) = dev->vs; return 0; }
METHOD(dev_DeleteVertexShader, 2) { shader_delete_vs(ARG(1)); return 0; }
METHOD(dev_SetVertexShaderConstant, 4) {
    uint32_t r = ARG(1), n = ARG(3);
    if (r < 96) memcpy(dev->vsc[r], GPTR(ARG(2)), 16 * (r + n > 96 ? 96 - r : n));
    for (uint32_t i = r; i < r + n && i < 96; i++) MARK(vsc[i]);
    return 0;
}
METHOD(dev_GetVertexShaderConstant, 4) {
    uint32_t r = ARG(1), n = ARG(3);
    if (r < 96) memcpy(GPTR(ARG(2)), dev->vsc[r], 16 * (r + n > 96 ? 96 - r : n));
    return 0;
}
METHOD(dev_SetStreamSource, 4) {
    uint32_t s = ARG(1);
    if (s >= 16) return D3DERR_INVALIDCALL;
    if (ARG(2) && !rec_mask) res_addref(ARG(2));
    if (dev->stream[s] && !rec_mask) res_release(dev->stream[s]);
    dev->stream[s] = ARG(2);
    MARK(stream[s]);
    dev->stride[s] = ARG(3);
    return 0;
}
METHOD(dev_GetStreamSource, 4) {
    uint32_t s = ARG(1) & 15;
    if (dev->stream[s]) res_addref(dev->stream[s]);
    MEM32(ARG(2)) = dev->stream[s];
    MEM32(ARG(3)) = dev->stride[s];
    return 0;
}
METHOD(dev_SetIndices, 3) {
    MARK(ib);
    if (ARG(1) && !rec_mask) res_addref(ARG(1));
    if (dev->ib && !rec_mask) res_release(dev->ib);
    dev->ib = ARG(1);
    dev->base_vertex = ARG(2);
    return 0;
}
METHOD(dev_GetIndices, 3) {
    if (dev->ib) res_addref(dev->ib);
    MEM32(ARG(1)) = dev->ib;
    MEM32(ARG(2)) = dev->base_vertex;
    return 0;
}
METHOD(dev_CreatePixelShader, 3) { MEM32(ARG(2)) = shader_create_ps(ARG(1)); return 0; }
METHOD(dev_SetPixelShader, 2) { dev->ps = ARG(1); MARK(ps); return 0; }
METHOD(dev_GetPixelShader, 2) { MEM32(ARG(1)) = dev->ps; return 0; }
METHOD(dev_DeletePixelShader, 2) { shader_delete_ps(ARG(1)); return 0; }
METHOD(dev_SetPixelShaderConstant, 4) {
    uint32_t r = ARG(1), n = ARG(3);
    if (r < 8) memcpy(dev->psc[r], GPTR(ARG(2)), 16 * (r + n > 8 ? 8 - r : n));
    for (uint32_t i = r; i < r + n && i < 8; i++) MARK(psc[i]);
    return 0;
}
METHOD(dev_GetPixelShaderConstant, 4) {
    uint32_t r = ARG(1), n = ARG(3);
    if (r < 8) memcpy(GPTR(ARG(2)), dev->psc[r], 16 * (r + n > 8 ? 8 - r : n));
    return 0;
}

METHOD(dev_GetFrontBuffer, 2) { return res_read_front(ARG(1)); }
METHOD(dev_CopyRects, 6) { return res_copy_rects(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)); }

static const ComEntry dev_vt[] = {
    {"IDirect3DDevice8::QueryInterface", dev_QueryInterface}, {"IDirect3DDevice8::AddRef", dev_AddRef},
    {"IDirect3DDevice8::Release", dev_Release}, {"IDirect3DDevice8::TestCooperativeLevel", dev_TestCooperativeLevel},
    {"IDirect3DDevice8::GetAvailableTextureMem", dev_GetAvailableTextureMem},
    {"IDirect3DDevice8::ResourceManagerDiscardBytes", dev_ResourceManagerDiscardBytes},
    {"IDirect3DDevice8::GetDirect3D", dev_GetDirect3D}, {"IDirect3DDevice8::GetDeviceCaps", dev_GetDeviceCaps},
    {"IDirect3DDevice8::GetDisplayMode", dev_GetDisplayMode},
    {"IDirect3DDevice8::GetCreationParameters", dev_GetCreationParameters},
    {"IDirect3DDevice8::SetCursorProperties", dev_SetCursorProperties},
    {"IDirect3DDevice8::SetCursorPosition", dev_SetCursorPosition}, {"IDirect3DDevice8::ShowCursor", dev_ShowCursor},
    {"IDirect3DDevice8::CreateAdditionalSwapChain", NULL}, {"IDirect3DDevice8::Reset", dev_Reset},
    {"IDirect3DDevice8::Present", dev_Present}, {"IDirect3DDevice8::GetBackBuffer", dev_GetBackBuffer},
    {"IDirect3DDevice8::GetRasterStatus", dev_GetRasterStatus}, {"IDirect3DDevice8::SetGammaRamp", dev_SetGammaRamp},
    {"IDirect3DDevice8::GetGammaRamp", dev_GetGammaRamp}, {"IDirect3DDevice8::CreateTexture", dev_CreateTexture},
    {"IDirect3DDevice8::CreateVolumeTexture", NULL}, {"IDirect3DDevice8::CreateCubeTexture", dev_CreateCubeTexture},
    {"IDirect3DDevice8::CreateVertexBuffer", dev_CreateVertexBuffer},
    {"IDirect3DDevice8::CreateIndexBuffer", dev_CreateIndexBuffer},
    {"IDirect3DDevice8::CreateRenderTarget", dev_CreateRenderTarget},
    {"IDirect3DDevice8::CreateDepthStencilSurface", dev_CreateDepthStencilSurface},
    {"IDirect3DDevice8::CreateImageSurface", dev_CreateImageSurface}, {"IDirect3DDevice8::CopyRects", dev_CopyRects},
    {"IDirect3DDevice8::UpdateTexture", NULL}, {"IDirect3DDevice8::GetFrontBuffer", dev_GetFrontBuffer},
    {"IDirect3DDevice8::SetRenderTarget", dev_SetRenderTarget}, {"IDirect3DDevice8::GetRenderTarget", dev_GetRenderTarget},
    {"IDirect3DDevice8::GetDepthStencilSurface", dev_GetDepthStencilSurface},
    {"IDirect3DDevice8::BeginScene", dev_BeginScene}, {"IDirect3DDevice8::EndScene", dev_EndScene},
    {"IDirect3DDevice8::Clear", dev_Clear}, {"IDirect3DDevice8::SetTransform", dev_SetTransform},
    {"IDirect3DDevice8::GetTransform", dev_GetTransform}, {"IDirect3DDevice8::MultiplyTransform", dev_MultiplyTransform},
    {"IDirect3DDevice8::SetViewport", dev_SetViewport}, {"IDirect3DDevice8::GetViewport", dev_GetViewport},
    {"IDirect3DDevice8::SetMaterial", dev_SetMaterial}, {"IDirect3DDevice8::GetMaterial", dev_GetMaterial},
    {"IDirect3DDevice8::SetLight", dev_SetLight}, {"IDirect3DDevice8::GetLight", NULL},
    {"IDirect3DDevice8::LightEnable", dev_LightEnable}, {"IDirect3DDevice8::GetLightEnable", dev_GetLightEnable},
    {"IDirect3DDevice8::SetClipPlane", dev_SetClipPlane}, {"IDirect3DDevice8::GetClipPlane", NULL},
    {"IDirect3DDevice8::SetRenderState", dev_SetRenderState}, {"IDirect3DDevice8::GetRenderState", dev_GetRenderState},
    {"IDirect3DDevice8::BeginStateBlock", dev_BeginStateBlock}, {"IDirect3DDevice8::EndStateBlock", dev_EndStateBlock},
    {"IDirect3DDevice8::ApplyStateBlock", dev_ApplyStateBlock},
    {"IDirect3DDevice8::CaptureStateBlock", dev_CaptureStateBlock},
    {"IDirect3DDevice8::DeleteStateBlock", dev_DeleteStateBlock},
    {"IDirect3DDevice8::CreateStateBlock", dev_CreateStateBlock},
    {"IDirect3DDevice8::SetClipStatus", dev_SetClipStatus}, {"IDirect3DDevice8::GetClipStatus", dev_GetClipStatus},
    {"IDirect3DDevice8::GetTexture", dev_GetTexture}, {"IDirect3DDevice8::SetTexture", dev_SetTexture},
    {"IDirect3DDevice8::GetTextureStageState", dev_GetTextureStageState},
    {"IDirect3DDevice8::SetTextureStageState", dev_SetTextureStageState},
    {"IDirect3DDevice8::ValidateDevice", dev_ValidateDevice}, {"IDirect3DDevice8::GetInfo", NULL},
    {"IDirect3DDevice8::SetPaletteEntries", dev_SetPaletteEntries},
    {"IDirect3DDevice8::GetPaletteEntries", dev_GetPaletteEntries},
    {"IDirect3DDevice8::SetCurrentTexturePalette", dev_SetCurrentTexturePalette},
    {"IDirect3DDevice8::GetCurrentTexturePalette", dev_GetCurrentTexturePalette},
    {"IDirect3DDevice8::DrawPrimitive", dev_DrawPrimitive},
    {"IDirect3DDevice8::DrawIndexedPrimitive", dev_DrawIndexedPrimitive},
    {"IDirect3DDevice8::DrawPrimitiveUP", dev_DrawPrimitiveUP},
    {"IDirect3DDevice8::DrawIndexedPrimitiveUP", dev_DrawIndexedPrimitiveUP},
    {"IDirect3DDevice8::ProcessVertices", NULL}, {"IDirect3DDevice8::CreateVertexShader", dev_CreateVertexShader},
    {"IDirect3DDevice8::SetVertexShader", dev_SetVertexShader}, {"IDirect3DDevice8::GetVertexShader", dev_GetVertexShader},
    {"IDirect3DDevice8::DeleteVertexShader", dev_DeleteVertexShader},
    {"IDirect3DDevice8::SetVertexShaderConstant", dev_SetVertexShaderConstant},
    {"IDirect3DDevice8::GetVertexShaderConstant", dev_GetVertexShaderConstant},
    {"IDirect3DDevice8::GetVertexShaderDeclaration", NULL}, {"IDirect3DDevice8::GetVertexShaderFunction", NULL},
    {"IDirect3DDevice8::SetStreamSource", dev_SetStreamSource}, {"IDirect3DDevice8::GetStreamSource", dev_GetStreamSource},
    {"IDirect3DDevice8::SetIndices", dev_SetIndices}, {"IDirect3DDevice8::GetIndices", dev_GetIndices},
    {"IDirect3DDevice8::CreatePixelShader", dev_CreatePixelShader},
    {"IDirect3DDevice8::SetPixelShader", dev_SetPixelShader}, {"IDirect3DDevice8::GetPixelShader", dev_GetPixelShader},
    {"IDirect3DDevice8::DeletePixelShader", dev_DeletePixelShader},
    {"IDirect3DDevice8::SetPixelShaderConstant", dev_SetPixelShaderConstant},
    {"IDirect3DDevice8::GetPixelShaderConstant", dev_GetPixelShaderConstant},
    {"IDirect3DDevice8::GetPixelShaderFunction", NULL}, {"IDirect3DDevice8::DrawRectPatch", NULL},
    {"IDirect3DDevice8::DrawTriPatch", NULL}, {"IDirect3DDevice8::DeletePatch", NULL},
};

static uint32_t device_vtable(void) { return com_vtable(dev_vt, sizeof dev_vt / sizeof *dev_vt); }
