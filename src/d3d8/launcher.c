/* Before the game: the warning screen (assets/splash.bmp), then a launcher with Play and Options, where Options
 * edits sh3.ini. The images are built into the binary as BMPs (tools/launcher_assets.py), which SDL loads itself.
 * Drawn on the CPU into a 1280x720 canvas that is blitted to the window, so it needs no shaders and leaves the game
 * a fresh GL state. */
#include "d3d8.h"
#include <SDL2/SDL.h>
#include <math.h>

#define FADE_MS 700  /* every screen fades in and out */
#define CW 1280
#define CH 720

#define ASSET(name, path) \
    __asm__(".section .rodata\n.balign 16\n.global " #name "_bmp\n" #name "_bmp:\n.incbin \"" SRC_DIR "/" path "\"\n" \
            ".global " #name "_bmp_end\n" #name "_bmp_end:\n.previous"); \
    extern const uint8_t name##_bmp[], name##_bmp_end[];
ASSET(splash, "assets/splash.bmp")
ASSET(logo, "assets/logo.bmp")  /* the README's */
ASSET(font, "assets/font.bmp")  /* ASCII 32-126, 16x6 cells of 40x80, baseline at 60, 36 px advance */

/* ---- the game's menu sounds: WAVs in data/sound/sd.afs ---- */

enum { SND_CURSOR, SND_OK };
static SDL_AudioDeviceID snd_dev;
static struct { Uint8 *pcm; Uint32 len; } snd[2];

static void sound_init(void) {
    static const int ids[2] = {0, 3};  /* what the title menu plays on a move and a choice, at full volume as there */
    FILE *f = fopen("data/sound/sd.afs", "rb");
    SDL_AudioSpec spec;
    uint32_t n, ent[2];
    for (int i = 0; f && i < 2; i++) {
        uint8_t *wav;
        if (fseek(f, 8 + 8 * ids[i], SEEK_SET) || fread(ent, 4, 2, f) != 2 || ent[1] > (1u << 20)) break;
        if (fseek(f, ent[0], SEEK_SET) || !(wav = malloc(ent[1]))) break;
        if ((n = fread(wav, 1, ent[1], f)) == ent[1]) SDL_LoadWAV_RW(SDL_RWFromConstMem(wav, n), 1, &spec, &snd[i].pcm, &snd[i].len);
        free(wav);
    }
    if (f) fclose(f);
    if (!snd[SND_CURSOR].pcm || !snd[SND_OK].pcm) { rt_log("launcher: no menu sounds in sd.afs"); return; }
    if ((snd_dev = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0))) SDL_PauseAudioDevice(snd_dev, 0);  /* both 22 kHz mono */
    else rt_log("launcher: audio: %s", SDL_GetError());
}

static void sound(int i) {
    if (!snd_dev) return;
    SDL_ClearQueuedAudio(snd_dev);
    SDL_QueueAudio(snd_dev, snd[i].pcm, snd[i].len);
}

static void sound_close(void) {
    for (Uint64 t0 = SDL_GetTicks64(); snd_dev && SDL_GetQueuedAudioSize(snd_dev) && SDL_GetTicks64() - t0 < 1000;) SDL_Delay(10);
    if (snd_dev) SDL_CloseAudioDevice(snd_dev);
    for (int i = 0; i < 2; i++) SDL_FreeWAV(snd[i].pcm);
}

/* Loads a BMP to RGBA rows; NULL if it can't. */
static uint32_t *decode(const uint8_t *bmp, const uint8_t *end, int *w, int *h) {
    SDL_Surface *s = SDL_LoadBMP_RW(SDL_RWFromConstMem(bmp, (int)(end - bmp)), 1), *c;
    uint32_t *px = NULL;
    if (s && (c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0))) {
        *w = c->w;
        *h = c->h;
        if ((px = malloc((size_t)*w * *h * 4)))
            for (int y = 0; y < *h; y++) memcpy(px + (size_t)y * *w, (uint8_t *)c->pixels + (size_t)y * c->pitch, (size_t)*w * 4);
        SDL_FreeSurface(c);
    }
    SDL_FreeSurface(s);
    return px;
}

static GLuint tex, fbo;
static int tex_w, tex_h;

