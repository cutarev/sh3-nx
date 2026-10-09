/* Textures, surfaces and buffers. The game locks a guest memory copy; GL work happens lazily on the
 * render thread (at bind or draw time), so a loader thread may create and fill resources. */
#include "d3d8.h"

static Res *table[1 << 16];
static uint32_t next_slot = 1;

/* NULL for NULL, and for anything that is not a live resource object (a released texture the game
 * still has bound, say), which is logged once. */
Res *res_of(uint32_t obj) {
    if (!obj) return NULL;
    uint32_t slot = MEM32(obj + 4);
    Res *r = slot < (1u << 16) ? table[slot] : NULL;
    if (r && r->obj == obj) return r;
    static uint32_t warned;
    if (warned != obj) rt_log("D3D: %08X is not a live resource (slot %08X)", obj, slot), warned = obj;
    return NULL;
}

/* ---- formats ---- */
typedef struct { uint32_t fmt; int bpp, block; GLenum ifmt, gfmt, type; GLint swz[4]; } Fmt;
#define RGBA {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA}
#define RGB1 {GL_RED, GL_GREEN, GL_BLUE, GL_ONE}
static const Fmt fmts[] = {
    {FMT_A8R8G8B8, 4, 0, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, RGBA},
    {FMT_X8R8G8B8, 4, 0, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, RGB1},
    {FMT_R5G6B5, 2, 0, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, RGB1},
    {FMT_X1R5G5B5, 2, 0, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, RGB1},
    {FMT_A1R5G5B5, 2, 0, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, RGBA},
    {FMT_A4R4G4B4, 2, 0, GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, RGBA},
    {FMT_X4R4G4B4, 2, 0, GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, RGB1},
    {FMT_A8, 1, 0, GL_R8, GL_RED, GL_UNSIGNED_BYTE, {GL_ZERO, GL_ZERO, GL_ZERO, GL_RED}},
    {FMT_L8, 1, 0, GL_R8, GL_RED, GL_UNSIGNED_BYTE, {GL_RED, GL_RED, GL_RED, GL_ONE}},
    {FMT_A8L8, 2, 0, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, {GL_RED, GL_RED, GL_RED, GL_GREEN}},
    {FMT_V8U8, 2, 0, GL_RG8_SNORM, GL_RG, GL_BYTE, {GL_RED, GL_GREEN, GL_ONE, GL_ONE}},
    {FMT_P8, 1, 0, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, RGBA},  /* expanded through the palette */
    {FMT_DXT1, 0, 8, 0x83F1, 0, 0, RGBA},
    {FMT_DXT2, 0, 16, 0x83F2, 0, 0, RGBA},
    {FMT_DXT3, 0, 16, 0x83F2, 0, 0, RGBA},
    {FMT_DXT4, 0, 16, 0x83F3, 0, 0, RGBA},
    {FMT_DXT5, 0, 16, 0x83F3, 0, 0, RGBA},
    {FMT_D16, 2, 0, GL_DEPTH24_STENCIL8, 0, 0, RGBA}, {FMT_D16_LOCKABLE, 2, 0, GL_DEPTH24_STENCIL8, 0, 0, RGBA},
    {FMT_D24S8, 4, 0, GL_DEPTH24_STENCIL8, 0, 0, RGBA}, {FMT_D24X8, 4, 0, GL_DEPTH24_STENCIL8, 0, 0, RGBA},
    {FMT_D24X4S4, 4, 0, GL_DEPTH24_STENCIL8, 0, 0, RGBA}, {FMT_D32, 4, 0, GL_DEPTH24_STENCIL8, 0, 0, RGBA},
    {FMT_D15S1, 2, 0, GL_DEPTH24_STENCIL8, 0, 0, RGBA},
};
static const Fmt *fmt_info(uint32_t f) {
    for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++)
        if (fmts[i].fmt == f) return &fmts[i];
    rt_fatal("unsupported D3D format %u (0x%08X)", f, f);
}
static int is_depth_fmt(uint32_t f) { return f >= FMT_D16_LOCKABLE && f <= FMT_D16; }

