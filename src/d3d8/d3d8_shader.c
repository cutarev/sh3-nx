/* GLSL for every draw: the fixed-function pipeline generated from device state, or vs_1_1 / ps_1_x
 * bytecode translated once. Programs are cached by the state that shaped them. */
#include "d3d8.h"
#include <stdarg.h>
#include <math.h>
#include <SDL2/SDL.h>

/* ---- string builder ---- */
typedef struct { char *s; size_t n, cap; } Str;
static void sp(Str *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sp(Str *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (b->n + k < b->cap) { b->n += k; return; }
        b->cap = (b->cap + k) * 2;
        b->s = realloc(b->s, b->cap);
    }
}

/* ---- shader objects created by the game ---- */
typedef struct { int used; char *glsl; VsInput in[16]; float consts[96][4]; uint8_t const_set[96]; int has_func; uint32_t hash; } VShader;
typedef struct { int used; char *glsl; uint32_t hash; } PShader;
static uint32_t glsl_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}
static void progcache_shader_created(void);
static VShader vshaders[1024];
static PShader pshaders[256];
#define VS_BASE 0xF0000001u  /* handles above every FVF code */

static VShader *vs_of(uint32_t h) { return h >= VS_BASE && h - VS_BASE < 1024 && vshaders[h - VS_BASE].used ? &vshaders[h - VS_BASE] : NULL; }
static PShader *ps_of(uint32_t h) { return h && h < 256 && pshaders[h].used ? &pshaders[h] : NULL; }

const VsInput *shader_vs_inputs(uint32_t h) {
    VShader *v = vs_of(h);
    if (!v) rt_fatal("draw with unknown vertex shader %08X", h);
    return v->in;
}

/* ---- bytecode common ---- */
static const int8_t n_params[90] = {
    [0] = 0, [1] = 2, [2] = 3, [3] = 3, [4] = 4, [5] = 3, [6] = 2, [7] = 2, [8] = 3, [9] = 3, [10] = 3, [11] = 3,
    [12] = 3, [13] = 3, [14] = 2, [15] = 2, [16] = 2, [17] = 3, [18] = 4, [19] = 2, [20] = 3, [21] = 3, [22] = 3,
    [23] = 3, [24] = 3, [64] = 1, [65] = 1, [66] = 1, [67] = 2, [68] = 2, [69] = 2, [70] = 2, [71] = 2, [72] = 2,
    [73] = 2, [74] = 2, [75] = 2, [76] = 3, [77] = 2, [78] = 2, [79] = 2, [80] = 4, [81] = 5, [82] = 2, [83] = 2,
    [84] = 2, [85] = 2, [86] = 2, [87] = 1, [88] = 4, [89] = 3,
};

static const char *mask_str(uint32_t tok) {
    static char m[5];
    uint32_t w = (tok >> 16) & 0xF;
    int i = 0;
    if (w == 0xF || !w) return "";
    m[i++] = '.';
    for (int c = 0; c < 4; c++) if (w & (1 << c)) m[i++] = "xyzw"[c];
    m[i] = 0;
    return m;
}
static int mask_count(uint32_t tok) { uint32_t w = (tok >> 16) & 0xF; return w ? __builtin_popcount(w) : 4; }

/* Register name for a parameter token. vs: the vertex shader namespace; ps: the pixel one. */
static void reg_name(char *out, uint32_t tok, int vs, int rel) {
    uint32_t type = (tok >> 28) & 7, n = tok & 0x7FF;
    switch (type) {
    case 0: sprintf(out, "r%u", n); break;
    case 1: sprintf(out, vs ? "v%u" : "vC%u", n); break;
    case 2:
        if (vs && rel) sprintf(out, "c[a0 + %u]", n);
        else sprintf(out, vs ? "c[%u]" : "c%u", n);
        break;
    case 3: sprintf(out, vs ? "a0f" : "t%u", n); break;
    case 4: sprintf(out, n == 0 ? "oPos" : n == 1 ? "oFog" : "oPts"); break;
    case 5: sprintf(out, "oD%u", n); break;
    case 6: sprintf(out, "oT%u", n); break;
    default: sprintf(out, "r0"); break;
    }
}

/* A source operand as a vec4 GLSL expression, with swizzle and modifier applied. */
static void src_expr(char *out, uint32_t tok, int vs) {
    char r[32], sw[8] = "";
    reg_name(r, tok, vs, (tok >> 13) & 1);
    uint32_t s = (tok >> 16) & 0xFF;
    if (s != 0xE4) {
        sw[0] = '.';
        for (int i = 0; i < 4; i++) sw[1 + i] = "xyzw"[(s >> (2 * i)) & 3];
        sw[5] = 0;
    }
    char v[64];
    snprintf(v, sizeof v, "%s%s", r, sw);
    switch ((tok >> 24) & 0xF) {
    case 1: sprintf(out, "(-%s)", v); break;
    case 2: sprintf(out, "(%s - 0.5)", v); break;
    case 3: sprintf(out, "(0.5 - %s)", v); break;
    case 4: sprintf(out, "(2.0 * %s - 1.0)", v); break;
    case 5: sprintf(out, "(1.0 - 2.0 * %s)", v); break;
    case 6: sprintf(out, "(1.0 - %s)", v); break;
    case 7: sprintf(out, "(2.0 * %s)", v); break;
    case 8: sprintf(out, "(-2.0 * %s)", v); break;
    default: sprintf(out, "%s", v); break;
    }
}

/* Writes dst = expr (a vec4 expression) honoring the write mask, saturate and the ps shift. */
static void emit_dst(Str *b, uint32_t tok, int vs, const char *expr) {
    char d[32], e[512];
    reg_name(d, tok, vs, 0);
    int shift = (tok >> 24) & 0xF;
    static const char *scale[16] = {"", " * 2.0", " * 4.0", " * 8.0", "", "", "", "", "", "", "", "", "", " * 0.125", " * 0.25", " * 0.5"};
    snprintf(e, sizeof e, "(%s)%s", expr, vs ? "" : scale[shift]);
    if ((tok >> 20) & 1) { char t[600]; snprintf(t, sizeof t, "clamp(%s, 0.0, 1.0)", e); strcpy(e, t); }
    const char *m = mask_str(tok);
    if (!strcmp(d, "oFog") || !strcmp(d, "oPts")) sp(b, "  %s = vec4(%s).x;\n", d, e);
    else if (!strcmp(d, "a0f")) sp(b, "  a0 = int(floor((%s).x));\n", e);
    else sp(b, "  %s%s = vec4(%s)%s;\n", d, m, e, m);
}

