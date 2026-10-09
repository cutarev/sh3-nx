/* DSOUND: secondary buffers live in guest memory and are mixed in software into one SDL stream. */
#include "../runtime/rt.h"
#include <SDL2/SDL.h>
#include <math.h>

#define OUT_RATE 44100
#define MAX_BUFS 512

typedef struct {
    uint32_t obj, obj3d, objnotify;   /* guest COM objects: buffer, its 3D interface, its notify interface */
    uint32_t mem, size, flags;
    int channels, bits, rate, freq;
    int volume, pan;                  /* hundredths of dB, -10000..10000 */
    int playing, looping, refs;
    double pos;                       /* play cursor in frames */
    float x, y, z, mindist, maxdist;
    int mode3d;                       /* DS3DMODE_NORMAL 0, HEADRELATIVE 1, DISABLE 2 */
    uint32_t notify_off[32], notify_ev[32], n_notify;
} Buf;

static Buf *bufs[MAX_BUFS];
static SDL_AudioDeviceID audio;
static SDL_mutex *mx;
static float lis_pos[3], lis_front[3] = {0, 0, 1}, lis_top[3] = {0, 1, 0}, rolloff = 1;
static uint32_t ds_obj, listener_obj, primary_obj;
static uint32_t vt_ds, vt_buf, vt_3d, vt_notify, vt_listener;

void rt_signal_event(uint32_t handle);

static Buf *buf_of(uint32_t obj) {
    uint32_t i = MEM32(obj + 4);
    return i < MAX_BUFS ? bufs[i] : NULL;
}
#define BUF buf_of(THIS)

static int frame_bytes(Buf *b) { return b->channels * b->bits / 8; }

static float sample(Buf *b, uint32_t frame, int ch) {
    uint32_t off = frame * frame_bytes(b) + (b->channels > 1 ? ch : 0) * (b->bits / 8);
    if (b->bits == 16) return *(int16_t *)GPTR(b->mem + off) / 32768.0f;
    return (*(uint8_t *)GPTR(b->mem + off) - 128) / 128.0f;
}

static float db_gain(int hundredths) { return hundredths <= -10000 ? 0 : powf(10.0f, hundredths / 2000.0f); }

/* Inverse-distance rolloff and a left/right pan from the listener's right vector. */
static void gains_3d(Buf *b, float *gl, float *gr) {
    float dx = b->x, dy = b->y, dz = b->z;
    if (b->mode3d == 0) { dx -= lis_pos[0]; dy -= lis_pos[1]; dz -= lis_pos[2]; }
    float dist = sqrtf(dx * dx + dy * dy + dz * dz), md = b->mindist > 0 ? b->mindist : 1;
    if (dist > b->maxdist && b->maxdist > 0) dist = b->maxdist;
    float g = dist <= md ? 1 : md / (md + rolloff * (dist - md));
    float rx = lis_top[1] * lis_front[2] - lis_top[2] * lis_front[1];  /* right = up x front (left-handed) */
    float ry = lis_top[2] * lis_front[0] - lis_top[0] * lis_front[2];
    float rz = lis_top[0] * lis_front[1] - lis_top[1] * lis_front[0];
    float side = dist > 1e-4f ? (dx * rx + dy * ry + dz * rz) / dist : 0;
    *gl = g * sqrtf(0.5f * (1 - side));
    *gr = g * sqrtf(0.5f * (1 + side));
}

/* A movie soundtrack (SH2's Bink player fed it; unused until SH3 plays its FMVs): interleaved stereo at
 * OUT_RATE, queued ahead and mixed over the buffers. A FIFO, so sample k plays k / OUT_RATE after the first
 * one, in step with the video clock. */
#define TRACK_FRAMES (OUT_RATE * 4)
static float track[TRACK_FRAMES * 2];
static uint32_t track_rd, track_wr;  /* frames read / written; wr - rd are queued */
static int track_paused;

void ds_track_write(const float *lr, int frames) {
    if (!mx) return;
    SDL_LockMutex(mx);
    for (int f = 0; f < frames && track_wr - track_rd < TRACK_FRAMES; f++, track_wr++) {
        track[2 * (track_wr % TRACK_FRAMES)] = lr[2 * f];
        track[2 * (track_wr % TRACK_FRAMES) + 1] = lr[2 * f + 1];
    }
    SDL_UnlockMutex(mx);
}
void ds_track_clear(void) { if (mx) { SDL_LockMutex(mx); track_rd = track_wr; SDL_UnlockMutex(mx); } }
void ds_track_pause(int on) { track_paused = on; }