/* Uploads w x h RGBA rows (row 0 at the top) and shows them letterboxed. */
static void present(const uint32_t *px, int w, int h) {
    if (w != tex_w || h != tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        tex_w = w;
        tex_h = h;
    } else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int ww, wh;
    SDL_GL_GetDrawableSize(rt_window, &ww, &wh);
    glClear(GL_COLOR_BUFFER_BIT);
    /* the texture's row 0 is the image's top, the window's row 0 its bottom */
    float s = fminf((float)ww / w, (float)wh / h);
    int dw = w * s, dh = h * s, dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBlitFramebuffer(0, 0, w, h, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    SDL_GL_SwapWindow(rt_window);
}

/* ---- the canvas ---- */

static uint32_t canvas[CW * CH];
static uint32_t *font_px;
static int font_w;

static uint32_t rgb(int r, int g, int b) { return 0xFF000000u | b << 16 | g << 8 | r; }  /* RGBA bytes */

/* Text at x (left edge) and baseline y; size = glyph height in px (80 = the atlas'), sampled bilinearly. */
static int text(int x, int y, int size, uint32_t color, const char *s) {
    float k = size / 80.0f;
    int adv = (int)(36 * k + 0.5f), cw = (int)(40 * k + 0.5f), ch = size, top = y - (int)(60 * k);
    for (; *s; s++, x += adv) {
        int c = (unsigned char)*s - 32;
        if (c <= 0 || c >= 95 || !font_px) continue;
        int gx = c % 16 * 40, gy = c / 16 * 80;
        for (int py = 0; py < ch; py++) {
            int oy = top + py;
            if (oy < 0 || oy >= CH) continue;
            float fy = (py + 0.5f) / k - 0.5f;
            int y0 = (int)fy;
            float ty = fy - y0;
            if (y0 < 0) y0 = 0, ty = 0;
            if (y0 >= 79) y0 = 78, ty = 1;
            for (int px = 0; px < cw; px++) {
                int ox = x + px;
                if (ox < 0 || ox >= CW) continue;
                float fx = (px + 0.5f) / k - 0.5f;
                int x0 = (int)fx;
                float tx = fx - x0;
                if (x0 < 0) x0 = 0, tx = 0;
                if (x0 >= 39) x0 = 38, tx = 1;
                const uint32_t *r0 = font_px + (size_t)(gy + y0) * font_w + gx + x0, *r1 = r0 + font_w;
                float a = ((r0[0] >> 24) * (1 - tx) + (r0[1] >> 24) * tx) * (1 - ty) +
                          ((r1[0] >> 24) * (1 - tx) + (r1[1] >> 24) * tx) * ty;
                if (a < 1) continue;
                uint32_t *d = &canvas[oy * CW + ox], o = 0xFF000000u;
                for (int sh = 0; sh < 24; sh += 8) {
                    float v = ((*d >> sh) & 0xFF) + (((color >> sh) & 0xFF) - (float)((*d >> sh) & 0xFF)) * a / 255;
                    o |= (uint32_t)v << sh;
                }
                *d = o;
            }
        }
    }
    return x;
}
/* An RGBA image scaled bilinearly into the canvas rect x, y, dw, dh, blended by its alpha. */
static void image(const uint32_t *img, int w, int h, int x, int y, int dw, int dh) {
    for (int py = 0; py < dh; py++) {
        float fy = (py + 0.5f) * h / dh - 0.5f;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < h ? y0 + 1 : y0;
        float ty = fy < 0 ? 0 : fy - y0;
        for (int px = 0; px < dw; px++) {
            float fx = (px + 0.5f) * w / dw - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < w ? x0 + 1 : x0;
            float tx = fx < 0 ? 0 : fx - x0;
            const uint32_t q[4] = {img[y0 * w + x0], img[y0 * w + x1], img[y1 * w + x0], img[y1 * w + x1]};
            float k[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty}, a = 0, c[3] = {0};
            for (int i = 0; i < 4; i++) {
                float qa = (q[i] >> 24) * k[i];
                a += qa;
                for (int j = 0; j < 3; j++) c[j] += ((q[i] >> 8 * j) & 0xFF) * qa;
            }
            if (a < 1) continue;
            uint32_t *d = &canvas[(y + py) * CW + x + px], o = 0xFF000000u;
            for (int j = 0; j < 3; j++) {
                float dv = (*d >> 8 * j) & 0xFF;
                o |= (uint32_t)(dv + (c[j] / a - dv) * a / 255) << 8 * j;
            }
            *d = o;
        }
    }
}

static int text_w(int size, const char *s) { return (int)strlen(s) * (int)(36 * size / 80.0f + 0.5f); }
static void text_c(int y, int size, uint32_t color, const char *s) { text((CW - text_w(size, s)) / 2, y, size, color, s); }

/* ---- sh3.ini ---- */

typedef struct {
    const char *label, *key;
    const char *vals[4], *names[4];  /* NULL-terminated; vals[def] is what the game uses without the key */
    int def, cur, was;
    char other[32];  /* a value the file has that the list doesn't: shown until changed */
} Opt;

static Opt opts[] = {
    {"Resolution", "resolution", {"1280x720", "1920x1080", "960x720", "640x480"}, {"720p", "1080p", "4:3 (960x720)", "4:3 (640x480)"}},
    {"Game threads", "cores", {"3", "1"}, {"Three cores", "One core"}},
};
#define N_OPTS (int)(sizeof opts / sizeof *opts)

static int n_vals(const Opt *o) { int n = 0; while (n < 4 && o->vals[n]) n++; return n; }
static const char *opt_name(const Opt *o) { return o->cur < 0 ? o->other : o->names[o->cur]; }

/* The value after "key =" on an uncommented line, as the game's own parsers (sscanf " key = ") read it. */
static int ini_value(const char *line, const char *key, char *val) {
    char fmt[64];
    snprintf(fmt, sizeof fmt, " %s = %%31[^ \t\r\n;]", key);
    return sscanf(line, fmt, val) == 1;
}

static void ini_load(void) {
    FILE *f = fopen("sh3.ini", "r");
    char line[256], val[32];
    for (int i = 0; i < N_OPTS; i++) opts[i].cur = opts[i].def;
    while (f && fgets(line, sizeof line, f))
        for (int i = 0; i < N_OPTS; i++)
            if (ini_value(line, opts[i].key, val)) {
                Opt *o = &opts[i];
                o->cur = -1;
                snprintf(o->other, sizeof o->other, "%s", val);
                for (int j = 0; j < n_vals(o); j++)
                    if (!strcmp(o->vals[j], val)) o->cur = j;
            }
    if (f) fclose(f);
    for (int i = 0; i < N_OPTS; i++) opts[i].was = opts[i].cur;
}

/* Rewrites the changed keys in place (the last line that sets each, as that's the one the game ends up
 * with) and appends the ones the file lacks; comments and every other line stay as they are. */
static void ini_save(void) {
    char *lines[512];
    int n = 0;
    FILE *f = fopen("sh3.ini", "r");
    char line[256], val[32];
    while (f && n < 512 && fgets(line, sizeof line, f)) lines[n++] = strdup(line);
    if (f) fclose(f);
    if (!(f = fopen("sh3.ini", "w"))) { rt_log("launcher: can't write sh3.ini"); return; }
    for (int i = 0; i < N_OPTS; i++) {
        Opt *o = &opts[i];
        if (o->cur == o->was) continue;
        int last = -1;
        for (int j = 0; j < n; j++)
            if (ini_value(lines[j], o->key, val)) last = j;
        char set[96];
        snprintf(set, sizeof set, "%s=%s\n", o->key, o->vals[o->cur]);
        if (last >= 0) { free(lines[last]); lines[last] = strdup(set); }
        else if (n < 512) {
            if (n && !strchr(lines[n - 1], '\n')) {  /* a last line without its newline */
                lines[n - 1] = realloc(lines[n - 1], strlen(lines[n - 1]) + 2);
                strcat(lines[n - 1], "\n");
            }
            lines[n++] = strdup(set);
        }
        o->was = o->cur;
    }
    for (int j = 0; j < n; j++) { fputs(lines[j], f); free(lines[j]); }
    fclose(f);
}

/* ---- input ---- */

enum { IN_NONE, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_OK, IN_BACK };

static int poll_input(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: exit(0);
        case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
            case SDLK_UP: return IN_UP;
            case SDLK_DOWN: return IN_DOWN;
            case SDLK_LEFT: return IN_LEFT;
            case SDLK_RIGHT: return IN_RIGHT;
            case SDLK_RETURN: case SDLK_SPACE: return IN_OK;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: return IN_BACK;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return IN_UP;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return IN_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return IN_LEFT;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return IN_RIGHT;
#ifdef __SWITCH__  /* Nintendo's way round, as in the game (dinput.c): confirm on A (east), back on B (south) */
            case SDL_CONTROLLER_BUTTON_B: return IN_OK;
            case SDL_CONTROLLER_BUTTON_A: return IN_BACK;
#else
            case SDL_CONTROLLER_BUTTON_A: return IN_OK;
            case SDL_CONTROLLER_BUTTON_B: return IN_BACK;
#endif
            case SDL_CONTROLLER_BUTTON_START: return IN_OK;
            }
            break;
        case SDL_CONTROLLERAXISMOTION: {  /* the left stick, once per push past half way */
            static int held[2];
            int ax = e.caxis.axis;
            if (ax != SDL_CONTROLLER_AXIS_LEFTX && ax != SDL_CONTROLLER_AXIS_LEFTY) break;
            int dir = e.caxis.value > 16000 ? 1 : e.caxis.value < -16000 ? -1 : 0, was = held[ax];
            held[ax] = dir;
            if (dir && dir != was) return ax == SDL_CONTROLLER_AXIS_LEFTX ? (dir > 0 ? IN_RIGHT : IN_LEFT) : (dir > 0 ? IN_DOWN : IN_UP);
            break;
        }
        }
    }
    return IN_NONE;
}