static void level_size(const Fmt *f, uint32_t w, uint32_t h, uint32_t *pitch, uint32_t *size) {
    if (f->block) {
        *pitch = (w + 3) / 4 * f->block;
        *size = *pitch * ((h + 3) / 4);
    } else {
        *pitch = w * f->bpp;
        *size = *pitch * h;
    }
}

/* ---- objects ---- */
static uint32_t vt_tex, vt_cube, vt_surf, vt_vb, vt_ib;

static Res *new_res(int kind, uint32_t vtable) {
    uint32_t slot = 0;
    for (uint32_t i = 0; i < (1u << 16) && !slot; i++) {
        uint32_t s = (next_slot + i) & 0xFFFF;
        if (s && !table[s]) slot = s;
    }
    if (!slot) rt_fatal("too many D3D resources");
    next_slot = slot + 1;
    Res *r = calloc(1, sizeof *r);
    r->kind = kind;
    r->refs = 1;
    r->obj = com_new(vtable, 16);
    MEM32(r->obj + 4) = slot;
    table[slot] = r;
    return r;
}

static void free_res(Res *r) {
    if (r->tex && !r->parent) glDeleteTextures(1, &r->tex);
    if (r->buf) glDeleteBuffers(1, &r->buf);
    if (r->mem) gfree(r->mem);
    for (int f = 0; f < 6; f++)
        for (int l = 0; l < 14; l++)
            if (r->surf[f][l]) free_res(res_of(r->surf[f][l]));
    table[MEM32(r->obj + 4)] = NULL;
    gfree(r->obj);
    free(r);
}

void res_addref(uint32_t obj) {
    Res *r = res_of(obj);
    if (!r) return;
    if (r->parent) r = r->parent;  /* a texture level shares its texture's count */
    r->refs++;
}
void res_release(uint32_t obj) {
    Res *r = res_of(obj);
    if (!r) return;
    if (r->parent) r = r->parent;
    /* ponytail: objects are freed at zero, but GL names deleted off the render thread would leak;
     * all game releases seen so far come from the render thread. */
    if (--r->refs == 0) free_res(r);
}

uint32_t res_surface(uint32_t w, uint32_t h, uint32_t fmt, int is_rt, int is_depth) {
    Res *r = new_res(RES_SURFACE, vt_surf);
    r->fmt = fmt;
    r->w = w;
    r->h = h;
    r->is_rt = is_rt;
    r->is_depth = is_depth || is_depth_fmt(fmt);
    level_size(fmt_info(fmt), w, h, &r->pitch, &r->size);
    if (!r->is_rt && !r->is_depth) r->mem = galloc(r->size);  /* image surfaces live in guest memory only */
    return r->obj;
}

uint32_t res_texture(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, int cube) {
    Res *t = new_res(cube ? RES_CUBE : RES_TEXTURE, cube ? vt_cube : vt_tex);
    uint32_t max = 1;
    while ((w >> max) || (h >> max)) max++;
    if (!levels || levels > max) levels = max;
    if (levels > 14) levels = 14;
    *t = (Res){t->kind, t->obj, 1, fmt, usage, pool, w, h, levels};
    t->is_rt = usage & 1;
    t->is_depth = (usage & 2) || is_depth_fmt(fmt);
    const Fmt *f = fmt_info(fmt);
    for (int face = 0; face < (cube ? 6 : 1); face++)
        for (uint32_t l = 0; l < levels; l++) {
            Res *s = new_res(RES_SURFACE, vt_surf);
            s->fmt = fmt;
            s->w = w >> l ? w >> l : 1;
            s->h = h >> l ? h >> l : 1;
            s->parent = t;
            s->level = l;
            s->face = face;
            s->is_rt = t->is_rt;
            s->is_depth = t->is_depth;
            level_size(f, s->w, s->h, &s->pitch, &s->size);
            if (!s->is_rt && !s->is_depth) s->mem = galloc(s->size);
            t->surf[face][l] = s->obj;
        }
    return t->obj;
}