static void mix(void *ud, Uint8 *stream, int len) {
    (void)ud;
    float *out = (float *)stream;
    int frames = len / 8;
    memset(out, 0, len);
    SDL_LockMutex(mx);
    for (int f = 0; f < frames && !track_paused && track_rd != track_wr; f++, track_rd++) {
        out[2 * f] = track[2 * (track_rd % TRACK_FRAMES)];
        out[2 * f + 1] = track[2 * (track_rd % TRACK_FRAMES) + 1];
    }
    for (int i = 0; i < MAX_BUFS; i++) {
        Buf *b = bufs[i];
        if (!b || !b->playing || (b->flags & 1)) continue;
        uint32_t nframes = b->size / frame_bytes(b);
        double step = (double)(b->freq ? b->freq : b->rate) / OUT_RATE;
        float vol = db_gain(b->volume), gl = 1, gr = 1;
        if ((b->flags & 0x10) && b->mode3d != 2) gains_3d(b, &gl, &gr);
        else {
            gl = b->pan > 0 ? db_gain(-b->pan) : 1;
            gr = b->pan < 0 ? db_gain(b->pan) : 1;
        }
        for (int f = 0; f < frames; f++) {
            uint32_t p = (uint32_t)b->pos;
            if (p >= nframes) {
                if (!b->looping) { b->playing = 0; b->pos = 0; break; }
                b->pos -= nframes;
                p = (uint32_t)b->pos;
            }
            uint32_t prev_byte = p * frame_bytes(b);
            out[2 * f] += sample(b, p, 0) * vol * gl;
            out[2 * f + 1] += sample(b, p, 1) * vol * gr;
            b->pos += step;
            uint32_t now_byte = ((uint32_t)b->pos % nframes) * frame_bytes(b);
            for (uint32_t n = 0; n < b->n_notify; n++) {  /* cursor crossed a notification offset */
                uint32_t o = b->notify_off[n];
                if (o == 0xFFFFFFFFu) continue;
                if ((prev_byte <= o && o < now_byte) || (now_byte < prev_byte && (o >= prev_byte || o < now_byte)))
                    rt_signal_event(b->notify_ev[n]);
            }
        }
        if (!b->playing)
            for (uint32_t n = 0; n < b->n_notify; n++)
                if (b->notify_off[n] == 0xFFFFFFFFu) rt_signal_event(b->notify_ev[n]);  /* DSBPN_OFFSETSTOP */
    }
    SDL_UnlockMutex(mx);
    for (int i = 0; i < frames * 2; i++) out[i] = out[i] > 1 ? 1 : out[i] < -1 ? -1 : out[i];
    static FILE *wav;  /* debugging: SH3_WAV=file records the mix as raw float32 stereo at OUT_RATE */
    if (!wav && getenv("SH3_WAV")) wav = fopen(getenv("SH3_WAV"), "wb");
    if (wav) fwrite(out, 8, frames, wav);
}

/* ---- IDirectSoundBuffer8 ---- */
static uint32_t new_buffer(uint32_t desc);