/* ---- screens ---- */

#define C_BG rgb(8, 8, 10)
#define C_TEXT rgb(200, 200, 200)
#define C_DIM rgb(110, 110, 110)
#define C_SEL rgb(220, 40, 30)

static uint32_t *logo_px;
static int logo_w, logo_h;

static void draw_menu(int sel) {
    static const char *items[] = {"Play", "Options"};
    for (int i = 0; i < CW * CH; i++) canvas[i] = C_BG;
    if (logo_px) image(logo_px, logo_w, logo_h, (CW - 800) / 2, 170, 800, 800 * logo_h / logo_w);
    text_c(320, 32, C_DIM, "sh3-nx");
    for (int i = 0; i < 2; i++) text_c(470 + i * 70, 48, i == sel ? C_SEL : C_TEXT, items[i]);
    text_c(680, 24, C_DIM, "A: select");
}

static void draw_options(int sel) {
    for (int i = 0; i < CW * CH; i++) canvas[i] = C_BG;
    text_c(110, 56, C_TEXT, "Options");
    for (int i = 0; i <= N_OPTS; i++) {
        int y = 190 + i * 56;
        uint32_t c = i == sel ? C_SEL : C_TEXT;
        if (i == N_OPTS) { text_c(y + 20, 36, c, "Back"); break; }
        text(140, y, 36, c, opts[i].label);
        char v[64];
        snprintf(v, sizeof v, i == sel ? "< %s >" : "%s", opt_name(&opts[i]));
        text(CW - 140 - text_w(36, v), y, 36, c, v);
    }
    text_c(690, 24, C_DIM, "Left/Right: change   B: back (saves sh3.ini)");
}