uint32_t res_buffer(int kind, uint32_t size, uint32_t usage, uint32_t fmt_or_fvf, uint32_t pool) {
    Res *b = new_res(kind, kind == RES_VB ? vt_vb : vt_ib);
    b->size = size;
    b->usage = usage;
    b->fmt = fmt_or_fvf;
    b->pool = pool;
    b->mem = galloc(size);
    b->dirty = 1;
    return b->obj;
}

/* ---- GL side ---- */

static GLenum face_target(Res *t, int face) {
    return t->kind == RES_CUBE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
}

/* Creates the GL texture of a texture or standalone surface, with every level allocated. */
static void gl_ensure(Res *t) {
    if (t->tex) return;
    const Fmt *f = fmt_info(t->fmt);
    GLenum target = t->kind == RES_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    glGenTextures(1, &t->tex);
    glBindTexture(target, t->tex);
    uint32_t levels = t->kind == RES_SURFACE ? 1 : t->levels;
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
    glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, f->swz);
    for (int face = 0; face < (t->kind == RES_CUBE ? 6 : 1); face++)
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t w = t->w >> l ? t->w >> l : 1, h = t->h >> l ? t->h >> l : 1, pitch, size;
            if (t->is_depth)
                glTexImage2D(face_target(t, face), l, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                             GL_UNSIGNED_INT_24_8, NULL);
            else if (f->block) {
                level_size(f, w, h, &pitch, &size);
                glCompressedTexImage2D(face_target(t, face), l, f->ifmt, w, h, 0, size, NULL);
            } else glTexImage2D(face_target(t, face), l, f->ifmt, w, h, 0, f->gfmt, f->type, NULL);
        }
}

static GLuint surface_tex(Res *s) { if (s->parent) { gl_ensure(s->parent); return s->parent->tex; } gl_ensure(s); return s->tex; }

/* Uploads one level from its guest copy. */
static void upload_level(Res *t, Res *s) {
    const Fmt *f = fmt_info(s->fmt);
    GLenum target = face_target(t, s->face);
    glBindTexture(t->kind == RES_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (f->block) glCompressedTexSubImage2D(target, s->level, 0, 0, s->w, s->h, f->ifmt, s->size, GPTR(s->mem));
    else if (s->fmt == FMT_P8) {
        uint32_t *rgba = malloc(4u * s->w * s->h);
        const uint8_t *src = GPTR(s->mem);
        const uint32_t *pal = dev->palette[dev->cur_palette];  /* PALETTEENTRY is R,G,B,flags in memory */
        for (uint32_t i = 0; i < s->w * s->h; i++) rgba[i] = pal[src[i]] | 0xFF000000u;
        glTexSubImage2D(target, s->level, 0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
    } else glTexSubImage2D(target, s->level, 0, 0, s->w, s->h, f->gfmt, f->type, GPTR(s->mem));
    s->dirty = 0;
}

void res_upload_dirty(Res *t) {
    gl_ensure(t);
    if (!t->dirty) return;
    t->dirty = 0;
    for (int face = 0; face < (t->kind == RES_CUBE ? 6 : 1); face++)
        for (uint32_t l = 0; l < t->levels; l++) {
            Res *s = res_of(t->surf[face][l]);
            if (s->dirty) upload_level(t, s);
        }
}

static void mark_dirty(Res *s) {
    s->dirty = 1;
    if (s->parent) s->parent->dirty = 1;
}

/* A depth surface smaller than the render target clips a GL framebuffer to their intersection, where D3D8
 * drivers drew the whole target: SH3 renders its scene into 512x512 with the 640x480 auto depth, which left
 * the bottom 32 rows black. Such a pass gets a depth texture as big as both instead.
 * ponytail: one spare, re-made when the size changes. */
static GLuint spare_depth(uint32_t w, uint32_t h) {
    static GLuint tex;
    static uint32_t sw, sh;
    if (!tex || sw != w || sh != h) {
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        sw = w;
        sh = h;
    }
    return tex;
}

void res_bind_targets(void) {
    Res *rt = res_of(dev->rt), *ds = res_of(dev->ds);
    GLuint depth = ds ? surface_tex(ds) : 0;
    int depth_level = ds ? ds->level : 0;
    if (ds && (ds->w < rt->w || ds->h < rt->h)) {
        depth = spare_depth(ds->w > rt->w ? ds->w : rt->w, ds->h > rt->h ? ds->h : rt->h);
        depth_level = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, dev->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, rt->parent ? face_target(rt->parent, rt->face) : GL_TEXTURE_2D,
                           surface_tex(rt), rt->level);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, depth_level);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) rt_log("framebuffer incomplete: %04X (rt %ux%u fmt %u)", st, rt->w, rt->h, rt->fmt);
}