/* Instructions shared by both shader types. Returns 0 when the opcode is not one of them. */
static int emit_alu(Str *b, uint32_t op, const uint32_t *p, int vs) {
    char s0[96], s1[96], s2[96], e[512];
    int np = n_params[op];
    if (np > 1) src_expr(s0, p[1], vs);
    if (np > 2) src_expr(s1, p[2], vs);
    if (np > 3) src_expr(s2, p[3], vs);
    switch (op) {
    case 1: snprintf(e, sizeof e, "%s", s0); break;
    case 2: snprintf(e, sizeof e, "%s + %s", s0, s1); break;
    case 3: snprintf(e, sizeof e, "%s - %s", s0, s1); break;
    case 4: snprintf(e, sizeof e, "%s * %s + %s", s0, s1, s2); break;
    case 5: snprintf(e, sizeof e, "%s * %s", s0, s1); break;
    case 6: snprintf(e, sizeof e, "vec4(rcp_(%s.w))", s0); break;
    case 7: snprintf(e, sizeof e, "vec4(rsq_(%s.w))", s0); break;
    case 8: snprintf(e, sizeof e, "vec4(dot(%s.xyz, %s.xyz))", s0, s1); break;
    case 9: snprintf(e, sizeof e, "vec4(dot(%s, %s))", s0, s1); break;
    case 10: snprintf(e, sizeof e, "min(%s, %s)", s0, s1); break;
    case 11: snprintf(e, sizeof e, "max(%s, %s)", s0, s1); break;
    case 12: snprintf(e, sizeof e, "vec4(lessThan(%s, %s))", s0, s1); break;
    case 13: snprintf(e, sizeof e, "vec4(greaterThanEqual(%s, %s))", s0, s1); break;
    case 14: snprintf(e, sizeof e, "vec4(exp2(%s.w))", s0); break;
    case 15: snprintf(e, sizeof e, "vec4(log_(%s.w))", s0); break;
    case 16: snprintf(e, sizeof e, "lit_(%s)", s0); break;
    case 17: snprintf(e, sizeof e, "vec4(1.0, %s.y * %s.y, %s.z, %s.w)", s0, s1, s0, s1); break;
    case 18: snprintf(e, sizeof e, "mix(%s, %s, %s)", s2, s1, s0); break;
    case 19: snprintf(e, sizeof e, "fract(%s)", s0); break;
    case 78: snprintf(e, sizeof e, "expp_(%s.w)", s0); break;
    case 79: snprintf(e, sizeof e, "vec4(log_(%s.w))", s0); break;
    case 20: case 21: case 22: case 23: case 24: {  /* m4x4, m4x3, m3x4, m3x3, m3x2: dp against consecutive rows */
        static const int rows[] = {4, 3, 4, 3, 2}, dims[] = {4, 4, 3, 3, 3};
        int nr = rows[op - 20], dim = dims[op - 20];
        char base[96];
        reg_name(base, p[2], vs, (p[2] >> 13) & 1);
        int n = 0;
        n += snprintf(e + n, sizeof e - n, "vec4(");
        for (int i = 0; i < 4; i++) {
            if (i < nr) {
                char row[64];
                if (strchr(base, '[')) { /* c[k] or c[a0 + k] */
                    char pre[64]; int k; const char *br = strchr(base, '[');
                    snprintf(pre, br - base + 1, "%s", base);
                    if (strstr(base, "a0")) { sscanf(strstr(base, "+") + 1, "%d", &k); snprintf(row, sizeof row, "%s[a0 + %d]", pre, k + i); }
                    else { sscanf(br + 1, "%d", &k); snprintf(row, sizeof row, "%s[%d]", pre, k + i); }
                } else snprintf(row, sizeof row, "r%d", (int)(p[2] & 0x7FF) + i);
                n += snprintf(e + n, sizeof e - n, dim == 4 ? "dot(%s, %s)" : "dot(%s.xyz, %s.xyz)", s0, row);
            } else n += snprintf(e + n, sizeof e - n, "0.0");
            n += snprintf(e + n, sizeof e - n, i < 3 ? ", " : ")");
        }
        break;
    }
    case 80: snprintf(e, sizeof e, "mix(%s, %s, step(%s.w, 0.5))", s1, s2, s0); break;   /* cnd: r0.a > 0.5 ? s1 : s2 */
    case 88: snprintf(e, sizeof e, "mix(%s, %s, lessThan(%s, vec4(0.0)))", s1, s2, s0); break;  /* cmp: s0 >= 0 ? s1 : s2 */
    default: return 0;
    }
    emit_dst(b, p[0], vs, e);
    return 1;
}

static const char *glsl_helpers =
    "float rcp_(float x) { return x == 0.0 ? 3.0e38 : 1.0 / x; }\n"
    "float rsq_(float x) { x = abs(x); return x == 0.0 ? 3.0e38 : inversesqrt(x); }\n"
    "float log_(float x) { x = abs(x); return x == 0.0 ? -3.0e38 : log2(x); }\n"
    "vec4 lit_(vec4 s) { float d = max(s.x, 0.0); float p = clamp(s.w, -127.9961, 127.9961);\n"
    "  return vec4(1.0, d, (s.x > 0.0 && s.y > 0.0) ? pow(s.y, p) : 0.0, 1.0); }\n"
    "vec4 expp_(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n";

/* ---- vertex shaders ---- */

static void parse_decl(VShader *v, uint32_t decl) {
    for (int i = 0; i < 16; i++) v->in[i].stream = -1;
    int stream = 0, off = 0;
    static const int tsize[8] = {4, 8, 12, 16, 4, 4, 4, 8};
    for (uint32_t p = decl;; p += 4) {
        uint32_t t = MEM32(p), kind = t >> 29;
        if (t == 0xFFFFFFFFu) break;
        if (kind == 1) { stream = t & 0xF; off = 0; }
        else if (kind == 2) {
            if (t & (1 << 28)) off += 4 * ((t >> 16) & 0xF);   /* SKIP */
            else {
                uint32_t type = (t >> 16) & 0xF, reg = t & 0x1F;
                if (reg < 16) v->in[reg] = (VsInput){stream, off, (int)type};
                off += tsize[type & 7];
            }
        } else if (kind == 4) {
            uint32_t n = (t >> 25) & 0xF, addr = t & 0x7F;
            for (uint32_t i = 0; i < n * 4 && addr + i / 4 < 96; i++) {
                v->consts[addr + i / 4][i % 4] = MEMF(p + 4 + 4 * i);
                v->const_set[addr + i / 4] = 1;
            }
            p += 16 * n;
        }
    }
}