/* Fades the canvas in from black, or out to black, then leaves it as it was. Input meanwhile is dropped. */
static void fade(int in) {
    static uint32_t pic[CW * CH];
    memcpy(pic, canvas, sizeof pic);
    for (Uint64 t0 = SDL_GetTicks64(), t = 0; t < FADE_MS;) {
        poll_input();
        t = SDL_GetTicks64() - t0;
        uint32_t k = t >= FADE_MS ? 256 : (uint32_t)(t * 256 / FADE_MS);
        if (!in) k = 256 - k;
        for (int i = 0; i < CW * CH; i++)
            canvas[i] = 0xFF000000u | ((pic[i] & 0xFF00FFu) * k >> 8 & 0xFF00FFu) | ((pic[i] & 0xFF00u) * k >> 8 & 0xFF00u);
        present(canvas, CW, CH);
    }
    memcpy(canvas, pic, sizeof pic);
}

static void options(void) {
    int sel = 0;
    draw_options(sel);
    fade(1);
    for (int dirty = 0;; ) {
        if (dirty) { draw_options(sel); dirty = 0; }
        present(canvas, CW, CH);
        int in = poll_input();
        if (in == IN_NONE) continue;
        dirty = 1;
        Opt *o = sel < N_OPTS ? &opts[sel] : NULL;
        if (in != IN_BACK) sound(!o && in == IN_OK ? SND_OK : SND_CURSOR);
        if (in == IN_UP) sel = (sel + N_OPTS) % (N_OPTS + 1);
        if (in == IN_DOWN) sel = (sel + 1) % (N_OPTS + 1);
        if (o && (in == IN_LEFT || in == IN_RIGHT || in == IN_OK)) {
            int n = n_vals(o);
            o->cur = o->cur < 0 ? 0 : (o->cur + (in == IN_LEFT ? n - 1 : 1)) % n;
        }
        if (in == IN_BACK || (!o && in == IN_OK)) {
            ini_save();
            fade(0);
            return;
        }
    }
}