METHOD(b_QueryInterface, 3) {
    Buf *b = BUF;
    uint32_t d1 = MEM32(ARG(1));
    uint32_t r = d1 == 0x279AFA86u ? b->obj3d : d1 == 0xB0210783u ? b->objnotify
               : d1 == 0x279AFA84u ? listener_obj : (d1 == 0x6825A449u || d1 == 0x279AFA85u) ? b->obj : 0;
    MEM32(ARG(2)) = r;
    if (r) b->refs++;
    return r ? 0 : 0x80004002u;
}
METHOD(b_AddRef, 1) { return ++BUF->refs; }
METHOD(b_Release, 1) {
    Buf *b = BUF;
    if (!b || --b->refs > 0) return b ? b->refs : 0;
    SDL_LockMutex(mx);
    bufs[MEM32(THIS + 4)] = NULL;
    SDL_UnlockMutex(mx);
    if (!(b->flags & 0x80000001u)) gfree(b->mem);  /* not the primary, not a duplicate */
    free(b);
    return 0;
}
METHOD(b_GetCaps, 2) {
    Buf *b = BUF;
    MEM32(ARG(1) + 4) = b->flags;
    MEM32(ARG(1) + 8) = b->size;
    return 0;
}
METHOD(b_GetCurrentPosition, 3) {
    Buf *b = BUF;
    SDL_LockMutex(mx);
    uint32_t play = b->size ? ((uint32_t)b->pos * frame_bytes(b)) % b->size : 0;
    SDL_UnlockMutex(mx);
    uint32_t ahead = (b->freq ? b->freq : b->rate) / 50 * frame_bytes(b);  /* ~20 ms */
    if (ARG(1)) MEM32(ARG(1)) = play;
    if (ARG(2)) MEM32(ARG(2)) = b->size ? (play + ahead) % b->size : 0;
    return 0;
}
METHOD(b_GetFormat, 4) {
    Buf *b = BUF;
    uint32_t w = ARG(1);
    if (ARG(3)) MEM32(ARG(3)) = 18;
    if (!w) return 0;
    *(uint16_t *)GPTR(w) = 1;
    *(uint16_t *)GPTR(w + 2) = b->channels;
    MEM32(w + 4) = b->rate;
    MEM32(w + 8) = b->rate * frame_bytes(b);
    *(uint16_t *)GPTR(w + 12) = frame_bytes(b);
    *(uint16_t *)GPTR(w + 14) = b->bits;
    if (ARG(2) >= 18) *(uint16_t *)GPTR(w + 16) = 0;
    return 0;
}
METHOD(b_GetVolume, 2) { MEM32(ARG(1)) = BUF->volume; return 0; }
METHOD(b_GetPan, 2) { MEM32(ARG(1)) = BUF->pan; return 0; }
METHOD(b_GetFrequency, 2) { MEM32(ARG(1)) = BUF->freq ? BUF->freq : BUF->rate; return 0; }
METHOD(b_GetStatus, 2) { MEM32(ARG(1)) = BUF->playing ? (1 | (BUF->looping ? 4 : 0)) : 0; return 0; }
/* Lock may wrap past the end: the second region starts at the buffer's beginning. */
METHOD(b_Lock, 8) {
    Buf *b = BUF;
    uint32_t off = ARG(1), n = ARG(2);
    if (ARG(7) & 2) n = b->size;                         /* DSBLOCK_ENTIREBUFFER */
    if (ARG(7) & 1) {                                    /* DSBLOCK_FROMWRITECURSOR */
        uint32_t ahead = (b->freq ? b->freq : b->rate) / 50 * frame_bytes(b);
        off = ((uint32_t)b->pos * frame_bytes(b) + ahead) % (b->size ? b->size : 1);
    }
    if (n > b->size) n = b->size;
    off %= b->size ? b->size : 1;
    uint32_t first = n < b->size - off ? n : b->size - off;
    MEM32(ARG(3)) = b->mem + off;
    MEM32(ARG(4)) = first;
    if (ARG(5)) MEM32(ARG(5)) = first < n ? b->mem : 0;
    if (ARG(6)) MEM32(ARG(6)) = n - first;
    return 0;
}
METHOD(b_Play, 4) {
    Buf *b = BUF;
    SDL_LockMutex(mx);
    b->playing = 1;
    b->looping = ARG(3) & 1;
    SDL_UnlockMutex(mx);
    return 0;
}
METHOD(b_SetCurrentPosition, 2) {
    Buf *b = BUF;
    SDL_LockMutex(mx);
    b->pos = (double)(ARG(1) / frame_bytes(b));
    SDL_UnlockMutex(mx);
    return 0;
}
METHOD(b_SetFormat, 2) { return 0; }
METHOD(b_SetVolume, 2) { BUF->volume = (int32_t)ARG(1); return 0; }
METHOD(b_SetPan, 2) { BUF->pan = (int32_t)ARG(1); return 0; }
METHOD(b_SetFrequency, 2) { BUF->freq = ARG(1); return 0; }
METHOD(b_Stop, 1) {
    SDL_LockMutex(mx);
    BUF->playing = 0;
    SDL_UnlockMutex(mx);
    return 0;
}
METHOD(b_Unlock, 5) { return 0; }
METHOD(b_Restore, 1) { return 0; }
/* DirectSound effects are taken and ignored: GetObjectInPath hands out one effect object that accepts any
 * parameters (SH3 puts I3DL2 reverb on its footsteps, which then play dry; failing SetFX drops the sound). */