static char *translate_vs(uint32_t func) {
    Str b = {0};
    sp(&b, "uniform vec4 c[96];\nint a0 = 0;\n");
    for (int i = 0; i < 16; i++) sp(&b, "layout(location = %d) in vec4 v%d;\n", i, i);
    sp(&b, "%s", glsl_helpers);
    sp(&b, "void vs_main(out vec4 oPos, out vec4 oD0, out vec4 oD1, out vec4 oT0, out vec4 oT1, out vec4 oT2,"
           " out vec4 oT3, out vec4 oT4, out vec4 oT5, out vec4 oT6, out vec4 oT7, out float oFog, out float oPts) {\n");
    sp(&b, "  vec4 r0 = vec4(0.0), r1 = r0, r2 = r0, r3 = r0, r4 = r0, r5 = r0, r6 = r0, r7 = r0, r8 = r0, r9 = r0, r10 = r0, r11 = r0;\n");
    sp(&b, "  oPos = vec4(0.0); oD0 = vec4(1.0); oD1 = vec4(0.0); oFog = 1.0; oPts = 1.0;\n");
    sp(&b, "  oT0 = oT1 = oT2 = oT3 = oT4 = oT5 = oT6 = oT7 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    for (uint32_t p = func + 4;; ) {
        uint32_t t = MEM32(p), op = t & 0xFFFF;
        if (t == 0x0000FFFFu) break;
        if (op == 0xFFFE) { p += 4 + 4 * ((t >> 16) & 0x7FFF); continue; }  /* comment */
        if (op >= 90 || (op && !n_params[op])) rt_fatal("vs: unknown opcode %u", op);
        uint32_t prm[8];
        for (int i = 0; i < n_params[op]; i++) prm[i] = MEM32(p + 4 + 4 * i);
        if (op && !emit_alu(&b, op, prm, 1)) rt_fatal("vs: unsupported opcode %u", op);
        p += 4 + 4 * n_params[op];
    }
    sp(&b, "}\n");
    return b.s;
}

uint32_t shader_create_vs(uint32_t decl, uint32_t func) {
    for (int i = 0; i < 1024; i++)
        if (!vshaders[i].used) {
            VShader *v = &vshaders[i];
            memset(v, 0, sizeof *v);
            v->used = 1;
            parse_decl(v, decl);
            v->has_func = func != 0;
            if (func) v->glsl = translate_vs(func);
            v->hash = func ? glsl_hash(v->glsl) : 0;
            progcache_shader_created();
            return VS_BASE + i;
        }
    rt_fatal("too many vertex shaders");
}

void shader_delete_vs(uint32_t h) {
    VShader *v = vs_of(h);
    if (v) { free(v->glsl); v->used = 0; }
}

/* ---- pixel shaders ---- */

static char *translate_ps(uint32_t func) {
    Str b = {0};
    uint32_t version = MEM32(func) & 0xFFFF;
    sp(&b, "%s", glsl_helpers);
    sp(&b, "vec4 ps_main(vec4 vC0, vec4 vC1) {\n");
    sp(&b, "  vec4 r0 = vec4(0.0), r1 = r0, t0 = r0, t1 = r0, t2 = r0, t3 = r0;\n");
    sp(&b, "  vec4 c0 = pc[0], c1 = pc[1], c2 = pc[2], c3 = pc[3], c4 = pc[4], c5 = pc[5], c6 = pc[6], c7 = pc[7];\n");
    for (uint32_t p = func + 4;; ) {
        uint32_t t = MEM32(p), op = t & 0xFFFF;
        if (t == 0x0000FFFFu) break;
        if (op == 0xFFFE) { p += 4 + 4 * ((t >> 16) & 0x7FFF); continue; }
        if (op == 0xFFFD) { p += 4; continue; }  /* phase */
        if (op >= 90 || (op && !n_params[op])) rt_fatal("ps: unknown opcode %u", op);
        uint32_t prm[8];
        for (int i = 0; i < n_params[op]; i++) prm[i] = MEM32(p + 4 + 4 * i);
        uint32_t n = prm[0] & 0x7FF;
        switch (op) {
        case 0: break;
        case 81:  /* def c#, x, y, z, w */
            sp(&b, "  c%u = vec4(%.9g, %.9g, %.9g, %.9g);\n", n,
               *(float *)&prm[1], *(float *)&prm[2], *(float *)&prm[3], *(float *)&prm[4]);
            break;
        case 66: sp(&b, "  t%u = tex%u(vT%u);\n", n, n, n); break;                       /* tex */
        case 64: sp(&b, "  t%u = vec4(clamp(vT%u.xyz, 0.0, 1.0), 1.0);\n", n, n); break;   /* texcoord */
        case 65: sp(&b, "  if (any(lessThan(vT%u.xyz, vec3(0.0)))) discard;\n", n); break; /* texkill */
        /* texreg2ar / texreg2gb / texreg2rgb: stage n's texture at coordinates read from another t register */
        case 69: sp(&b, "  t%u = tex%u(vec4(t%u.wx, 0.0, 1.0));\n", n, n, prm[1] & 0x7FF); break;
        case 70: sp(&b, "  t%u = tex%u(vec4(t%u.yz, 0.0, 1.0));\n", n, n, prm[1] & 0x7FF); break;
        case 82: sp(&b, "  t%u = tex%u(vec4(t%u.xyz, 1.0));\n", n, n, prm[1] & 0x7FF); break;
        default:
            if (!emit_alu(&b, op, prm, 0)) rt_fatal("ps %X: unsupported opcode %u", version, op);
        }
        p += 4 + 4 * n_params[op];
    }
    sp(&b, "  return r0;\n}\n");
    return b.s;
}

uint32_t shader_create_ps(uint32_t func) {
    for (int i = 1; i < 256; i++)
        if (!pshaders[i].used) {
            pshaders[i] = (PShader){1, translate_ps(func), 0};
            pshaders[i].hash = glsl_hash(pshaders[i].glsl);
            progcache_shader_created();
            return i;
        }
    rt_fatal("too many pixel shaders");
}

void shader_delete_ps(uint32_t h) {
    PShader *p = ps_of(h);
    if (p) { free(p->glsl); p->used = 0; }
}

/* ---- program keys ---- */

typedef struct {
    uint32_t vs, ps;          /* shader handles (vs: 0 or FFP with declaration -> ffp) */
    uint8_t ffp_vs, rhw, normal, diffuse, specular, lighting, normalize, localviewer, colorvertex;
    uint8_t diffsrc, ambsrc, specsrc, emissrc, vblend;
    uint8_t light[8];
    uint8_t tci[8], tcgen[8], ttff[8], tcsize[8];
    uint8_t colorop[8], ca[8][3], alphaop[8], aa[8][3], result[8], textype[8];
    uint8_t alphatest, alphafunc, fogmode, fogtable, specenable, fogenable;
} Key;

typedef struct Prog {
    Key key;
    GLuint id;
    GLint c, pc, wvp, wv, normalm, proj, texm, mat, amb, lt, ltcol, ltpos, ltdir, ltatt, ltspot, world;
    GLint vp, halfpix, tfactor, fogcolor, fogparams, alpharef, viewproj, bumpenv;
    struct Prog *next;
} Prog;
static Prog *progs[4096];

static uint32_t key_hash(const Key *k) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ ((const uint8_t *)k)[i]) * 16777619u;
    return h;
}

/* ---- fixed-function vertex stage ---- */