/* Reads a render target back into the surface's guest copy, for locks of rendered images. */
static void readback(Res *s) {
    const Fmt *f = fmt_info(s->fmt);
    if (!s->mem) s->mem = galloc(s->size);
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface_tex(s), s->level);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s->w, s->h, f->gfmt, f->type, GPTR(s->mem));
    glDeleteFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, dev->fbo);
}

/* The last presented frame. Direct3D's front buffer is what was last shown, not the back buffer the game has
 * already cleared for the next frame; the game uses it for the transition cross-fades and the save thumbnail. */
static GLuint front_tex, front_fbo;
static uint32_t front_w, front_h;

void res_snapshot_front(void) {  /* called by Present with the back buffer bound for reading */
    Res *bb = res_of(dev->backbuffer);
    if (!bb) return;
    if (!front_tex || front_w != bb->w || front_h != bb->h) {
        if (front_tex) { glDeleteTextures(1, &front_tex); glDeleteFramebuffers(1, &front_fbo); }
        front_w = bb->w;
        front_h = bb->h;
        glGenTextures(1, &front_tex);
        glBindTexture(GL_TEXTURE_2D, front_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, front_w, front_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &front_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, front_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, front_tex, 0);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, front_fbo);
    glBlitFramebuffer(0, 0, front_w, front_h, 0, 0, front_w, front_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

/* GetFrontBuffer: the last presented image into an A8R8G8B8 image surface. */
uint32_t res_read_front(uint32_t dst_o) {
    Res *src = res_of(dev->backbuffer), *dst = res_of(dst_o);
    if (!dst || !dst->mem || dst->fmt != FMT_A8R8G8B8) return D3DERR_INVALIDCALL;
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    /* the presented frame when there is one, else the back buffer */
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           front_tex && front_w == src->w && front_h == src->h ? front_tex : surface_tex(src), 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, dst->pitch / 4);
    glReadPixels(0, 0, src->w < dst->w ? src->w : dst->w, src->h < dst->h ? src->h : dst->h, GL_BGRA,
                 GL_UNSIGNED_BYTE, GPTR(dst->mem));
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glDeleteFramebuffers(1, &fb);
    for (uint32_t i = 0; i < dst->size; i += 4) *(uint8_t *)GPTR(dst->mem + i + 3) = 0xFF;
    mark_dirty(dst);
    res_bind_targets();
    return 0;
}

uint32_t res_copy_rects(uint32_t src_o, uint32_t rects, uint32_t n, uint32_t dst_o, uint32_t points) {
    Res *src = res_of(src_o), *dst = res_of(dst_o);
    if (src->fmt != dst->fmt && !(src->is_rt && dst->is_rt)) return D3DERR_INVALIDCALL;
    int32_t whole[4] = {0, 0, (int32_t)src->w, (int32_t)src->h};
    if (!n) n = 1;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t *r = rects ? (const int32_t *)GPTR(rects + 16 * i) : whole;  /* RECT l t r b */
        int32_t dx = points ? ((int32_t *)GPTR(points + 8 * i))[0] : r[0];
        int32_t dy = points ? ((int32_t *)GPTR(points + 8 * i))[1] : r[1];
        int w = r[2] - r[0], h = r[3] - r[1];
        if (src->is_rt && dst->is_rt) {  /* GPU to GPU */
            GLuint fb[2];
            glGenFramebuffers(2, fb);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface_tex(src), src->level);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface_tex(dst), dst->level);
            glDisable(GL_SCISSOR_TEST);
            glBlitFramebuffer(r[0], r[1], r[2], r[3], dx, dy, dx + w, dy + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glDeleteFramebuffers(2, fb);
            res_bind_targets();
            continue;
        }
        if (src->is_rt) readback(src);
        const Fmt *f = fmt_info(src->fmt);
        if (dst->mem) {  /* row copies between guest copies */
            int bpp = f->block ? f->block : f->bpp, div = f->block ? 4 : 1;
            for (int y = 0; y < h / div; y++)
                memcpy(GPTR(dst->mem + (dy / div + y) * dst->pitch + dx / div * bpp),
                       GPTR(src->mem + (r[1] / div + y) * src->pitch + r[0] / div * bpp), w / div * bpp);
            mark_dirty(dst);
        } else {         /* guest copy into a render target */
            glBindTexture(GL_TEXTURE_2D, surface_tex(dst));
            glPixelStorei(GL_UNPACK_ROW_LENGTH, src->w);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, dst->level, dx, dy, w, h, f->gfmt, f->type,
                            GPTR(src->mem + r[1] * src->pitch + r[0] * f->bpp));
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
    }
    return 0;
}