METHOD(fx_QueryInterface, 3) { MEM32(ARG(2)) = THIS; return 0; }
METHOD(fx_AddRef, 1) { return 1; }
METHOD(fx_Release, 1) { return 0; }
METHOD(fx_Ok, 2) { return 0; }  /* Set/GetAllParameters, Set/GetPreset, Set/GetQuality */
static const ComEntry fx_vt[] = {
    {"IDirectSoundFX::QueryInterface", fx_QueryInterface}, {"IDirectSoundFX::AddRef", fx_AddRef},
    {"IDirectSoundFX::Release", fx_Release}, {"IDirectSoundFX::SetAllParameters", fx_Ok},
    {"IDirectSoundFX::GetAllParameters", fx_Ok}, {"IDirectSoundFX::SetPreset", fx_Ok},
    {"IDirectSoundFX::GetPreset", fx_Ok}, {"IDirectSoundFX::SetQuality", fx_Ok}, {"IDirectSoundFX::GetQuality", fx_Ok},
};
METHOD(b_SetFX, 4) {
    for (uint32_t i = 0; ARG(3) && i < ARG(1); i++) MEM32(ARG(3) + 4 * i) = 2;  /* DSFXR_LOCSOFTWARE */
    return 0;
}
METHOD(b_AcquireResources, 4) { return 0; }
METHOD(b_GetObjectInPath, 5) {
    static uint32_t fx;
    if (!fx) fx = com_new(com_vtable(fx_vt, sizeof fx_vt / sizeof *fx_vt), 8);
    MEM32(ARG(4)) = fx;
    return 0;
}

static const ComEntry buf_vt[] = {
    {"IDirectSoundBuffer8::QueryInterface", b_QueryInterface}, {"IDirectSoundBuffer8::AddRef", b_AddRef},
    {"IDirectSoundBuffer8::Release", b_Release}, {"IDirectSoundBuffer8::GetCaps", b_GetCaps},
    {"IDirectSoundBuffer8::GetCurrentPosition", b_GetCurrentPosition}, {"IDirectSoundBuffer8::GetFormat", b_GetFormat},
    {"IDirectSoundBuffer8::GetVolume", b_GetVolume}, {"IDirectSoundBuffer8::GetPan", b_GetPan},
    {"IDirectSoundBuffer8::GetFrequency", b_GetFrequency}, {"IDirectSoundBuffer8::GetStatus", b_GetStatus},
    {"IDirectSoundBuffer8::Initialize", NULL}, {"IDirectSoundBuffer8::Lock", b_Lock},
    {"IDirectSoundBuffer8::Play", b_Play}, {"IDirectSoundBuffer8::SetCurrentPosition", b_SetCurrentPosition},
    {"IDirectSoundBuffer8::SetFormat", b_SetFormat}, {"IDirectSoundBuffer8::SetVolume", b_SetVolume},
    {"IDirectSoundBuffer8::SetPan", b_SetPan}, {"IDirectSoundBuffer8::SetFrequency", b_SetFrequency},
    {"IDirectSoundBuffer8::Stop", b_Stop}, {"IDirectSoundBuffer8::Unlock", b_Unlock},
    {"IDirectSoundBuffer8::Restore", b_Restore}, {"IDirectSoundBuffer8::SetFX", b_SetFX},
    {"IDirectSoundBuffer8::AcquireResources", b_AcquireResources}, {"IDirectSoundBuffer8::GetObjectInPath", b_GetObjectInPath},
};