static void ffp_vs(Str *b, const Key *k) {
    sp(b, "layout(location = 0) in vec4 aPos;\nlayout(location = 1) in vec4 aBlend;\nlayout(location = 3) in vec3 aNormal;\n"
          "layout(location = 5) in vec4 aDiffuse;\nlayout(location = 6) in vec4 aSpecular;\n");
    for (int t = 0; t < 8; t++) sp(b, "layout(location = %d) in vec4 aTex%d;\n", 7 + t, t);
    sp(b, "uniform mat4 uWVP, uWV, uProj, uTexM[8], uWorld[4], uViewProj;\nuniform mat3 uNormalM;\n"
          "uniform vec4 uMat[4]; uniform float uPower; uniform vec4 uAmbient;\n"
          "uniform vec4 uLtDiffuse[8], uLtSpecular[8], uLtAmbient[8], uLtPos[8], uLtDir[8], uLtAtt[8], uLtSpot[8];\n"
          "uniform vec4 uVp; uniform vec4 uFogParams;\n");
    sp(b, "void vs_main(out vec4 oPos, out vec4 oD0, out vec4 oD1, out vec4 oT0, out vec4 oT1, out vec4 oT2,"
          " out vec4 oT3, out vec4 oT4, out vec4 oT5, out vec4 oT6, out vec4 oT7, out float oFog, out float oPts) {\n");
    sp(b, "  vec4 oT[8]; oPts = 1.0;\n");
    sp(b, "  vec4 dif = %s, spe = %s;\n", k->diffuse ? "aDiffuse" : "vec4(1.0)", k->specular ? "aSpecular" : "vec4(0.0)");
    if (k->rhw) {
        /* Screen-space vertices: undo the viewport so GL's viewport puts them back. */
        sp(b, "  float w = aPos.w == 0.0 ? 1.0 : 1.0 / aPos.w;\n"
              "  vec3 ndc = vec3((aPos.x - uVp.x) / uVp.z * 2.0 - 1.0, 1.0 - (aPos.y - uVp.y) / uVp.w * 2.0, aPos.z);\n"
              "  oPos = vec4(ndc * w, w);\n  vec3 P = vec3(0.0), N = vec3(0.0, 0.0, -1.0);\n"
              "  oD0 = dif; oD1 = spe; oFog = spe.a;\n");
    } else {
        sp(b, "  vec4 pos = vec4(aPos.xyz, 1.0);\n");
        if (k->vblend) {  /* world-space blend of up to 4 matrices; the last weight is 1 - sum */
            sp(b, "  vec4 wsum = vec4(0.0); float wl = 1.0;\n");
            for (int i = 0; i < k->vblend; i++) sp(b, "  wsum += aBlend[%d] * (uWorld[%d] * pos); wl -= aBlend[%d];\n", i, i, i);
            sp(b, "  wsum += wl * (uWorld[%d] * pos);\n  vec4 vpos = uViewProj * wsum;\n  oPos = vpos;\n", k->vblend);
            sp(b, "  vec3 P = (uWV * pos).xyz;\n");
        } else sp(b, "  oPos = uWVP * pos;\n  vec3 P = (uWV * pos).xyz;\n");
        sp(b, "  vec3 N = %s;\n", k->normal ? (k->normalize ? "normalize(uNormalM * aNormal)" : "uNormalM * aNormal") : "vec3(0.0)");
        if (k->lighting) {
            const char *srcs[3] = {"uMat[%d]", "dif", "spe"};
            char md[32], ma[32], ms[32], me[32];
            snprintf(md, sizeof md, srcs[k->colorvertex && k->diffsrc < 3 ? k->diffsrc : 0], 0);
            snprintf(ma, sizeof ma, srcs[k->colorvertex && k->ambsrc < 3 ? k->ambsrc : 0], 1);
            snprintf(ms, sizeof ms, srcs[k->colorvertex && k->specsrc < 3 ? k->specsrc : 0], 2);
            snprintf(me, sizeof me, srcs[k->colorvertex && k->emissrc < 3 ? k->emissrc : 0], 3);
            sp(b, "  vec3 V = %s; vec3 accA = vec3(0.0), accD = vec3(0.0), accS = vec3(0.0);\n",
               k->localviewer ? "normalize(-P)" : "vec3(0.0, 0.0, -1.0)");
            for (int i = 0; i < 8; i++) {
                if (!k->light[i]) continue;
                sp(b, "  {\n");
                if (k->light[i] == 3) sp(b, "    vec3 L = -uLtDir[%d].xyz; float att = 1.0;\n", i);
                else {
                    sp(b, "    vec3 Lv = uLtPos[%d].xyz - P; float d = length(Lv); vec3 L = Lv / max(d, 1e-6);\n", i);
                    sp(b, "    float att = d > uLtAtt[%d].w ? 0.0 : 1.0 / max(uLtAtt[%d].x + uLtAtt[%d].y * d + uLtAtt[%d].z * d * d, 1e-6);\n", i, i, i, i);
                    if (k->light[i] == 2)  /* spot: uLtSpot = cos(theta/2), cos(phi/2), falloff */
                        sp(b, "    float rho = dot(-L, uLtDir[%d].xyz);\n"
                              "    att *= rho > uLtSpot[%d].x ? 1.0 : rho <= uLtSpot[%d].y ? 0.0 :"
                              " pow(clamp((rho - uLtSpot[%d].y) / max(uLtSpot[%d].x - uLtSpot[%d].y, 1e-6), 0.0, 1.0), uLtSpot[%d].z);\n",
                           i, i, i, i, i, i, i);
                }
                sp(b, "    float nl = max(dot(N, L), 0.0);\n    accA += uLtAmbient[%d].rgb * att;\n"
                      "    accD += uLtDiffuse[%d].rgb * nl * att;\n", i, i);
                if (k->specenable)
                    sp(b, "    if (nl > 0.0) accS += uLtSpecular[%d].rgb * pow(max(dot(N, normalize(L + V)), 0.0), uPower) * att;\n", i);
                sp(b, "  }\n");
            }
            sp(b, "  oD0 = vec4(%s.rgb + %s.rgb * (uAmbient.rgb + accA) + %s.rgb * accD, %s.a);\n", me, ma, md, md);
            sp(b, "  oD1 = vec4(%s.rgb * accS, %s.a);\n", ms, ms);
            sp(b, "  oD0 = clamp(oD0, 0.0, 1.0); oD1 = clamp(oD1, 0.0, 1.0);\n");
        } else sp(b, "  oD0 = dif; oD1 = spe;\n");
        /* Vertex fog from eye depth (FOGVERTEXMODE), otherwise the specular alpha. */
        if (k->fogmode == 0) sp(b, "  oFog = spe.a;\n");
        else {
            sp(b, "  float fz = abs(P.z);\n");
            if (k->fogmode == 3) sp(b, "  oFog = clamp((uFogParams.y - fz) / max(uFogParams.y - uFogParams.x, 1e-6), 0.0, 1.0);\n");
            else if (k->fogmode == 1) sp(b, "  oFog = exp(-uFogParams.z * fz);\n");
            else sp(b, "  oFog = exp(-(uFogParams.z * fz) * (uFogParams.z * fz));\n");
        }
    }
    /* Texture coordinates per stage: a vertex set or a generated one, then the texture matrix. */
    for (int s = 0; s < 8; s++) {
        if (!k->colorop[s] && s) { sp(b, "  oT[%d] = vec4(0.0, 0.0, 0.0, 1.0);\n", s); continue; }
        int idx = k->tci[s] & 7;
        if (k->tcgen[s] == 1) sp(b, "  oT[%d] = vec4(N, 1.0);\n", s);
        else if (k->tcgen[s] == 2) sp(b, "  oT[%d] = vec4(P, 1.0);\n", s);
        else if (k->tcgen[s] == 3) sp(b, "  oT[%d] = vec4(reflect(normalize(P), N), 1.0);\n", s);
        else {
            static const char *pad[5] = {"", "aTex%d.x, 0.0, 1.0, 0.0", "aTex%d.xy, 1.0, 0.0", "aTex%d.xyz, 1.0", "aTex%d"};
            char e[48];
            snprintf(e, sizeof e, pad[k->tcsize[idx] ? k->tcsize[idx] : 2], idx);
            sp(b, "  oT[%d] = vec4(%s);\n", s, e);
        }
        if (k->ttff[s] & 7) {
            /* D3D multiplies a (count)-component row vector by the matrix; missing inputs are 1 past the last. */
            sp(b, "  oT[%d] = uTexM[%d] * oT[%d];\n", s, s, s);
            if (k->ttff[s] & 0x80) {
                int c = k->ttff[s] & 7;
                sp(b, "  oT[%d] = vec4(oT[%d].xy, 0.0, oT[%d].%c);\n", s, s, s, "xyzww"[c]);
            }
        }
    }
    sp(b, "  oT0 = oT[0]; oT1 = oT[1]; oT2 = oT[2]; oT3 = oT[3]; oT4 = oT[4]; oT5 = oT[5]; oT6 = oT[6]; oT7 = oT[7];\n}\n");
}

