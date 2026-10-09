#define GL_LOADER_IMPL
#include "gl.h"
#include "../runtime/rt.h"

#define GL_DEFINE(type, name) type p_##name;
GL_FUNCS(GL_DEFINE)

void gl_load(void) {
#define GL_LOAD_FN(type, name) if (!(p_##name = (type)SDL_GL_GetProcAddress(#name))) rt_fatal("missing GL function " #name);
    GL_FUNCS(GL_LOAD_FN)
}