/* ---- COM methods ---- */

#define RES ((Res *)res_of(THIS))
METHOD(r_QueryInterface, 3) { return E_NOINTERFACE; }
METHOD(r_AddRef, 1) { res_addref(THIS); return RES->parent ? RES->parent->refs : RES->refs; }
METHOD(r_Release, 1) {
    Res *r = RES->parent ? RES->parent : RES;
    uint32_t left = r->refs - 1;
    res_release(THIS);
    return left;
}
METHOD(r_GetDevice, 2) { MEM32(ARG(1)) = dev->obj; return 0; }
METHOD(r_SetPrivateData, 5) { return 0; }
METHOD(r_GetPrivateData, 4) { return D3DERR_NOTAVAILABLE; }
METHOD(r_FreePrivateData, 2) { return 0; }
METHOD(r_SetPriority, 2) { return 0; }
METHOD(r_GetPriority, 1) { return 0; }
METHOD(r_PreLoad, 1) { return 0; }
METHOD(r_GetType, 1) {
    static const uint32_t type[] = {0, 3, 5, 1, 6, 7};
    return type[RES->kind];
}
METHOD(t_SetLOD, 2) { return 0; }
METHOD(t_GetLOD, 1) { return 0; }
METHOD(t_GetLevelCount, 1) { return RES->levels; }

static void surface_desc(Res *s, uint32_t d) {
    uint32_t usage = s->is_rt ? 1 : s->is_depth ? 2 : 0;
    uint32_t v[8] = {s->fmt, 1, usage, s->parent ? s->parent->pool : 0, s->size, 0, s->w, s->h};
    memcpy(GPTR(d), v, 32);
}
static Res *level_of(Res *t, int face, uint32_t l) { return l < t->levels ? res_of(t->surf[face][l]) : NULL; }