/* ---- IDirectSound3DBuffer8 and IDirectSoundNotify: share the buffer's slot at +4 ---- */
METHOD(s3_AddRef, 1) { return ++BUF->refs; }
METHOD(s3_Release, 1) { return --BUF->refs; }
METHOD(s3_QueryInterface, 3) { return b_QueryInterface_impl(); }
METHOD(s3_SetPosition, 5) { Buf *b = BUF; b->x = ARGF(1); b->y = ARGF(2); b->z = ARGF(3); return 0; }
METHOD(s3_SetMinDistance, 3) { BUF->mindist = ARGF(1); return 0; }
METHOD(s3_SetMaxDistance, 3) { BUF->maxdist = ARGF(1); return 0; }
METHOD(s3_SetMode, 3) { BUF->mode3d = ARG(1); return 0; }
METHOD(s3_Nop3, 3) { return 0; }
METHOD(s3_Nop4, 4) { return 0; }
METHOD(s3_Nop5, 5) { return 0; }
/* DS3DBUFFER: dwSize, vPosition, vVelocity, dwInsideConeAngle, dwOutsideConeAngle, vConeOrientation,
 * lConeOutsideVolume, flMinDistance, flMaxDistance, dwMode. */
METHOD(s3_SetAllParameters, 3) {
    Buf *b = BUF;
    uint32_t p = ARG(1);
    b->x = MEMF(p + 4); b->y = MEMF(p + 8); b->z = MEMF(p + 12);
    b->mindist = MEMF(p + 52); b->maxdist = MEMF(p + 56); b->mode3d = MEM32(p + 60);
    return 0;
}
static const ComEntry s3_vt[] = {
    {"IDirectSound3DBuffer8::QueryInterface", s3_QueryInterface}, {"IDirectSound3DBuffer8::AddRef", s3_AddRef},
    {"IDirectSound3DBuffer8::Release", s3_Release}, {"IDirectSound3DBuffer8::GetAllParameters", NULL},
    {"IDirectSound3DBuffer8::GetConeAngles", NULL}, {"IDirectSound3DBuffer8::GetConeOrientation", NULL},
    {"IDirectSound3DBuffer8::GetConeOutsideVolume", NULL}, {"IDirectSound3DBuffer8::GetMaxDistance", NULL},
    {"IDirectSound3DBuffer8::GetMinDistance", NULL}, {"IDirectSound3DBuffer8::GetMode", NULL},
    {"IDirectSound3DBuffer8::GetPosition", NULL}, {"IDirectSound3DBuffer8::GetVelocity", NULL},
    {"IDirectSound3DBuffer8::SetAllParameters", s3_SetAllParameters}, {"IDirectSound3DBuffer8::SetConeAngles", s3_Nop4},
    {"IDirectSound3DBuffer8::SetConeOrientation", s3_Nop5}, {"IDirectSound3DBuffer8::SetConeOutsideVolume", s3_Nop3},
    {"IDirectSound3DBuffer8::SetMaxDistance", s3_SetMaxDistance}, {"IDirectSound3DBuffer8::SetMinDistance", s3_SetMinDistance},
    {"IDirectSound3DBuffer8::SetMode", s3_SetMode}, {"IDirectSound3DBuffer8::SetPosition", s3_SetPosition},
    {"IDirectSound3DBuffer8::SetVelocity", s3_Nop5},
};

METHOD(n_SetNotificationPositions, 3) {
    Buf *b = BUF;
    SDL_LockMutex(mx);
    b->n_notify = ARG(1) < 32 ? ARG(1) : 32;
    for (uint32_t i = 0; i < b->n_notify; i++) {
        b->notify_off[i] = MEM32(ARG(2) + 8 * i);
        b->notify_ev[i] = MEM32(ARG(2) + 8 * i + 4);
    }
    SDL_UnlockMutex(mx);
    return 0;
}
static const ComEntry notify_vt[] = {
    {"IDirectSoundNotify::QueryInterface", s3_QueryInterface}, {"IDirectSoundNotify::AddRef", s3_AddRef},
    {"IDirectSoundNotify::Release", s3_Release}, {"IDirectSoundNotify::SetNotificationPositions", n_SetNotificationPositions},
};