static int same_image(const uint8_t *img, const uint8_t *end, const char *sha1) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, w[80];
    uint64_t n = (uint64_t)(end - img), bits = n * 8;
    char hex[41];
    for (uint64_t off = 0; off < n + 9; off += 64) {
        uint8_t b[64];
        for (int i = 0; i < 64; i++) {
            uint64_t k = off + i;
            b[i] = k < n ? img[k] : k == n ? 0x80 : 0;
        }
        if (off + 64 >= n + 9)
            for (int i = 0; i < 8; i++) b[63 - i] = (uint8_t)(bits >> 8 * i);
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
        for (int i = 16; i < 80; i++) { uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = x << 1 | x >> 31; }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f = i < 20 ? (bb & c) | (~bb & d) : i < 40 || i >= 60 ? bb ^ c ^ d : (bb & c) | (bb & d) | (c & d);
            uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d, d = c, c = bb << 30 | bb >> 2, bb = a, a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
    }
    for (int i = 0; i < 5; i++) snprintf(hex + 8 * i, 9, "%08x", h[i]);
    return !strcmp(hex, sha1);
}

static int warning_shown;

/* A warning screen: wait_ms after it has faded in, "Press A" fades in, and the screen waits for it. */
static void splash(const uint8_t *img, const uint8_t *end, Uint64 wait_ms, const char *sha1) {
    warning_shown = same_image(img, end, sha1);
    int w, h;
    uint32_t *px = decode(img, end, &w, &h);
    if (!px) { rt_log("launcher: can't decode a splash image"); return; }
    for (int i = 0; i < CW * CH; i++) canvas[i] = rgb(0, 0, 0);
    int dw = CW, dh = CW * h / w;
    if (dh > CH) dh = CH, dw = CH * w / h;
    image(px, w, h, (CW - dw) / 2, (CH - dh) / 2, dw, dh);
    free(px);
    fade(1);
    Uint64 t0 = SDL_GetTicks64();
    while (SDL_GetTicks64() - t0 < wait_ms) { poll_input(); present(canvas, CW, CH); }
    static uint32_t pic[CW * CH];
    memcpy(pic, canvas, sizeof pic);
    for (Uint64 t1 = SDL_GetTicks64(), t = 0; poll_input() != IN_OK || t < FADE_MS;) {
        t = SDL_GetTicks64() - t1;
        int k = t >= FADE_MS ? 256 : (int)(t * 256 / FADE_MS);  /* the text's colour from black up */
        memcpy(canvas, pic, sizeof pic);
        text_c(CH - 40, 32, rgb(235 * k >> 8, 235 * k >> 8, 235 * k >> 8), "Press A");  /* the picture's corners are taken */
        present(canvas, CW, CH);
    }
    sound(SND_OK);
    fade(0);
}

void rt_game_setup(void);  /* rt_core.c */

void launcher_run(void) {
#ifndef __SWITCH__
    /* Scripted runs (SH3_KEYS) go straight to the game; SH3_LAUNCHER=1 keeps the launcher for testing it. */
    if (getenv("SH3_KEYS") && !getenv("SH3_LAUNCHER")) { rt_game_setup(); return; }
#endif
    rt_window_open();
    gl_context_open();
    int fh;
    font_px = decode(font_bmp, font_bmp_end, &font_w, &fh);
    for (int i = 0; font_px && i < font_w * fh; i++) font_px[i] = font_px[i] << 24 | 0xFFFFFFu;  /* gray = coverage */
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glClearColor(0, 0, 0, 1);
    logo_px = decode(logo_bmp, logo_bmp_end, &logo_w, &logo_h);
    sound_init();
    splash(splash_bmp, splash_bmp_end, 4000, SPLASH_SHA1);
    ini_load();
    draw_menu(0);
    fade(1);
    for (int sel = 0, dirty = 0;; ) {
        if (dirty) { draw_menu(sel); dirty = 0; }
        present(canvas, CW, CH);
        int in = poll_input();
        if (in == IN_UP || in == IN_DOWN) sel ^= 1, dirty = 1, sound(SND_CURSOR);
        if (in == IN_OK) sound(SND_OK);
        if (in == IN_OK && sel == 0) { fade(0); break; }
        if (in == IN_OK && sel == 1) {
            fade(0);
            options();
            draw_menu(sel);
            fade(1);
        }
    }
    /* leave GL as the game expects a new context: framebuffer 0, nothing bound */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    free(font_px);
    free(logo_px);
    sound_close();
    if (warning_shown) rt_game_setup();
}