/* ---- fixed-function pixel stage ---- */

static void ffp_arg(char *out, int arg, int stage, int alpha) {
    static const char *src[6] = {"vD0", "cur", "tx", "uTFactor", "vD1", "tmp"};
    int a = arg & 0xF;
    const char *v = a < 6 ? src[a] : "cur";
    if (a == 1 && stage == 0) v = "vD0";
    char t[32];
    if (arg & 0x20) snprintf(t, sizeof t, "vec4(%s.a)", v); else snprintf(t, sizeof t, "%s", v);
    if (arg & 0x10) snprintf(out, 48, "(vec4(1.0) - %s)", t); else snprintf(out, 48, "%s", t);
    (void)alpha;
}

static void ffp_op(char *out, size_t n, int op, const char *a1, const char *a2, const char *a0) {
    switch (op) {
    case 2: snprintf(out, n, "%s", a1); break;
    case 3: snprintf(out, n, "%s", a2); break;
    case 4: snprintf(out, n, "%s * %s", a1, a2); break;
    case 5: snprintf(out, n, "%s * %s * 2.0", a1, a2); break;
    case 6: snprintf(out, n, "%s * %s * 4.0", a1, a2); break;
    case 7: snprintf(out, n, "%s + %s", a1, a2); break;
    case 8: snprintf(out, n, "%s + %s - 0.5", a1, a2); break;
    case 9: snprintf(out, n, "(%s + %s - 0.5) * 2.0", a1, a2); break;
    case 10: snprintf(out, n, "%s - %s", a1, a2); break;
    case 11: snprintf(out, n, "%s + %s - %s * %s", a1, a2, a1, a2); break;
    case 12: snprintf(out, n, "mix(%s, %s, vD0.a)", a2, a1); break;
    case 13: snprintf(out, n, "mix(%s, %s, tx.a)", a2, a1); break;
    case 14: snprintf(out, n, "mix(%s, %s, uTFactor.a)", a2, a1); break;
    case 15: snprintf(out, n, "%s + %s * (1.0 - tx.a)", a1, a2); break;
    case 16: snprintf(out, n, "mix(%s, %s, cur.a)", a2, a1); break;
    case 17: snprintf(out, n, "%s", a1); break;  /* premodulate: approximated */
    case 18: snprintf(out, n, "vec4(%s.rgb + %s.a * %s.rgb, %s.a)", a1, a1, a2, a1); break;
    case 19: snprintf(out, n, "vec4(%s.rgb * %s.rgb, %s.a + %s.a)", a1, a2, a1, a2); break;
    case 20: snprintf(out, n, "vec4(%s.rgb + (1.0 - %s.a) * %s.rgb, %s.a)", a1, a1, a2, a1); break;
    case 21: snprintf(out, n, "vec4((vec3(1.0) - %s.rgb) * %s.rgb, %s.a + %s.a)", a1, a2, a1, a2); break;
    case 24: snprintf(out, n, "vec4(clamp(4.0 * dot(%s.rgb - 0.5, %s.rgb - 0.5), 0.0, 1.0))", a1, a2); break;
    case 25: snprintf(out, n, "%s + %s * %s", a0, a1, a2); break;
    case 26: snprintf(out, n, "mix(%s, %s, %s)", a2, a1, a0); break;
    default: snprintf(out, n, "%s", a1); break;  /* bump mapping and friends: pass arg1 */
    }
}

static void ffp_ps(Str *b, const Key *k) {
    sp(b, "vec4 ps_main(vec4 vD0, vec4 vD1) {\n  vec4 cur = vD0, tmp = vec4(0.0), tx = vec4(1.0);\n");
    for (int s = 0; s < 8 && k->colorop[s]; s++) {
        if (k->textype[s]) sp(b, "  tx = tex%d(vT%d);\n", s, s);
        else sp(b, "  tx = vec4(1.0);\n");
        char a1[48], a2[48], a0[48], c[512], aa1[48], aa2[48], aa0[48], a[512];
        ffp_arg(a1, k->ca[s][0], s, 0); ffp_arg(a2, k->ca[s][1], s, 0); ffp_arg(a0, k->ca[s][2], s, 0);
        ffp_arg(aa1, k->aa[s][0], s, 1); ffp_arg(aa2, k->aa[s][1], s, 1); ffp_arg(aa0, k->aa[s][2], s, 1);
        ffp_op(c, sizeof c, k->colorop[s], a1, a2, a0);
        if (k->alphaop[s] && k->alphaop[s] != 1) ffp_op(a, sizeof a, k->alphaop[s], aa1, aa2, aa0);
        else snprintf(a, sizeof a, "%s", s ? "cur" : "vD0");
        sp(b, "  %s = clamp(vec4((%s).rgb, (%s).a), 0.0, 1.0);\n", k->result[s] == 5 ? "tmp" : "cur", c, a);
    }
    if (k->specenable) sp(b, "  cur.rgb = clamp(cur.rgb + vD1.rgb, 0.0, 1.0);\n");
    sp(b, "  return cur;\n}\n");
}

