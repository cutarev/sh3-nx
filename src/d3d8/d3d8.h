/* Direct3D 8 on OpenGL 4.5: shared state between the device, resources and shaders. */
#pragma once
#include "../runtime/rt.h"
#include "gl.h"

/* D3DFORMAT */
enum {
    FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_A8 = 28, FMT_X4R4G4B4 = 30, FMT_P8 = 41, FMT_L8 = 50,
    FMT_A8L8 = 51, FMT_A4L4 = 52, FMT_V8U8 = 60, FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73,
    FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D24X4S4 = 79, FMT_D16 = 80, FMT_VERTEXDATA = 100, FMT_INDEX16 = 101,
    FMT_INDEX32 = 102,
    FMT_DXT1 = 0x31545844, FMT_DXT2 = 0x32545844, FMT_DXT3 = 0x33545844, FMT_DXT4 = 0x34545844,
    FMT_DXT5 = 0x35545844,
};
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define E_NOINTERFACE 0x80004002u

enum { RES_TEXTURE = 1, RES_CUBE, RES_SURFACE, RES_VB, RES_IB };

typedef struct Res {
    int kind;
    uint32_t obj, refs;
    uint32_t fmt, usage, pool, w, h, levels;
    GLuint tex;                  /* textures; surfaces that are render or depth targets own one too */
    uint32_t surf[6][14];        /* textures: surface objects per face and level */
    struct Res *parent;          /* surfaces: the texture they belong to, or NULL */
    int level, face;
    uint32_t mem, size, pitch;   /* surfaces and buffers: guest copy the game locks */
    int dirty;
    GLuint buf;                  /* vertex and index buffers */
    int is_depth, is_rt;
} Res;

Res *res_of(uint32_t obj);       /* NULL for a NULL object */
void res_addref(uint32_t obj);
void res_release(uint32_t obj);

/* Texture stage, render state and transform indices used by the shaders. */
#define RS_COUNT 256
#define TSS_COUNT 32
#define MAX_STAGES 8

typedef struct Light { uint32_t type; float diffuse[4], specular[4], ambient[4], pos[3], dir[3];
                       float range, falloff, att0, att1, att2, theta, phi; } Light;

typedef struct Dev {
    uint32_t obj;
    uint32_t rs[RS_COUNT];
    uint32_t tss[MAX_STAGES][TSS_COUNT];
    float world[4][16], view[16], proj[16], texm[MAX_STAGES][16];
    float vp_x, vp_y, vp_w, vp_h, vp_minz, vp_maxz;
    float material[17];          /* diffuse, ambient, specular, emissive (rgba each), power */
    Light light[8];
    int light_on[8];
    uint32_t tex[MAX_STAGES];    /* bound texture objects */
    uint32_t stream[16], stride[16];
    uint32_t ib, base_vertex;
    uint32_t vs, ps;             /* FVF code or shader handle; pixel shader handle or 0 */
    float vsc[96][4], psc[8][4];
    uint32_t rt, ds;             /* bound surfaces */
    uint32_t backbuffer, autodepth;
    uint32_t bb_w, bb_h;
    GLuint fbo, vao, stream_buf, index_buf;
    uint32_t palette[256][256];
    uint32_t cur_palette;
} Dev;

extern Dev *dev;

/* d3d8_res.c */
uint32_t res_texture(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, int cube);
uint32_t res_surface(uint32_t w, uint32_t h, uint32_t fmt, int is_rt, int is_depth);
uint32_t res_buffer(int kind, uint32_t size, uint32_t usage, uint32_t fmt_or_fvf, uint32_t pool);
void res_upload_dirty(Res *t);   /* pushes locked texture levels to GL before a draw samples them */
void res_bind_targets(void);     /* attaches dev->rt / dev->ds to the FBO */
/* CopyRects: surface rectangles to another surface (guest copies, then GL). */
uint32_t res_copy_rects(uint32_t src, uint32_t rects, uint32_t n, uint32_t dst, uint32_t points);
void res_snapshot_front(void);  /* keeps the presented frame for GetFrontBuffer */
uint32_t res_read_front(uint32_t dst);
void res_init_vtables(void);

/* d3d8_shader.c */
void shader_bind_for_draw(int pretransformed, uint32_t fvf, const int *attr_present);
uint32_t shader_create_vs(uint32_t decl, uint32_t func);
uint32_t shader_create_ps(uint32_t func);
void shader_delete_vs(uint32_t h);
void shader_delete_ps(uint32_t h);
/* Vertex shader declarations: per input register, which stream, offset and D3DVSDT type. -1 stream if absent. */
typedef struct VsInput { int stream, offset, type; } VsInput;
const VsInput *shader_vs_inputs(uint32_t h);