/* ---- IDirectSound3DListener8 ---- */
METHOD(l_AddRef, 1) { return 1; }
METHOD(l_Release, 1) { return 0; }
METHOD(l_SetPosition, 5) { lis_pos[0] = ARGF(1); lis_pos[1] = ARGF(2); lis_pos[2] = ARGF(3); return 0; }
METHOD(l_SetOrientation, 8) {
    for (int i = 0; i < 3; i++) { lis_front[i] = ARGF(1 + i); lis_top[i] = ARGF(4 + i); }
    return 0;
}
METHOD(l_SetRolloffFactor, 3) { rolloff = ARGF(1); return 0; }
METHOD(l_Nop3, 3) { return 0; }
METHOD(l_Nop5, 5) { return 0; }
METHOD(l_Commit, 1) { return 0; }
METHOD(l_SetAllParameters, 3) {
    uint32_t p = ARG(1);  /* DS3DLISTENER: dwSize, vPosition, vVelocity, vOrientFront, vOrientTop, distance, rolloff, doppler */
    for (int i = 0; i < 3; i++) { lis_pos[i] = MEMF(p + 4 + 4 * i); lis_front[i] = MEMF(p + 28 + 4 * i); lis_top[i] = MEMF(p + 40 + 4 * i); }
    rolloff = MEMF(p + 56);
    return 0;
}
static const ComEntry listener_vt[] = {
    {"IDirectSound3DListener8::QueryInterface", NULL}, {"IDirectSound3DListener8::AddRef", l_AddRef},
    {"IDirectSound3DListener8::Release", l_Release}, {"IDirectSound3DListener8::GetAllParameters", NULL},
    {"IDirectSound3DListener8::GetDistanceFactor", NULL}, {"IDirectSound3DListener8::GetDopplerFactor", NULL},
    {"IDirectSound3DListener8::GetOrientation", NULL}, {"IDirectSound3DListener8::GetPosition", NULL},
    {"IDirectSound3DListener8::GetRolloffFactor", NULL}, {"IDirectSound3DListener8::GetVelocity", NULL},
    {"IDirectSound3DListener8::SetAllParameters", l_SetAllParameters},
    {"IDirectSound3DListener8::SetDistanceFactor", l_Nop3}, {"IDirectSound3DListener8::SetDopplerFactor", l_Nop3},
    {"IDirectSound3DListener8::SetOrientation", l_SetOrientation}, {"IDirectSound3DListener8::SetPosition", l_SetPosition},
    {"IDirectSound3DListener8::SetRolloffFactor", l_SetRolloffFactor}, {"IDirectSound3DListener8::SetVelocity", l_Nop5},
    {"IDirectSound3DListener8::CommitDeferredSettings", l_Commit},
};

/* ---- IDirectSound8 ---- */

/* DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, dwReserved, lpwfxFormat. */
static uint32_t new_buffer(uint32_t desc) {
    uint32_t flags = MEM32(desc + 4), wfx = MEM32(desc + 16);
    if ((flags & 1) && primary_obj) return primary_obj;
    Buf *b = calloc(1, sizeof *b);
    b->flags = flags;
    if (flags & 1) {  /* the primary buffer is never mixed; its 3D listener is what matters */
        b->size = 4; b->channels = 2; b->rate = OUT_RATE; b->bits = 16;
    } else {
        b->size = MEM32(desc + 8);
        b->channels = *(uint16_t *)GPTR(wfx + 2);
        b->rate = MEM32(wfx + 4);
        b->bits = *(uint16_t *)GPTR(wfx + 14);
    }
    b->mem = galloc(b->size);
    if (b->bits == 8) memset(GPTR(b->mem), 0x80, b->size);
    if (rt_trace || getenv("SH3_WAV"))
        rt_log("sound buffer: %u bytes, %d ch, %d Hz, %d bit, flags %X", b->size, b->channels, b->rate, b->bits, flags);
    b->refs = 1;
    b->maxdist = 1e9f;
    b->mindist = 1;
    SDL_LockMutex(mx);
    int slot = -1;
    for (int i = 0; i < MAX_BUFS && slot < 0; i++) if (!bufs[i]) slot = i;
    if (slot < 0) rt_fatal("too many sound buffers");
    bufs[slot] = b;
    SDL_UnlockMutex(mx);
    b->obj = com_new(vt_buf, 16);
    b->obj3d = com_new(vt_3d, 16);
    b->objnotify = com_new(vt_notify, 16);
    MEM32(b->obj + 4) = MEM32(b->obj3d + 4) = MEM32(b->objnotify + 4) = slot;
    if (flags & 1) primary_obj = b->obj;
    return b->obj;
}