/* ---- program assembly ---- */

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        rt_fatal("GLSL compile failed:\n%s\n--- source ---\n%s", log, src);
    }
    return s;
}

static Prog *build(const Key *k) {
    Str v = {0}, f = {0};
    sp(&v, "#version 430 core\n");
    if (k->ffp_vs) ffp_vs(&v, k);
    else sp(&v, "%s", vs_of(k->vs)->glsl);
    sp(&v, "uniform vec2 uHalfPix;\nout vec4 vD0, vD1, vT0, vT1, vT2, vT3, vT4, vT5, vT6, vT7;\nout float vFog, vFogZ;\n"
           "void main() {\n  vec4 p; float fog, pts;\n"
           "  vs_main(p, vD0, vD1, vT0, vT1, vT2, vT3, vT4, vT5, vT6, vT7, fog, pts);\n"
           "  vD0 = clamp(vD0, 0.0, 1.0); vD1 = clamp(vD1, 0.0, 1.0);\n"
           "  vFog = clamp(fog, 0.0, 1.0); vFogZ = p.w;\n"
           "  p.xy += uHalfPix * p.w;\n  gl_Position = p;\n  gl_PointSize = max(pts, 1.0);\n}\n");

    sp(&f, "#version 430 core\nin vec4 vD0, vD1, vT0, vT1, vT2, vT3, vT4, vT5, vT6, vT7;\nin float vFog, vFogZ;\n"
           "uniform vec4 pc[8]; uniform vec4 uTFactor, uFogColor, uFogParams; uniform float uAlphaRef;\n"
           "out vec4 fragColor;\n");
    for (int s = 0; s < 8; s++) {
        int cube = k->textype[s] == 2, proj = k->ttff[s] & 0x80;
        sp(&f, "uniform sampler%s s%d;\n", cube ? "Cube" : "2D", s);
        if (cube) sp(&f, "vec4 tex%d(vec4 c) { return texture(s%d, c.xyz); }\n", s, s);
        else if (proj) sp(&f, "vec4 tex%d(vec4 c) { return textureProj(s%d, c.xyw); }\n", s, s);
        else sp(&f, "vec4 tex%d(vec4 c) { return texture(s%d, c.xy); }\n", s, s);
    }
    if (k->ps) { sp(&f, "%s", ps_of(k->ps)->glsl); }
    else ffp_ps(&f, k);
    sp(&f, "void main() {\n  vec4 c = clamp(ps_main(vD0, vD1), 0.0, 1.0);\n");
    if (k->fogenable) {
        if (k->fogtable == 3) sp(&f, "  float fz = vFogZ; float ff = clamp((uFogParams.y - fz) / max(uFogParams.y - uFogParams.x, 1e-6), 0.0, 1.0);\n");
        else if (k->fogtable == 1) sp(&f, "  float ff = exp(-uFogParams.z * vFogZ);\n");
        else if (k->fogtable == 2) sp(&f, "  float ff = exp(-(uFogParams.z * vFogZ) * (uFogParams.z * vFogZ));\n");
        else sp(&f, "  float ff = vFog;\n");
        sp(&f, "  c.rgb = mix(uFogColor.rgb, c.rgb, ff);\n");
    }
    if (k->alphatest && k->alphafunc != 8) {
        static const char *cmp[9] = {"", "false", "<", "==", "<=", ">", "!=", ">=", "true"};
        if (k->alphafunc == 1) sp(&f, "  discard;\n");
        else sp(&f, "  if (!(round(c.a * 255.0) %s uAlphaRef)) discard;\n", cmp[k->alphafunc]);
    }
    sp(&f, "  fragColor = c;\n}\n");

    GLuint vs = compile(GL_VERTEX_SHADER, v.s), fs = compile(GL_FRAGMENT_SHADER, f.s);
    GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glLinkProgram(id);
    GLint ok;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(id, sizeof log, NULL, log);
        rt_fatal("GLSL link failed: %s\n%s\n%s", log, v.s, f.s);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    free(v.s);
    free(f.s);

    Prog *p = calloc(1, sizeof *p);
    p->key = *k;
    p->id = id;
    glUseProgram(id);
    for (int s = 0; s < 8; s++) {
        char n[8];
        snprintf(n, sizeof n, "s%d", s);
        glUniform1i(glGetUniformLocation(id, n), s);
    }
#define U(field, name) p->field = glGetUniformLocation(id, name)
    U(c, "c"); U(pc, "pc"); U(wvp, "uWVP"); U(wv, "uWV"); U(normalm, "uNormalM"); U(proj, "uProj");
    U(texm, "uTexM"); U(mat, "uMat"); U(amb, "uAmbient"); U(ltcol, "uLtDiffuse"); U(ltpos, "uLtPos");
    U(ltdir, "uLtDir"); U(ltatt, "uLtAtt"); U(ltspot, "uLtSpot"); U(world, "uWorld"); U(viewproj, "uViewProj");
    U(vp, "uVp"); U(halfpix, "uHalfPix"); U(tfactor, "uTFactor"); U(fogcolor, "uFogColor");
    U(fogparams, "uFogParams"); U(alpharef, "uAlphaRef");
#undef U
    return p;
}

/* ---- persistent program cache ----
 * Programs are built the first time a state combination is drawn, which stalls that frame (the
 * "stutter when areas or shaders load"). Every key built is appended to sh3-progs.bin, with the
 * game's shaders named by a hash of their GLSL rather than by handle, and the next run builds them
 * up front: fixed-function ones before the first draw, shader ones as soon as the game has created
 * the shaders they need (handles differ between runs, hashes do not). */
typedef struct { Key key; uint32_t vsh, psh; } ProgRec;
#define PROGCACHE_FILE "sh3-progs.bin"
#define PROGCACHE_MAGIC 0x53483350u  /* "SH3P" */
static ProgRec *recs;
static size_t nrecs, caprecs, npending;
static uint8_t *rec_done;
static int warm, cache_bad, shaders_changed;

static uint32_t vs_handle_of(uint32_t hash) {
    for (int i = 0; i < 1024; i++) if (vshaders[i].used && vshaders[i].hash == hash) return VS_BASE + i;
    return 0;
}
static uint32_t ps_handle_of(uint32_t hash) {
    for (int i = 1; i < 256; i++) if (pshaders[i].used && pshaders[i].hash == hash) return i;
    return 0;
}

static Prog *prog_get(const Key *k, int record);