static uint32_t lock_surface(Res *s, uint32_t locked, uint32_t rect) {
    if (!s) return D3DERR_INVALIDCALL;
    if (!s->mem) readback(s);
    uint32_t off = 0;
    if (rect) {
        const int32_t *r = GPTR(rect);
        const Fmt *f = fmt_info(s->fmt);
        off = f->block ? r[1] / 4 * s->pitch + r[0] / 4 * f->block : r[1] * s->pitch + r[0] * f->bpp;
    }
    MEM32(locked) = s->pitch;
    MEM32(locked + 4) = s->mem + off;
    return 0;
}
static uint32_t unlock_surface(Res *s) {
    if (!s) return D3DERR_INVALIDCALL;
    if (s->parent || !s->is_rt) mark_dirty(s);
    else {  /* a locked render target: push the guest copy back */
        const Fmt *f = fmt_info(s->fmt);
        glBindTexture(GL_TEXTURE_2D, surface_tex(s));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->w, s->h, f->gfmt, f->type, GPTR(s->mem));
    }
    return 0;
}

METHOD(t_GetLevelDesc, 3) { Res *s = level_of(RES, 0, ARG(1)); if (!s) return D3DERR_INVALIDCALL; surface_desc(s, ARG(2)); return 0; }
METHOD(t_GetSurfaceLevel, 3) {
    Res *s = level_of(RES, 0, ARG(1));
    if (!s) return D3DERR_INVALIDCALL;
    res_addref(s->obj);
    MEM32(ARG(2)) = s->obj;
    return 0;
}
METHOD(t_LockRect, 5) { return lock_surface(level_of(RES, 0, ARG(1)), ARG(2), ARG(3)); }
METHOD(t_UnlockRect, 2) { return unlock_surface(level_of(RES, 0, ARG(1))); }
METHOD(t_AddDirtyRect, 2) { RES->dirty = 1; return 0; }

METHOD(c_GetCubeMapSurface, 4) {
    Res *s = level_of(RES, ARG(1) % 6, ARG(2));
    if (!s) return D3DERR_INVALIDCALL;
    res_addref(s->obj);
    MEM32(ARG(3)) = s->obj;
    return 0;
}
METHOD(c_LockRect, 6) { return lock_surface(level_of(RES, ARG(1) % 6, ARG(2)), ARG(3), ARG(4)); }
METHOD(c_UnlockRect, 3) { return unlock_surface(level_of(RES, ARG(1) % 6, ARG(2))); }
METHOD(c_AddDirtyRect, 3) { RES->dirty = 1; return 0; }

METHOD(s_GetContainer, 3) {
    Res *p = RES->parent;
    if (!p) return E_NOINTERFACE;
    res_addref(p->obj);
    MEM32(ARG(2)) = p->obj;
    return 0;
}
METHOD(s_GetDesc, 2) { surface_desc(RES, ARG(1)); return 0; }
METHOD(s_LockRect, 4) { return lock_surface(RES, ARG(1), ARG(2)); }
METHOD(s_UnlockRect, 1) { return unlock_surface(RES); }

METHOD(b_Lock, 5) {
    Res *b = RES;
    MEM32(ARG(3)) = b->mem + (ARG(1) < b->size ? ARG(1) : 0);
    return 0;
}
METHOD(b_Unlock, 1) { RES->dirty = 1; return 0; }
METHOD(vb_GetDesc, 2) {
    Res *b = RES;
    uint32_t v[6] = {FMT_VERTEXDATA, 6, b->usage, b->pool, b->size, b->fmt};
    memcpy(GPTR(ARG(1)), v, 24);
    return 0;
}
METHOD(ib_GetDesc, 2) {
    Res *b = RES;
    uint32_t v[5] = {b->fmt, 7, b->usage, b->pool, b->size};
    memcpy(GPTR(ARG(1)), v, 20);
    return 0;
}

#define RESOURCE(i) \
    {i "::QueryInterface", r_QueryInterface}, {i "::AddRef", r_AddRef}, {i "::Release", r_Release}, \
    {i "::GetDevice", r_GetDevice}, {i "::SetPrivateData", r_SetPrivateData}, {i "::GetPrivateData", r_GetPrivateData}, \
    {i "::FreePrivateData", r_FreePrivateData}, {i "::SetPriority", r_SetPriority}, {i "::GetPriority", r_GetPriority}, \
    {i "::PreLoad", r_PreLoad}, {i "::GetType", r_GetType}