/* IDirectSound and IDirectSound8 are the same object here (the movie player asks for IDirectSound). */
METHOD(ds_QueryInterface, 3) {
    uint32_t d1 = MEM32(ARG(1)), ok = d1 == 0x279AFA83u || d1 == 0xC50A7E93u;
    MEM32(ARG(2)) = ok ? THIS : 0;
    return ok ? 0 : 0x80004002u;
}
METHOD(ds_AddRef, 1) { return 1; }
METHOD(ds_Release, 1) { return 0; }
METHOD(ds_CreateSoundBuffer, 4) { MEM32(ARG(2)) = new_buffer(ARG(1)); return 0; }
METHOD(ds_GetCaps, 2) { memset(GPTR(ARG(1) + 4), 0, 92); MEM32(ARG(1) + 4) = 0xF5F; return 0; }
METHOD(ds_DuplicateSoundBuffer, 3) {
    Buf *src = buf_of(ARG(1));
    if (!src) return 0x80070057u;
    uint32_t wfx = galloc(18), desc = galloc(36);
    *(uint16_t *)GPTR(wfx) = 1;
    *(uint16_t *)GPTR(wfx + 2) = src->channels;
    MEM32(wfx + 4) = src->rate;
    *(uint16_t *)GPTR(wfx + 14) = src->bits;
    MEM32(desc + 4) = src->flags;
    MEM32(desc + 8) = src->size;
    MEM32(desc + 16) = wfx;
    uint32_t o = new_buffer(desc);
    Buf *d = buf_of(o);
    gfree(d->mem);
    d->mem = src->mem;   /* duplicates share the sample memory */
    d->flags |= 1u << 31;
    gfree(wfx);
    gfree(desc);
    MEM32(ARG(2)) = o;
    return 0;
}
METHOD(ds_SetCooperativeLevel, 3) { return 0; }
METHOD(ds_Compact, 1) { return 0; }
METHOD(ds_GetSpeakerConfig, 2) { MEM32(ARG(1)) = 4; return 0; }  /* stereo */
METHOD(ds_SetSpeakerConfig, 2) { return 0; }

static const ComEntry ds_vt[] = {
    {"IDirectSound8::QueryInterface", ds_QueryInterface}, {"IDirectSound8::AddRef", ds_AddRef},
    {"IDirectSound8::Release", ds_Release}, {"IDirectSound8::CreateSoundBuffer", ds_CreateSoundBuffer},
    {"IDirectSound8::GetCaps", ds_GetCaps}, {"IDirectSound8::DuplicateSoundBuffer", ds_DuplicateSoundBuffer},
    {"IDirectSound8::SetCooperativeLevel", ds_SetCooperativeLevel}, {"IDirectSound8::Compact", ds_Compact},
    {"IDirectSound8::GetSpeakerConfig", ds_GetSpeakerConfig}, {"IDirectSound8::SetSpeakerConfig", ds_SetSpeakerConfig},
    {"IDirectSound8::Initialize", NULL}, {"IDirectSound8::VerifyCertification", NULL},
};

/* DirectSoundCreate8(guid, out, outer), DSOUND.dll ordinal 11. */
WINAPI(DirectSoundCreate8, "DSOUND#11", 3) {
    if (!ds_obj) {
        mx = SDL_CreateMutex();
        vt_ds = com_vtable(ds_vt, sizeof ds_vt / sizeof *ds_vt);
        vt_buf = com_vtable(buf_vt, sizeof buf_vt / sizeof *buf_vt);
        vt_3d = com_vtable(s3_vt, sizeof s3_vt / sizeof *s3_vt);
        vt_notify = com_vtable(notify_vt, sizeof notify_vt / sizeof *notify_vt);
        vt_listener = com_vtable(listener_vt, sizeof listener_vt / sizeof *listener_vt);
        ds_obj = com_new(vt_ds, 8);
        listener_obj = com_new(vt_listener, 8);
        SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec want = {.freq = OUT_RATE, .format = AUDIO_F32SYS, .channels = 2, .samples = 1024, .callback = mix}, have;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);
        else rt_log("no audio device: %s", SDL_GetError());
    }
    MEM32(ARG(1)) = ds_obj;
    return 0;
}