static void rec_add(const ProgRec *r) {
    if (nrecs == caprecs) {
        caprecs = caprecs ? caprecs * 2 : 256;
        recs = realloc(recs, caprecs * sizeof *recs);
        rec_done = realloc(rec_done, caprecs);
    }
    recs[nrecs] = *r;
    rec_done[nrecs++] = 0;
}

/* Builds every cached record whose shaders now exist. */
static void progcache_build_ready(void) {
    if (!npending) return;
    for (size_t i = 0; i < nrecs; i++) {
        if (rec_done[i]) continue;
        Key k = recs[i].key;
        if (recs[i].vsh && !(k.vs = vs_handle_of(recs[i].vsh))) continue;
        if (recs[i].psh && !(k.ps = ps_handle_of(recs[i].psh))) continue;
        rec_done[i] = 1;
        npending--;
        prog_get(&k, 0);
    }
}

static void progcache_warmup(void) {
    if (warm) {
        if (shaders_changed) { shaders_changed = 0; progcache_build_ready(); }
        return;
    }
    warm = 1;
    shaders_changed = 0;
    FILE *f = fopen(PROGCACHE_FILE, "rb");
    if (!f) return;
    uint32_t hdr[2];
    if (fread(hdr, 4, 2, f) != 2 || hdr[0] != PROGCACHE_MAGIC || hdr[1] != sizeof(ProgRec)) {
        fclose(f);
        cache_bad = 1;  /* another build's layout: start over */
        return;
    }
    ProgRec r;
    while (fread(&r, sizeof r, 1, f) == 1) { rec_add(&r); npending++; }
    fclose(f);
    Uint64 t0 = SDL_GetTicks64();
    progcache_build_ready();
    rt_log("program cache: %zu records, %zu built up front in %llu ms", nrecs, nrecs - npending,
           (unsigned long long)(SDL_GetTicks64() - t0));
}

/* The game may create shaders from a loading thread with no GL context, so the build waits for the
 * next draw. */

static void progcache_shader_created(void) { shaders_changed = 1; }

static void progcache_append(const Key *k) {
    ProgRec r = {*k, 0, 0};
    if (k->vs) { VShader *v = vs_of(k->vs); r.vsh = v ? v->hash : 0; r.key.vs = 0; }
    if (k->ps) { PShader *p = ps_of(k->ps); r.psh = p ? p->hash : 0; r.key.ps = 0; }
    for (size_t i = 0; i < nrecs; i++)
        if (recs[i].vsh == r.vsh && recs[i].psh == r.psh && !memcmp(&recs[i].key, &r.key, sizeof r.key)) return;
    rec_add(&r);
    rec_done[nrecs - 1] = 1;
    FILE *f = fopen(PROGCACHE_FILE, cache_bad ? "wb" : "ab");
    if (!f) return;
    if (cache_bad || ftell(f) == 0) {
        uint32_t hdr[2] = {PROGCACHE_MAGIC, sizeof(ProgRec)};
        fwrite(hdr, 4, 2, f);
        cache_bad = 0;
    }
    fwrite(&r, sizeof r, 1, f);
    fclose(f);
}

static Prog *prog_get(const Key *k, int record) {
    uint32_t h = key_hash(k);
    Prog *p = progs[h & 4095];
    while (p && memcmp(&p->key, k, sizeof *k)) p = p->next;
    if (!p) {
        p = build(k);
        p->next = progs[h & 4095];
        progs[h & 4095] = p;
        if (record) progcache_append(k);
    }
    return p;
}

/* ---- per-draw ---- */

static void mat_mul(float *r, const float *a, const float *b) {  /* r = a * b, D3D row-major */
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(r, t, 64);
}

/* Inverse-transpose of the upper 3x3 of a D3D row-major matrix, laid out for a GLSL mat3. */
static void normal_matrix(float *out, const float *m) {
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-12f) det = 1;
    float inv[9] = {A / det, -(b * i - c * h) / det, (b * f - c * e) / det,
                    B / det, (a * i - c * g) / det, -(a * f - c * d) / det,
                    C / det, -(a * h - b * g) / det, (a * e - b * d) / det};
    /* D3D transforms normals by n * (M^-1)^T, which as a GLSL column-vector product is M^-1 * n:
     * upload M^-1 column-major. */
    for (int r = 0; r < 3; r++)
        for (int cc = 0; cc < 3; cc++) out[cc * 3 + r] = inv[r * 3 + cc];
}

static void color4(float *o, uint32_t c) { o[0] = (c >> 16 & 255) / 255.f; o[1] = (c >> 8 & 255) / 255.f; o[2] = (c & 255) / 255.f; o[3] = (c >> 24) / 255.f; }
static float rsf(uint32_t i) { float f; memcpy(&f, &dev->rs[i], 4); return f; }