static const ComEntry tex_vt[] = {
    RESOURCE("IDirect3DTexture8"), {"IDirect3DTexture8::SetLOD", t_SetLOD}, {"IDirect3DTexture8::GetLOD", t_GetLOD},
    {"IDirect3DTexture8::GetLevelCount", t_GetLevelCount}, {"IDirect3DTexture8::GetLevelDesc", t_GetLevelDesc},
    {"IDirect3DTexture8::GetSurfaceLevel", t_GetSurfaceLevel}, {"IDirect3DTexture8::LockRect", t_LockRect},
    {"IDirect3DTexture8::UnlockRect", t_UnlockRect}, {"IDirect3DTexture8::AddDirtyRect", t_AddDirtyRect},
};
static const ComEntry cube_vt[] = {
    RESOURCE("IDirect3DCubeTexture8"), {"IDirect3DCubeTexture8::SetLOD", t_SetLOD},
    {"IDirect3DCubeTexture8::GetLOD", t_GetLOD}, {"IDirect3DCubeTexture8::GetLevelCount", t_GetLevelCount},
    {"IDirect3DCubeTexture8::GetLevelDesc", t_GetLevelDesc},
    {"IDirect3DCubeTexture8::GetCubeMapSurface", c_GetCubeMapSurface},
    {"IDirect3DCubeTexture8::LockRect", c_LockRect}, {"IDirect3DCubeTexture8::UnlockRect", c_UnlockRect},
    {"IDirect3DCubeTexture8::AddDirtyRect", c_AddDirtyRect},
};
static const ComEntry surf_vt[] = {
    {"IDirect3DSurface8::QueryInterface", r_QueryInterface}, {"IDirect3DSurface8::AddRef", r_AddRef},
    {"IDirect3DSurface8::Release", r_Release}, {"IDirect3DSurface8::GetDevice", r_GetDevice},
    {"IDirect3DSurface8::SetPrivateData", r_SetPrivateData}, {"IDirect3DSurface8::GetPrivateData", r_GetPrivateData},
    {"IDirect3DSurface8::FreePrivateData", r_FreePrivateData}, {"IDirect3DSurface8::GetContainer", s_GetContainer},
    {"IDirect3DSurface8::GetDesc", s_GetDesc}, {"IDirect3DSurface8::LockRect", s_LockRect},
    {"IDirect3DSurface8::UnlockRect", s_UnlockRect},
};
static const ComEntry vb_vt[] = {
    RESOURCE("IDirect3DVertexBuffer8"), {"IDirect3DVertexBuffer8::Lock", b_Lock},
    {"IDirect3DVertexBuffer8::Unlock", b_Unlock}, {"IDirect3DVertexBuffer8::GetDesc", vb_GetDesc},
};
static const ComEntry ib_vt[] = {
    RESOURCE("IDirect3DIndexBuffer8"), {"IDirect3DIndexBuffer8::Lock", b_Lock},
    {"IDirect3DIndexBuffer8::Unlock", b_Unlock}, {"IDirect3DIndexBuffer8::GetDesc", ib_GetDesc},
};

void res_init_vtables(void) {
    vt_tex = com_vtable(tex_vt, sizeof tex_vt / sizeof *tex_vt);
    vt_cube = com_vtable(cube_vt, sizeof cube_vt / sizeof *cube_vt);
    vt_surf = com_vtable(surf_vt, sizeof surf_vt / sizeof *surf_vt);
    vt_vb = com_vtable(vb_vt, sizeof vb_vt / sizeof *vb_vt);
    vt_ib = com_vtable(ib_vt, sizeof ib_vt / sizeof *ib_vt);
}