void shader_bind_for_draw(int rhw, uint32_t vs, const int *present) {
    Key k;
    memset(&k, 0, sizeof k);
    VShader *v = vs >= VS_BASE ? vs_of(vs) : NULL;
    k.ffp_vs = !v || !v->has_func;
    k.vs = k.ffp_vs ? 0 : vs;
    k.ps = ps_of(dev->ps) ? dev->ps : 0;
    uint32_t *rs = dev->rs;
    if (k.ffp_vs) {
        k.rhw = rhw;
        k.normal = present[3];
        k.diffuse = present[5];
        k.specular = present[6];
        k.lighting = !rhw && rs[137];
        k.normalize = rs[143];
        k.localviewer = rs[142];
        k.colorvertex = rs[141];
        k.diffsrc = rs[145]; k.specsrc = rs[146]; k.ambsrc = rs[147]; k.emissrc = rs[148];
        k.vblend = rs[151] <= 3 && present[1] ? rs[151] : 0;
        if (k.lighting)
            for (int i = 0; i < 8; i++) k.light[i] = dev->light_on[i] ? dev->light[i].type : 0;
        k.specenable = rs[29];
        k.fogmode = rs[28] && !rs[35] ? rs[140] : 0;
        if (vs < VS_BASE) {
            int ntex = (vs >> 8) & 0xF;
            static const int size[4] = {2, 3, 4, 1};
            for (int t = 0; t < 8; t++) k.tcsize[t] = t < ntex ? size[(vs >> (16 + 2 * t)) & 3] : 0;
        } else
            for (int t = 0; t < 8; t++) k.tcsize[t] = present[7 + t] ? 4 : 0;
    }
    for (int s = 0; s < 8; s++) {
        uint32_t *t = dev->tss[s];
        Res *tex = res_of(dev->tex[s]);
        k.textype[s] = tex ? (tex->kind == RES_CUBE ? 2 : 1) : 0;
        k.ttff[s] = (t[24] & 7) | (t[24] & 256 ? 0x80 : 0);
        if (k.ffp_vs) { k.tci[s] = t[11] & 0xFFFF; k.tcgen[s] = t[11] >> 16; }
        if (k.ps) continue;
        if (t[1] == 1 || (s && !k.colorop[s - 1])) break;
        k.colorop[s] = t[1];
        k.ca[s][0] = t[2]; k.ca[s][1] = t[3]; k.ca[s][2] = t[26];
        k.alphaop[s] = t[4];
        k.aa[s][0] = t[5]; k.aa[s][1] = t[6]; k.aa[s][2] = t[27];
        k.result[s] = t[28];
    }
    if (k.ps) for (int s = 0; s < 4; s++) k.colorop[s] = 1;  /* keeps their texcoords alive in the ffp vertex stage */
    if (!k.ps) k.specenable = rs[29];
    k.alphatest = rs[15];
    k.alphafunc = rs[25];
    k.fogenable = rs[28];
    k.fogtable = rs[28] ? rs[35] : 0;

    progcache_warmup();
    Prog *p = prog_get(&k, 1);
    glUseProgram(p->id);

    /* Uniforms */
    glUniform2f(p->halfpix, 1.0f / dev->vp_w, -1.0f / dev->vp_h);
    float vp[4] = {dev->vp_x, dev->vp_y, dev->vp_w, dev->vp_h};
    if (p->vp >= 0) glUniform4fv(p->vp, 1, vp);
    if (p->c >= 0) {
        if (v) for (int i = 0; i < 96; i++) if (v->const_set[i]) memcpy(dev->vsc[i], v->consts[i], 16);
        glUniform4fv(p->c, 96, &dev->vsc[0][0]);
    }
    if (p->pc >= 0) glUniform4fv(p->pc, 8, &dev->psc[0][0]);
    if (p->wvp >= 0) {
        float wv[16], wvp[16], vpm[16], nm[9];
        mat_mul(wv, dev->world[0], dev->view);
        mat_mul(wvp, wv, dev->proj);
        mat_mul(vpm, dev->view, dev->proj);
        glUniformMatrix4fv(p->wvp, 1, GL_FALSE, wvp);
        if (p->wv >= 0) glUniformMatrix4fv(p->wv, 1, GL_FALSE, wv);
        if (p->viewproj >= 0) glUniformMatrix4fv(p->viewproj, 1, GL_FALSE, vpm);
        if (p->world >= 0) glUniformMatrix4fv(p->world, 4, GL_FALSE, &dev->world[0][0]);
        normal_matrix(nm, wv);
        if (p->normalm >= 0) glUniformMatrix3fv(p->normalm, 1, GL_FALSE, nm);
    }
    if (p->texm >= 0) glUniformMatrix4fv(p->texm, 8, GL_FALSE, &dev->texm[0][0]);
    if (p->mat >= 0) {
        glUniform4fv(p->mat, 4, dev->material);
        glUniform1f(glGetUniformLocation(p->id, "uPower"), dev->material[16]);
        float amb[4];
        color4(amb, rs[139]);
        glUniform4fv(p->amb, 1, amb);
        float dif[8][4], spe[8][4], ambl[8][4], pos[8][4], dir[8][4], att[8][4], spot[8][4];
        for (int i = 0; i < 8; i++) {
            Light *l = &dev->light[i];
            memcpy(dif[i], l->diffuse, 16); memcpy(spe[i], l->specular, 16); memcpy(ambl[i], l->ambient, 16);
            const float *V = dev->view;  /* lights live in view space */
            for (int c = 0; c < 3; c++) {
                pos[i][c] = l->pos[0] * V[c] + l->pos[1] * V[4 + c] + l->pos[2] * V[8 + c] + V[12 + c];
                dir[i][c] = l->dir[0] * V[c] + l->dir[1] * V[4 + c] + l->dir[2] * V[8 + c];
            }
            float dl = sqrtf(dir[i][0] * dir[i][0] + dir[i][1] * dir[i][1] + dir[i][2] * dir[i][2]);
            if (dl > 0) for (int c = 0; c < 3; c++) dir[i][c] /= dl;
            pos[i][3] = dir[i][3] = 0;
            att[i][0] = l->att0; att[i][1] = l->att1; att[i][2] = l->att2; att[i][3] = l->range > 0 ? l->range : 1e30f;
            spot[i][0] = cosf(l->theta / 2); spot[i][1] = cosf(l->phi / 2); spot[i][2] = l->falloff; spot[i][3] = 0;
        }
        glUniform4fv(p->ltcol, 8, &dif[0][0]);
        glUniform4fv(glGetUniformLocation(p->id, "uLtSpecular"), 8, &spe[0][0]);
        glUniform4fv(glGetUniformLocation(p->id, "uLtAmbient"), 8, &ambl[0][0]);
        glUniform4fv(p->ltpos, 8, &pos[0][0]);
        glUniform4fv(p->ltdir, 8, &dir[0][0]);
        glUniform4fv(p->ltatt, 8, &att[0][0]);
        glUniform4fv(p->ltspot, 8, &spot[0][0]);
    }
    float c4[4];
    color4(c4, rs[60]);
    if (p->tfactor >= 0) glUniform4fv(p->tfactor, 1, c4);
    color4(c4, rs[34]);
    if (p->fogcolor >= 0) glUniform4fv(p->fogcolor, 1, c4);
    float fp[4] = {rsf(36), rsf(37), rsf(38), 0};
    if (p->fogparams >= 0) glUniform4fv(p->fogparams, 1, fp);
    if (p->alpharef >= 0) glUniform1f(p->alpharef, (float)(rs[24] & 255));

    /* Samplers follow the texture stage states. */
    static GLuint samplers[8];
    if (!samplers[0]) glGenSamplers(8, samplers);
    for (int s = 0; s < 8; s++) {
        uint32_t *t = dev->tss[s];
        static const GLint wrap[6] = {GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER, GL_MIRROR_CLAMP_TO_EDGE};
        glSamplerParameteri(samplers[s], GL_TEXTURE_WRAP_S, wrap[t[13] < 6 ? t[13] : 1]);
        glSamplerParameteri(samplers[s], GL_TEXTURE_WRAP_T, wrap[t[14] < 6 ? t[14] : 1]);
        glSamplerParameteri(samplers[s], GL_TEXTURE_WRAP_R, wrap[t[25] < 6 ? t[25] : 1]);
        int lin_mag = t[16] >= 2, lin_min = t[17] >= 2, mip = t[18];
        glSamplerParameteri(samplers[s], GL_TEXTURE_MAG_FILTER, lin_mag ? GL_LINEAR : GL_NEAREST);
        GLint minf = mip == 0 ? (lin_min ? GL_LINEAR : GL_NEAREST)
                   : mip == 1 ? (lin_min ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                              : (lin_min ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
        glSamplerParameteri(samplers[s], GL_TEXTURE_MIN_FILTER, minf);
        float bc[4];
        color4(bc, t[15]);
        glSamplerParameterfv(samplers[s], GL_TEXTURE_BORDER_COLOR, bc);
        glBindSampler(s, samplers[s]);
    }
}
