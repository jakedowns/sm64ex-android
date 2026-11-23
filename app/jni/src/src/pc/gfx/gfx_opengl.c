#ifdef RAPI_GL

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

// Stereo state lives in gfx_pc.c (set by the Leia bridge).
extern int gfx_stereo_enabled;
extern int gfx_stereo_eye;
extern int gfx_single_eye;
extern int gfx_stereo_eye_width;
#ifdef __ANDROID__
#include <android/log.h>
#endif
extern int gfx_stereo_vp[2][2];
extern int gfx_stereo_toe_pct;
extern int gfx_stereo_zbuf;
extern int gfx_stereo_wire;
extern float gfx_stereo_fps;
extern float gfx_cam_near;
extern float gfx_cam_far;
extern float gfx_cam_tan_half_fov_y;



#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#ifdef __MINGW32__
# define FOR_WINDOWS 1
#else
# define FOR_WINDOWS 0
#endif

#if FOR_WINDOWS || defined(OSX_BUILD)
# define GLEW_STATIC
# include <GL/glew.h>
#endif

#include <SDL.h>

#define GL_GLEXT_PROTOTYPES 1
#ifdef USE_GLES
# include <SDL_opengles2.h>
#else
# include <SDL_opengl.h>
#endif

#include "../platform.h"
#include "../configfile.h"
#include "gfx_cc.h"
#include "gfx_rendering_api.h"

#define TEX_CACHE_STEP 512

// Splat mode config (defined in gfx_pc.c)
extern int gfx_splat_active;
extern int gfx_splat_size_px;
extern int gfx_splat_opacity;
extern int gfx_splat_softness;
extern int gfx_splat_density;
extern int gfx_splat_shape;

struct ShaderProgram {
    uint32_t shader_id;
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[7];
    GLint uniform_locations[5];
    GLint splat_size_loc;
    GLint splat_on_loc;
    GLint splat_opacity_loc;
    GLint splat_softness_loc;
    GLint splat_shape_loc;
    uint8_t attrib_sizes[7];
    uint8_t num_attribs;
    bool used_noise;
};

struct GLTexture {
    GLuint gltex;
    GLfloat size[2];
    bool filter;
};

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static GLuint opengl_vbo;

static int tex_cache_size = 0;
static int num_textures = 0;
static struct GLTexture *tex_cache = NULL;

static struct ShaderProgram *opengl_prg = NULL;
static struct GLTexture *opengl_tex[2];
static int opengl_curtex = 0;

static uint32_t frame_count;

static bool gfx_opengl_z_is_from_0_to_1(void) {
    return false;
}

static void gfx_opengl_vertex_array_set_attribs(struct ShaderProgram *prg) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        glEnableVertexAttribArray(prg->attrib_locations[i]);
        glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE, num_floats * sizeof(float), (void *) (pos * sizeof(float)));
        pos += prg->attrib_sizes[i];
    }
}

static inline void gfx_opengl_set_shader_uniforms(struct ShaderProgram *prg) {
    if (prg->used_noise)
        glUniform1f(prg->uniform_locations[4], (float)frame_count);
    if (prg->splat_size_loc >= 0)
        glUniform1f(prg->splat_size_loc, (float)gfx_splat_size_px);
    if (prg->splat_on_loc >= 0)
        glUniform1f(prg->splat_on_loc, gfx_splat_active ? 1.0f : 0.0f);
    if (prg->splat_opacity_loc >= 0)
        glUniform1f(prg->splat_opacity_loc, gfx_splat_opacity / 100.0f);
    if (prg->splat_softness_loc >= 0)
        glUniform1f(prg->splat_softness_loc, gfx_splat_softness / 100.0f);
    if (prg->splat_shape_loc >= 0)
        glUniform1f(prg->splat_shape_loc, gfx_splat_shape ? 1.0f : 0.0f);
}

static inline void gfx_opengl_set_texture_uniforms(struct ShaderProgram *prg, const int tile) {
    if (prg->used_textures[tile] && opengl_tex[tile]) {
        glUniform2f(prg->uniform_locations[tile*2 + 0], opengl_tex[tile]->size[0], opengl_tex[tile]->size[1]);
        glUniform1i(prg->uniform_locations[tile*2 + 1], opengl_tex[tile]->filter);
    }
}

static void gfx_opengl_unload_shader(struct ShaderProgram *old_prg) {
    if (old_prg != NULL) {
        for (int i = 0; i < old_prg->num_attribs; i++)
            glDisableVertexAttribArray(old_prg->attrib_locations[i]);
        if (old_prg == opengl_prg)
            opengl_prg = NULL;
    } else {
        opengl_prg = NULL;
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram *new_prg) {
    opengl_prg = new_prg;
    glUseProgram(new_prg->opengl_program_id);
    gfx_opengl_vertex_array_set_attribs(new_prg);
    gfx_opengl_set_shader_uniforms(new_prg);
    gfx_opengl_set_texture_uniforms(new_prg, 0);
    gfx_opengl_set_texture_uniforms(new_prg, 1);
}

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "vec4(texelVal0.a, texelVal0.a, texelVal0.a, texelVal0.a)" : "vec3(texelVal0.a, texelVal0.a, texelVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
        }
    }
}

static void append_formula(char *buf, size_t *len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(uint32_t shader_id) {
    uint8_t c[2][4];
    for (int i = 0; i < 4; i++) {
        c[0][i] = (shader_id >> (i * 3)) & 7;
        c[1][i] = (shader_id >> (12 + i * 3)) & 7;
    }
    bool opt_alpha = (shader_id & SHADER_OPT_ALPHA) != 0;
    bool opt_fog = (shader_id & SHADER_OPT_FOG) != 0;
    bool opt_texture_edge = (shader_id & SHADER_OPT_TEXTURE_EDGE) != 0;
    bool opt_noise = (shader_id & SHADER_OPT_NOISE) != 0;

    bool used_textures[2] = { 0, 0 };
    int num_inputs = 0;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            if (c[i][j] >= SHADER_INPUT_1 && c[i][j] <= SHADER_INPUT_4) {
                if (c[i][j] > num_inputs) {
                    num_inputs = c[i][j];
                }
            }
            if (c[i][j] == SHADER_TEXEL0 || c[i][j] == SHADER_TEXEL0A) {
                used_textures[0] = true;
            }
            if (c[i][j] == SHADER_TEXEL1) {
                used_textures[1] = true;
            }
        }
    }
    bool do_single[2] = { c[0][2] == 0, c[1][2] == 0 };
    bool do_multiply[2] = { c[0][1] == 0 && c[0][3] == 0, c[1][1] == 0 && c[1][3] == 0 };
    bool do_mix[2] = { c[0][1] == c[0][3], c[1][1] == c[1][3] };
    bool color_alpha_same = (shader_id & 0xfff) == ((shader_id >> 12) & 0xfff);

    char vs_buf[1024];
    char fs_buf[2048];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;

    // Vertex shader
#ifdef USE_GLES
    append_line(vs_buf, &vs_len, "#version 100");
#else
    append_line(vs_buf, &vs_len, "#version 120");
#endif
    append_line(vs_buf, &vs_len, "attribute vec4 aVtxPos;");
    append_line(vs_buf, &vs_len, "uniform float uSplatSize;");
    append_line(vs_buf, &vs_len, "uniform float uSplatShape;");
    append_line(vs_buf, &vs_len, "varying float vSplatSeed;");
    if (used_textures[0] || used_textures[1]) {
        append_line(vs_buf, &vs_len, "attribute vec2 aTexCoord;");
        append_line(vs_buf, &vs_len, "varying vec2 vTexCoord;");
        num_floats += 2;
    }
    if (opt_fog) {
        append_line(vs_buf, &vs_len, "attribute vec4 aFog;");
        append_line(vs_buf, &vs_len, "varying vec4 vFog;");
        num_floats += 4;
    }
    for (int i = 0; i < num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "attribute vec%d aInput%d;\n", opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "varying vec%d vInput%d;\n", opt_alpha ? 4 : 3, i + 1);
        num_floats += opt_alpha ? 4 : 3;
    }
    append_line(vs_buf, &vs_len, "void main() {");
    if (used_textures[0] || used_textures[1]) {
        append_line(vs_buf, &vs_len, "vTexCoord = aTexCoord;");
    }
    if (opt_fog) {
        append_line(vs_buf, &vs_len, "vFog = aFog;");
    }
    for (int i = 0; i < num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }
    append_line(vs_buf, &vs_len, "vSplatSeed = fract(sin(dot(aVtxPos.xy, vec2(12.9898, 78.233))) * 43758.5453);");
    append_line(vs_buf, &vs_len, "gl_Position = aVtxPos;");
    append_line(vs_buf, &vs_len, "gl_PointSize = uSplatSize * (1.0 + 0.35 * uSplatShape);");
    append_line(vs_buf, &vs_len, "}");

    // Fragment shader
#ifdef USE_GLES
    append_line(fs_buf, &fs_len, "#version 100");
    append_line(fs_buf, &fs_len, "precision mediump float;");
#else
    append_line(fs_buf, &fs_len, "#version 120");
#endif
    append_line(fs_buf, &fs_len, "uniform float uSplatOn;\n"
                                  "uniform float uSplatOpacity;\n"
                                  "uniform float uSplatSoftness;\n"
                                  "uniform float uSplatShape;");
    append_line(fs_buf, &fs_len, "varying float vSplatSeed;");

    if (used_textures[0] || used_textures[1]) {
        append_line(fs_buf, &fs_len, "varying vec2 vTexCoord;");
    }
    if (opt_fog) {
        append_line(fs_buf, &fs_len, "varying vec4 vFog;");
    }
    for (int i = 0; i < num_inputs; i++) {
        fs_len += sprintf(fs_buf + fs_len, "varying vec%d vInput%d;\n", opt_alpha ? 4 : 3, i + 1);
    }
    if (used_textures[0]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex0;");
        append_line(fs_buf, &fs_len, "uniform vec2 uTex0Size;");
        append_line(fs_buf, &fs_len, "uniform bool uTex0Filter;");
    }
    if (used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex1;");
        append_line(fs_buf, &fs_len, "uniform vec2 uTex1Size;");
        append_line(fs_buf, &fs_len, "uniform bool uTex1Filter;");
    }

    // 3 point texture filtering
    // Original author: ArthurCarvalho
    // Slightly modified GLSL implementation by twinaphex, mupen64plus-libretro project.

    if (used_textures[0] || used_textures[1]) {
        if (configFiltering == 2) {
            append_line(fs_buf, &fs_len, "#define TEX_OFFSET(off) texture2D(tex, texCoord - (off)/texSize)");
            append_line(fs_buf, &fs_len, "vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {");
            append_line(fs_buf, &fs_len, "  vec2 offset = fract(texCoord*texSize - vec2(0.5));");
            append_line(fs_buf, &fs_len, "  offset -= step(1.0, offset.x + offset.y);");
            append_line(fs_buf, &fs_len, "  vec4 c0 = TEX_OFFSET(offset);");
            append_line(fs_buf, &fs_len, "  vec4 c1 = TEX_OFFSET(vec2(offset.x - sign(offset.x), offset.y));");
            append_line(fs_buf, &fs_len, "  vec4 c2 = TEX_OFFSET(vec2(offset.x, offset.y - sign(offset.y)));");
            append_line(fs_buf, &fs_len, "  return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);");
            append_line(fs_buf, &fs_len, "}");
            append_line(fs_buf, &fs_len, "vec4 sampleTex(in sampler2D tex, in vec2 uv, in vec2 texSize, in bool dofilter) {");
            append_line(fs_buf, &fs_len, "if (dofilter)");
            append_line(fs_buf, &fs_len, "return filter3point(tex, uv, texSize);");
            append_line(fs_buf, &fs_len, "else");
            append_line(fs_buf, &fs_len, "return texture2D(tex, uv);");
            append_line(fs_buf, &fs_len, "}");
        } else {
            append_line(fs_buf, &fs_len, "vec4 sampleTex(in sampler2D tex, in vec2 uv, in vec2 texSize, in bool dofilter) {");
            append_line(fs_buf, &fs_len, "return texture2D(tex, uv);");
            append_line(fs_buf, &fs_len, "}");
        }
    }

    if (opt_alpha && opt_noise) {
        append_line(fs_buf, &fs_len, "uniform float frame_count;");

        append_line(fs_buf, &fs_len, "float random(in vec3 value) {");
        append_line(fs_buf, &fs_len, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
#ifdef USE_GLES
        append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143.7585453);");
#else
        append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143758.5453);");
#endif
        append_line(fs_buf, &fs_len, "}");
    }

    append_line(fs_buf, &fs_len, "void main() {");

    if (used_textures[0]) {
        append_line(fs_buf, &fs_len, "vec4 texVal0 = sampleTex(uTex0, vTexCoord, uTex0Size, uTex0Filter);");
    }
    if (used_textures[1]) {
        append_line(fs_buf, &fs_len, "vec4 texVal1 = sampleTex(uTex1, vTexCoord, uTex1Size, uTex1Filter);");
    }

    append_str(fs_buf, &fs_len, opt_alpha ? "vec4 texel = " : "vec3 texel = ");
    if (!color_alpha_same && opt_alpha) {
        append_str(fs_buf, &fs_len, "vec4(");
        append_formula(fs_buf, &fs_len, c, do_single[0], do_multiply[0], do_mix[0], false, false, true);
        append_str(fs_buf, &fs_len, ", ");
        append_formula(fs_buf, &fs_len, c, do_single[1], do_multiply[1], do_mix[1], true, true, true);
        append_str(fs_buf, &fs_len, ")");
    } else {
        append_formula(fs_buf, &fs_len, c, do_single[0], do_multiply[0], do_mix[0], opt_alpha, false, opt_alpha);
    }
    append_line(fs_buf, &fs_len, ";");

    if (opt_texture_edge && opt_alpha) {
        append_line(fs_buf, &fs_len, "if (texel.a > 0.3) texel.a = 1.0; else discard;");
    }
    // TODO discard if alpha is 0?
    if (opt_fog) {
        if (opt_alpha) {
            append_line(fs_buf, &fs_len, "texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

    if (opt_alpha && opt_noise) 
        append_line(fs_buf, &fs_len, "texel.a *= floor(random(floor(vec3(gl_FragCoord.xy, frame_count))) + 0.5);");

    if (opt_alpha) {
        append_line(fs_buf, &fs_len, "vec4 outColor = texel;");
    } else {
        append_line(fs_buf, &fs_len, "vec4 outColor = vec4(texel, 1.0);");
    }
    // Splat mode: soft elliptical fleck from the point-sprite footprint.
    // uSplatOn is 0 for every normal triangle batch.
    append_line(fs_buf, &fs_len, "if (uSplatOn > 0.5) {");
    append_line(fs_buf, &fs_len, "vec2 sp = gl_PointCoord - 0.5;");
    append_line(fs_buf, &fs_len, "float r2;");
    append_line(fs_buf, &fs_len, "if (uSplatShape < 0.5) {");
    append_line(fs_buf, &fs_len, "r2 = dot(sp, sp) * 4.0;");
    append_line(fs_buf, &fs_len, "} else {");
    // Paint-stroke brush: randomly rotate and stretch each splat, then carve a
    // ragged edge with layered sine noise.  Three aspect variants read as a
    // small brush set rather than one repeated stamp.
    append_line(fs_buf, &fs_len, "float ang = vSplatSeed * 6.2831853;");
    append_line(fs_buf, &fs_len, "float ca = cos(ang), sa = sin(ang);");
    append_line(fs_buf, &fs_len, "vec2 q = vec2(ca * sp.x - sa * sp.y, sa * sp.x + ca * sp.y);");
    append_line(fs_buf, &fs_len, "float variant = floor(vSplatSeed * 3.0);");
    append_line(fs_buf, &fs_len, "if (variant < 1.0) { q.x *= 2.2; q.y *= 0.42; }");
    append_line(fs_buf, &fs_len, "else if (variant < 2.0) { q.x *= 1.6; q.y *= 0.62; }");
    append_line(fs_buf, &fs_len, "else { q.x *= 2.8; q.y *= 0.32; }");
    append_line(fs_buf, &fs_len, "q.y *= 1.0 - 0.35 * abs(q.x);");
    append_line(fs_buf, &fs_len, "float ragged = 0.10 * sin(q.x * 23.0 + vSplatSeed * 41.0)");
    append_line(fs_buf, &fs_len, "             + 0.07 * sin(q.y * 31.0 - vSplatSeed * 17.0)");
    append_line(fs_buf, &fs_len, "             + 0.04 * sin((q.x + q.y) * 57.0 + vSplatSeed * 7.0);");
    append_line(fs_buf, &fs_len, "r2 = dot(q, q) * (1.0 + ragged * 2.5);");
    append_line(fs_buf, &fs_len, "}");
    append_line(fs_buf, &fs_len, "if (r2 > 1.0) discard;");
    append_line(fs_buf, &fs_len, "float edge = 1.0 - r2;");
    append_line(fs_buf, &fs_len, "outColor.a = uSplatOpacity * edge * exp(-r2 * (0.5 + 5.0 * uSplatSoftness));");
    append_line(fs_buf, &fs_len, "}");
    append_line(fs_buf, &fs_len, "gl_FragColor = outColor;");
    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    /*puts("Vertex shader:");
    puts(vs_buf);
    puts("Fragment shader:");
    puts(fs_buf);
    puts("End");*/

    const GLchar *sources[2] = { vs_buf, fs_buf };
    const GLint lengths[2] = { vs_len, fs_len };
    GLint success;

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Vertex shader compilation failed\n");
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        sys_fatal("vertex shader compilation failed (see terminal)");
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Fragment shader compilation failed\n");
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        sys_fatal("fragment shader compilation failed (see terminal)");
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    size_t cnt = 0;

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attrib_sizes[cnt] = 4;
    ++cnt;

    if (used_textures[0] || used_textures[1]) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aTexCoord");
        prg->attrib_sizes[cnt] = 2;
        ++cnt;
    }

    if (opt_fog) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < num_inputs; i++) {
        char name[16];
        sprintf(name, "aInput%d", i + 1);
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attrib_sizes[cnt] = opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->shader_id = shader_id;
    prg->opengl_program_id = shader_program;
    prg->num_inputs = num_inputs;
    prg->used_textures[0] = used_textures[0];
    prg->used_textures[1] = used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;
    prg->splat_size_loc = glGetUniformLocation(shader_program, "uSplatSize");
    prg->splat_on_loc = glGetUniformLocation(shader_program, "uSplatOn");
    prg->splat_opacity_loc = glGetUniformLocation(shader_program, "uSplatOpacity");
    prg->splat_softness_loc = glGetUniformLocation(shader_program, "uSplatSoftness");
    prg->splat_shape_loc = glGetUniformLocation(shader_program, "uSplatShape");

    gfx_opengl_load_shader(prg);

    if (used_textures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        prg->uniform_locations[0] = glGetUniformLocation(shader_program, "uTex0Size");
        prg->uniform_locations[1] = glGetUniformLocation(shader_program, "uTex0Filter");
        glUniform1i(sampler_location, 0);
    }
    if (used_textures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        prg->uniform_locations[2] = glGetUniformLocation(shader_program, "uTex1Size");
        prg->uniform_locations[3] = glGetUniformLocation(shader_program, "uTex1Filter");
        glUniform1i(sampler_location, 1);
    }

    if (opt_alpha && opt_noise) {
        prg->uniform_locations[4] = glGetUniformLocation(shader_program, "frame_count");
        prg->used_noise = true;
    } else {
        prg->used_noise = false;
    }

    return prg;
}

static struct ShaderProgram *gfx_opengl_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static GLuint gfx_opengl_new_texture(void) {
    if (num_textures >= tex_cache_size) {
        tex_cache_size += TEX_CACHE_STEP;
        tex_cache = realloc(tex_cache, sizeof(struct GLTexture) * tex_cache_size);
        if (!tex_cache) sys_fatal("out of memory allocating texture cache");
        // invalidate these because they might be pointing to garbage now
        opengl_tex[0] = NULL;
        opengl_tex[1] = NULL;
    }
    glGenTextures(1, &tex_cache[num_textures].gltex);
    return num_textures++;
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id) {
     opengl_tex[tile] = tex_cache + texture_id;
     opengl_curtex = tile;
     glActiveTexture(GL_TEXTURE0 + tile);
     glBindTexture(GL_TEXTURE_2D, opengl_tex[tile]->gltex);
     gfx_opengl_set_texture_uniforms(opengl_prg, tile);
}

static void gfx_opengl_upload_texture(uint8_t *rgba32_buf, int width, int height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    opengl_tex[opengl_curtex]->size[0] = width;
    opengl_tex[opengl_curtex]->size[1] = height;
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const GLenum filter = linear_filter ? GL_LINEAR : GL_NEAREST;
    glActiveTexture(GL_TEXTURE0 + tile);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
    opengl_curtex = tile;
    if (opengl_tex[tile]) {
        opengl_tex[tile]->filter = linear_filter;
        gfx_opengl_set_texture_uniforms(opengl_prg, tile);
    }
}

// N64 semantics: with the Z-buffer off there are no depth writes at all.
// GL still writes depth while the test is disabled unless the mask is off,
// which made the skybox stamp near depth over the whole scene.
static bool s_depth_test_on = true;
static bool s_depth_write_wanted = true;

static void gfx_opengl_set_depth_test(bool depth_test) {
    s_depth_test_on = depth_test;
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask((s_depth_test_on && s_depth_write_wanted) ? GL_TRUE : GL_FALSE);
}

static void gfx_opengl_set_depth_mask(bool z_upd) {
    s_depth_write_wanted = z_upd;
    glDepthMask((s_depth_test_on && z_upd) ? GL_TRUE : GL_FALSE);
}

static void gfx_opengl_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        glPolygonOffset(-2, -2);
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glPolygonOffset(0, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

/* ---------------- Stereo: one framebuffer per eye ---------------- */
#ifdef __ANDROID__
static GLuint s_eye_fbo[2] = {0, 0};
static GLuint s_eye_tex[2] = {0, 0};
static GLuint s_eye_depth[2] = {0, 0};
static int    s_eye_w = 0, s_eye_h = 0;
static int    s_fbo_mode = 0;
static int    s_depth_tex_mode = 0;   // 1 if GL_OES_depth_texture is available
static int    s_depth_tex_checked = 0;
#include <string.h>
#include <stdlib.h>
#include <time.h>
static int    s_canvas_x = 0, s_canvas_y = 0;
static int    s_canvas_w = 1, s_canvas_h = 1;

static GLuint s_comp_prog = 0, s_comp_vbo = 0;
static GLint  s_comp_pos = -1, s_comp_uv = -1, s_comp_tex = -1;
static GLint  s_comp_mode = -1, s_comp_solid = -1, s_comp_scale = -1;
static GLint  s_comp_ui = -1, s_comp_hasui = -1, s_comp_uibuf = -1;
int gfx_stereo_in_scale_pct = 100;   // pre-scale of each eye inside the SBS buffer
int gfx_stereo_debug_tint = 0;   // 1 = 50% red/blue overlay per half

static const char *s_comp_vs =
    "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
    "uniform float uScale;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos * uScale, 0.0, 1.0); }\n";
static const char *s_comp_fs =
    "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;\n"
    "uniform float uMode; uniform vec4 uSolid;\n"
    "uniform sampler2D uUI; uniform float uHasUI; uniform float uUIBuf;\n"
    "void main(){\n"
    "  vec4 t = texture2D(uTex, vUV);\n"
    "  vec4 ui = texture2D(uUI, vUV);\n"
    "  float a = uHasUI > 0.5 ? ui.a : 0.0;\n"          // premultiplied coverage
    "  vec3 urgb = uHasUI > 0.5 ? ui.rgb : vec3(0.0);\n"
    "  if (uUIBuf > 0.5) gl_FragColor = vec4(urgb + t.rgb * 0.2 * (1.0 - a), 1.0);\n"
    "  else if (uMode > 1.5) gl_FragColor = vec4(vec3(t.r) * (1.0 - a) + urgb, 1.0);\n"
    "  else if (uMode > 0.5) gl_FragColor = uSolid;\n"
    "  else gl_FragColor = vec4(t.rgb * (1.0 - a) + urgb, 1.0);\n"
    "}\n";

/* ---------------- Screen-space FX (MXAO / CinematicDOF / SSGI-lite) --------
 * A single fullscreen pass runs per eye after the scene is rendered:
 *   colour_out = dof(colour) * ao(colour, depth) + gi(colour, depth)
 * All controls are debug knobs; defaults are off so the v31 path is unchanged.
 */
static int s_fx_ao_on = 0;
static int s_fx_ao_intensity = 100;   // percent
static int s_fx_ao_radius = 120;      // world units
static int s_fx_ao_samples = 12;
static int s_fx_ao_bias = 20;         // world units (self-occlusion reject)
static int s_fx_ao_power = 150;       // percent (contrast)

static int s_fx_dof_on = 0;
static int s_fx_dof_focus = 800;      // world units (manual mode)
static int s_fx_dof_auto = 1;         // 1 = focus on Mario (default)
// Distance to Mario's centre minus his hitbox radius, written by the game code
// each tick (rendering_graph_node.c) so auto-focus tracks him.
float gfx_dof_focus_auto = 800.0f;
static int s_fx_dof_aperture = 100;   // percent
static int s_fx_dof_maxblur = 12;     // pixels
static int s_fx_dof_samples = 16;

static int s_fx_gi_on = 0;
static int s_fx_gi_intensity = 100;   // percent
static int s_fx_gi_radius = 150;      // world units
static int s_fx_gi_steps = 6;
static int s_fx_gi_rays = 4;

// SSRTX: screen-space ray-traced reflections (ReShade SSR/SSRT-style).
static int s_fx_ssrtx_on = 0;
static int s_fx_ssrtx_intensity = 100;  // percent
static int s_fx_ssrtx_distance = 600;   // max ray length (world units)
static int s_fx_ssrtx_steps = 20;
static int s_fx_ssrtx_rays = 1;
static int s_fx_ssrtx_thickness = 80;   // depth window for a hit (world units)
static int s_fx_ssrtx_roughness = 0;    // percent cone spread (needs rays > 1)
static int s_fx_ssrtx_fade = 100;       // screen-edge fade percent

static int s_fx_debug = 0;            // 0 off, 1 AO, 2 CoC, 3 GI
static int s_fx_normal_flip = 0;

// Realtime dynamic shadow map state (owned by gfx_pc.c / rendering_graph_node.c).
extern int gfx_shadow_enabled;
extern int gfx_shadow_quality;
extern int gfx_shadow_interval;
extern int gfx_shadow_strength;
extern int gfx_shadow_bias;
extern int gfx_shadow_normal_bias;
extern int gfx_shadow_softness;
extern int gfx_shadow_taps;
extern int gfx_shadow_range;
extern int gfx_shadow_yaw_deg;
extern int gfx_shadow_pitch_deg;
extern int gfx_shadow_debug;
extern float gfx_shadow_light_vp[4][4];
extern float gfx_cam_inv_mtx[4][4];
extern float gfx_shadow_light_dir[3];
extern int gfx_shadow_matrices_ready;
extern float gfx_eye_shear[2];
extern float gfx_eye_toe[2];
extern int gfx_eye_flip[2];
extern int gfx_splat_active;
extern int gfx_splat_size_px;
extern int gfx_splat_opacity;
extern int gfx_splat_softness;
extern int gfx_splat_density;
extern int gfx_splat_shape;
static int s_shadow_map_valid = 0;    // set by gfx_opengl_shadow_end()

/* Shadow map FBO: RGBA8 colour with packed depth + a depth renderbuffer. */
static GLuint s_shadow_fbo = 0, s_shadow_tex = 0, s_shadow_depth = 0;
static int    s_shadow_res = 0;
static int    s_shadow_rendering = 0;
static GLuint s_shadow_prog = 0;
static int    s_shadow_prog_state = 0;   // 0 = untried, 1 = ok, -1 = failed
static GLint  s_shadow_apos = -1;

static const char *s_fx_vs =
    "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *s_fx_fs =
    "precision highp float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uColor;\n"
    "uniform sampler2D uDepth;\n"
    "uniform vec2 uRes;\n"
    "uniform vec4 uCanvas;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "uniform float uTanHalf;\n"
    "uniform float uAspect;\n"
    "uniform float uFrame;\n"
    "uniform float uAOOn;\n"
    "uniform float uAOIntensity;\n"
    "uniform float uAORadius;\n"
    "uniform float uAOSamples;\n"
    "uniform float uAOBias;\n"
    "uniform float uAOPower;\n"
    "uniform float uDOFOn;\n"
    "uniform float uFocus;\n"
    "uniform float uAperture;\n"
    "uniform float uMaxBlur;\n"
    "uniform float uDOFSamples;\n"
    "uniform float uGIOn;\n"
    "uniform float uGIIntensity;\n"
    "uniform float uGIRadius;\n"
    "uniform float uGISteps;\n"
    "uniform float uGIRays;\n"
    "uniform float uDebug;\n"
    "uniform float uNormalFlip;\n"
    "uniform float uShear;\n"
    "uniform float uToe;\n"
    "uniform float uFlipY;\n"
    "uniform sampler2D uShadowMap;\n"
    "uniform mat4 uShadowVP;\n"
    "uniform mat4 uCamInv;\n"
    "uniform vec3 uShadowLightDir;\n"
    "uniform vec2 uShadowTexel;\n"
    "uniform float uShadowOn;\n"
    "uniform float uShadowStrength;\n"
    "uniform float uShadowBias;\n"
    "uniform float uShadowNormalBias;\n"
    "uniform float uShadowSoftness;\n"
    "uniform float uShadowTaps;\n"
    "uniform float uShadowDebug;\n"
    "uniform float uSSRTXOn;\n"
    "uniform float uSSRTXIntensity;\n"
    "uniform float uSSRTXDist;\n"
    "uniform float uSSRTXSteps;\n"
    "uniform float uSSRTXRays;\n"
    "uniform float uSSRTXThick;\n"
    "uniform float uSSRTXRough;\n"
    "uniform float uSSRTXFade;\n"
    "float unpackDepth(vec4 rgba) {\n"
    "  return dot(rgba, vec4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0));\n"
    "}\n"
    "float shadowLookup(vec3 world, vec3 N) {\n"
    "  vec3 wp = world + N * uShadowNormalBias;\n"
    "  vec4 lp = uShadowVP * vec4(wp, 1.0);\n"
    "  if (lp.w <= 0.0) return 1.0;\n"
    "  vec3 ndc = lp.xyz / lp.w;\n"
    "  vec2 uv = ndc.xy * 0.5 + 0.5;\n"
    "  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;\n"
    "  float edge = smoothstep(0.0, 0.04, min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)));\n"
    "  float ref = ndc.z * 0.5 + 0.5 - uShadowBias * 0.00009;\n"
    "  float lit = 1.0;\n"
    "  float taps = uShadowTaps;\n"
    "  if (taps < 1.5) {\n"
    "    float md = unpackDepth(texture2D(uShadowMap, uv));\n"
    "    lit = (ref > md) ? 0.0 : 1.0;\n"
    "  } else {\n"
    "    float side = taps < 6.0 ? 2.0 : (taps < 12.0 ? 3.0 : 4.0);\n"
    "    float sum = 0.0;\n"
    "    float cnt = 0.0;\n"
    "    float radius = uShadowSoftness * 2.0;\n"
    "    for (int y = 0; y < 4; y++) {\n"
    "      for (int x = 0; x < 4; x++) {\n"
    "        if (float(x) >= side || float(y) >= side) continue;\n"
    "        vec2 o = (vec2(float(x), float(y)) / (side - 1.0) - 0.5) * 2.0 * radius * uShadowTexel;\n"
    "        float md = unpackDepth(texture2D(uShadowMap, uv + o));\n"
    "        sum += (ref > md) ? 0.0 : 1.0;\n"
    "        cnt += 1.0;\n"
    "      }\n"
    "    }\n"
    "    lit = cnt > 0.0 ? sum / cnt : 1.0;\n"
    "  }\n"
    "  float ndl = max(dot(N, uShadowLightDir), 0.0);\n"
    "  float litWeight = smoothstep(0.02, 0.35, ndl);\n"
    "  return mix(1.0, mix(1.0, lit, edge), uShadowStrength * litWeight);\n"
    "}\n"
    "float linZ(float d) {\n"
    "  float z = d * 2.0 - 1.0;\n"
    "  return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));\n"
    "}\n"
    "vec3 viewPos(vec2 cuv, float z) {\n"
    "  vec2 ndc = cuv * 2.0 - 1.0;\n"
    "  float ndcz = (uFar + uNear - 2.0 * uNear * uFar / z) / (uFar - uNear);\n"
    "  if (uFlipY > 0.5) ndc.y = -ndc.y;\n"
    "  ndc.x -= uShear + uToe * ndcz;\n"
    "  return vec3(ndc.x * z * uTanHalf * uAspect,\n"
    "              ndc.y * z * uTanHalf, -z);\n"
    "}\n"
    "vec2 canvasUV(vec2 eyeUV) {\n"
    "  return (eyeUV * uRes - uCanvas.xy) / uCanvas.zw;\n"
    "}\n"
    "vec2 eyeUVfromCanvas(vec2 cuv) {\n"
    "  return (cuv * uCanvas.zw + uCanvas.xy) / uRes;\n"
    "}\n"
    "float hash12(vec2 p) {\n"
    "  return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "}\n"
    // Forward projection: view space -> canvas UV, including the per-eye
    // shear/toe/flip so the reflection ray lands on the right pixel.
    "vec2 viewToCanvas(vec3 vp) {\n"
    "  float z = max(-vp.z, 1.0);\n"
    "  vec2 ndc = vec2(vp.x / (z * uTanHalf * uAspect), vp.y / (z * uTanHalf));\n"
    "  float ndcz = (uFar + uNear - 2.0 * uNear * uFar / z) / (uFar - uNear);\n"
    "  ndc.x += uShear + uToe * ndcz;\n"
    "  if (uFlipY > 0.5) ndc.y = -ndc.y;\n"
    "  return ndc * 0.5 + 0.5;\n"
    "}\n"
    // Screen-space ray-traced reflection: march the reflected view ray through
    // the depth buffer and grab the colour of the first surface it penetrates.
    "vec3 ssrtxReflect(vec3 P, vec3 N, vec2 cuv) {\n"
    "  vec3 V = normalize(P);\n"
    "  vec3 Rbase = reflect(V, N);\n"
    "  if (dot(Rbase, N) <= 0.0) return vec3(0.0);\n"
    "  vec3 acc = vec3(0.0);\n"
    "  float hits = 0.0;\n"
    "  float rays = max(uSSRTXRays, 1.0);\n"
    "  float spread = uSSRTXRough * (rays > 1.5 ? 1.0 : 0.0);\n"
    "  float stepLen = uSSRTXDist / max(uSSRTXSteps, 1.0);\n"
    "  vec3 origin = P + N * max(2.0, uSSRTXThick * 0.05);\n"
    "  vec3 up = abs(Rbase.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);\n"
    "  vec3 T = normalize(cross(up, Rbase));\n"
    "  vec3 B = cross(Rbase, T);\n"
    "  for (int r = 0; r < 8; r++) {\n"
    "    if (float(r) >= rays) break;\n"
    "    float ang = (float(r) + 0.5) / rays * 6.2831853 + hash12(cuv * uRes + float(r)) * 6.2831853;\n"
    "    vec3 R = normalize(Rbase + (T * cos(ang) + B * sin(ang)) * spread * 0.35);\n"
    "    for (int i = 1; i <= 48; i++) {\n"
    "      if (float(i) > uSSRTXSteps) break;\n"
    "      vec3 Ps = origin + R * (float(i) * stepLen);\n"
    "      float rayDist = -Ps.z;\n"
    "      if (rayDist <= uNear) break;\n"
    "      vec2 ruv = viewToCanvas(Ps);\n"
    "      if (ruv.x < 0.0 || ruv.x > 1.0 || ruv.y < 0.0 || ruv.y > 1.0) break;\n"
    "      vec2 euv = eyeUVfromCanvas(ruv);\n"
    "      float sceneDist = linZ(texture2D(uDepth, euv).r);\n"
    "      float d = rayDist - sceneDist;\n"
    "      if (d > 0.0 && d < uSSRTXThick) {\n"
    "        acc += texture2D(uColor, euv).rgb;\n"
    "        hits += 1.0;\n"
    "        break;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  if (hits < 0.5) return vec3(0.0);\n"
    "  float cosT = max(dot(-V, N), 0.0);\n"
    "  float fres = 0.04 + 0.96 * pow(1.0 - cosT, 5.0);\n"
    "  float edge = smoothstep(0.0, 0.08, min(min(cuv.x, 1.0 - cuv.x), min(cuv.y, 1.0 - cuv.y)));\n"
    "  float fade = mix(1.0, edge, uSSRTXFade / 100.0);\n"
    "  return (acc / hits) * fres * fade * (uSSRTXIntensity / 100.0);\n"
    "}\n"
    "void main() {\n"
    "  vec3 color = texture2D(uColor, vUV).rgb;\n"
    "  vec2 cuv = canvasUV(vUV);\n"
    "  if (uDebug < 0.5 && (cuv.x < 0.0 || cuv.x > 1.0 || cuv.y < 0.0 || cuv.y > 1.0)) {\n"
    "    gl_FragColor = vec4(color, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  float depth = texture2D(uDepth, vUV).r;\n"
    "  float z = linZ(depth);\n"
    "  vec3 P = viewPos(cuv, z);\n"
    "  vec2 tx = 1.0 / uRes;\n"
    "  float zx = linZ(texture2D(uDepth, vUV + vec2(tx.x, 0.0)).r);\n"
    "  float zy = linZ(texture2D(uDepth, vUV + vec2(0.0, tx.y)).r);\n"
    "  vec3 Px = viewPos(canvasUV(vUV + vec2(tx.x, 0.0)), zx);\n"
    "  vec3 Py = viewPos(canvasUV(vUV + vec2(0.0, tx.y)), zy);\n"
    "  vec3 N = normalize(cross(Px - P, Py - P));\n"
    "  if (N.z < 0.0) N = -N;\n"
    "  if (uNormalFlip > 0.5) N = -N;\n"
    "  vec3 Pw = (uCamInv * vec4(P, 1.0)).xyz;\n"
    "  vec3 Nw = normalize((uCamInv * vec4(N, 0.0)).xyz);\n"
    "  float shadow = 1.0;\n"
    "  if (uShadowOn > 0.5) shadow = shadowLookup(Pw, Nw);\n"
    "  vec3 refl = vec3(0.0);\n"
    "  if (uSSRTXOn > 0.5) refl = ssrtxReflect(P, N, cuv);\n"
    "  float ao = 1.0;\n"
    "  if (uAOOn > 0.5) {\n"
    "    float occ = 0.0;\n"
    "    float wsum = 0.0;\n"
    "    float ns = max(uAOSamples, 1.0);\n"
    "    for (int i = 0; i < 32; i++) {\n"
    "      if (float(i) >= ns) break;\n"
    "      float fi = (float(i) + 0.5) / ns;\n"
    "      float ang = float(i) * 2.39996323 + uFrame * 0.618;\n"
    "      vec2 dir = vec2(cos(ang), sin(ang));\n"
    "      float r = uAORadius * fi;\n"
    "      vec2 ndcOff = dir * r / (z * uTanHalf);\n"
    "      ndcOff.x /= uAspect;\n"
    "      vec2 suv = vUV + ndcOff * 0.5 * uCanvas.zw / uRes;\n"
    "      vec2 scuv = canvasUV(suv);\n"
    "      wsum += 1.0;\n"
    "      if (scuv.x < 0.0 || scuv.x > 1.0 || scuv.y < 0.0 || scuv.y > 1.0) continue;\n"
    "      float sz = linZ(texture2D(uDepth, suv).r);\n"
    "      if (abs(sz - z) < uAOBias) continue;\n"
    "      vec3 d = viewPos(scuv, sz) - P;\n"
    "      float dist = length(d);\n"
    "      if (dist < 0.001 || dist > uAORadius * 2.0) continue;\n"
    "      float ndl = max(dot(N, d / dist), 0.0);\n"
    "      float fall = 1.0 - dist / (uAORadius * 2.0);\n"
    "      occ += ndl * fall * fall;\n"
    "    }\n"
    "    float amount = clamp(occ / max(wsum, 1.0) * (uAOIntensity / 100.0) * 3.0, 0.0, 1.0);\n"
    "    ao = pow(1.0 - amount, max(uAOPower, 1.0) / 100.0);\n"
    "  }\n"
    "  vec3 dofColor = color;\n"
    "  float cocPx = 0.0;\n"
    "  if (uDOFOn > 0.5) {\n"
    "    float coc = abs(z - uFocus) / max(z, 1.0);\n"
    "    cocPx = clamp(coc * (uAperture / 100.0) * 220.0, 0.0, uMaxBlur);\n"
    "    if (cocPx > 0.5) {\n"
    "      vec3 acc = color;\n"
    "      float ws = 1.0;\n"
    "      float ns = max(uDOFSamples, 1.0);\n"
    "      for (int i = 0; i < 32; i++) {\n"
    "        if (float(i) >= ns) break;\n"
    "        float fi = (float(i) + 0.5) / ns;\n"
    "        float ang = float(i) * 2.39996323;\n"
    "        float rad = sqrt(fi) * cocPx;\n"
    "        vec2 suv = vUV + vec2(cos(ang), sin(ang)) * rad / uRes;\n"
    "        vec2 scuv = canvasUV(suv);\n"
    "        if (scuv.x < 0.0 || scuv.x > 1.0 || scuv.y < 0.0 || scuv.y > 1.0) continue;\n"
    "        vec3 sc = texture2D(uColor, suv).rgb;\n"
    "        float sz = linZ(texture2D(uDepth, suv).r);\n"
    "        float scoc = abs(sz - uFocus) / max(sz, 1.0) * (uAperture / 100.0) * 220.0;\n"
    "        float w = (sz < z - 1.0 && scoc < 1.0) ? 0.15 : 1.0;\n"
    "        acc += sc * w; ws += w;\n"
    "      }\n"
    "      dofColor = acc / ws;\n"
    "    }\n"
    "  }\n"
    "  vec3 gi = vec3(0.0);\n"
    "  if (uGIOn > 0.5) {\n"
    "    float ws = 0.0;\n"
    "    float nr = max(uGIRays, 1.0);\n"
    "    float nst = max(uGISteps, 1.0);\n"
    "    for (int r = 0; r < 8; r++) {\n"
    "      if (float(r) >= nr) break;\n"
    "      float ang = (float(r) + 0.5) / nr * 6.2831853 + uFrame * 0.07;\n"
    "      vec2 dir = vec2(cos(ang), sin(ang));\n"
    "      for (int s = 1; s <= 12; s++) {\n"
    "        if (float(s) > nst) break;\n"
    "        float t = float(s) / nst;\n"
    "        vec2 ndcOff = dir * (t * uGIRadius) / (z * uTanHalf);\n"
    "        ndcOff.x /= uAspect;\n"
    "        vec2 suv = vUV + ndcOff * 0.5 * uCanvas.zw / uRes;\n"
    "        vec2 scuv = canvasUV(suv);\n"
    "        if (scuv.x < 0.0 || scuv.x > 1.0 || scuv.y < 0.0 || scuv.y > 1.0) continue;\n"
    "        float sz = linZ(texture2D(uDepth, suv).r);\n"
    "        float diff = sz - z;\n"
    "        if (diff > -uGIRadius * 0.5 && diff < uGIRadius * 2.0) {\n"
    "          vec3 sc = texture2D(uColor, suv).rgb;\n"
    "          float dw = exp(-abs(diff) / (uGIRadius + 1.0));\n"
    "          float tw = 1.0 - t;\n"
    "          gi += sc * dw * tw;\n"
    "          ws += dw * tw;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    gi = ws > 0.0 ? gi / ws : vec3(0.0);\n"
    "  }\n"
    "  if (uShadowDebug > 1.5) gl_FragColor = vec4(vec3(unpackDepth(texture2D(uShadowMap, clamp(cuv, 0.0, 1.0)))), 1.0);\n"
    "  else if (uShadowDebug > 0.5) gl_FragColor = vec4(vec3(shadow), 1.0);\n"
    "  else if (uDebug > 3.5) gl_FragColor = vec4(refl * 3.0, 1.0);\n"
    "  else if (uDebug > 2.5) gl_FragColor = vec4(gi * 3.0 * (uGIIntensity / 100.0), 1.0);\n"
    "  else if (uDebug > 1.5) gl_FragColor = vec4(vec3(clamp(cocPx / max(uMaxBlur, 1.0), 0.0, 1.0)), 1.0);\n"
    "  else if (uDebug > 0.5) gl_FragColor = vec4(vec3(ao), 1.0);\n"
    "  else {\n"
    "    vec3 outc = dofColor * ao * shadow + gi * (uGIIntensity / 100.0) * 0.6\n"
    "              + refl * mix(0.35, 1.0, ao);\n"
    "    gl_FragColor = vec4(outc, 1.0);\n"
    "  }\n"
    "}\n";

static struct {
    GLint color, depth, res, canvas, nearv, farv, tanhalf, aspect, frame;
    GLint ao_on, ao_intensity, ao_radius, ao_samples, ao_bias, ao_power;
    GLint dof_on, dof_focus, dof_aperture, dof_maxblur, dof_samples;
    GLint gi_on, gi_intensity, gi_radius, gi_steps, gi_rays;
    GLint debug, normal_flip;
    GLint shear, toe, flipy;
    GLint shadow_map, shadow_vp, shadow_caminv, shadow_dir, shadow_texel;
    GLint shadow_on, shadow_strength, shadow_bias, shadow_nbias;
    GLint shadow_softness, shadow_taps, shadow_debug;
    GLint ssrtx_on, ssrtx_intensity, ssrtx_dist, ssrtx_steps;
    GLint ssrtx_rays, ssrtx_thick, ssrtx_rough, ssrtx_fade;
} s_fx_u;

static GLuint s_fx_prog = 0;
static int    s_fx_prog_state = 0;   // 0 = untried, 1 = ok, -1 = failed
static GLuint s_pp_fbo[2] = {0, 0};
static GLuint s_pp_tex[2] = {0, 0};
static int    s_pp_w = 0, s_pp_h = 0;

static int stereo_fx_any(void) {
    return s_fx_ao_on || s_fx_dof_on || s_fx_gi_on || s_fx_debug != 0
        || (gfx_shadow_enabled && s_shadow_map_valid)
        || s_fx_ssrtx_on;
}

// Row-vector matrix (translation in row 3). GLSL matrices are column-major,
// so uploading the row-major array as-is makes the shader see M = m^T, which
// is exactly the column-vector form we need (world = camInv^T * view).
static void upload_mat4_rowvec(GLint loc, const float m[4][4]) {
    glUniformMatrix4fv(loc, 1, GL_FALSE, &m[0][0]);
}

void gfx_opengl_ssfx_set(int which, int value) {
    switch (which) {
        case 1:  s_fx_ao_on = value ? 1 : 0; break;
        case 2:  s_fx_ao_intensity = value < 0 ? 0 : value; break;
        case 3:  s_fx_ao_radius = value < 1 ? 1 : value; break;
        case 4:  s_fx_ao_samples = value < 1 ? 1 : (value > 32 ? 32 : value); break;
        case 5:  s_fx_ao_bias = value < 0 ? 0 : value; break;
        case 6:  s_fx_ao_power = value < 1 ? 1 : value; break;
        case 11: s_fx_dof_on = value ? 1 : 0; break;
        case 12: s_fx_dof_focus = value < 1 ? 1 : value; break;
        case 13: s_fx_dof_aperture = value < 0 ? 0 : value; break;
        case 14: s_fx_dof_maxblur = value < 0 ? 0 : (value > 48 ? 48 : value); break;
        case 15: s_fx_dof_samples = value < 1 ? 1 : (value > 32 ? 32 : value); break;
        case 16: s_fx_dof_auto = value ? 1 : 0; break;
        case 21: s_fx_gi_on = value ? 1 : 0; break;
        case 22: s_fx_gi_intensity = value < 0 ? 0 : value; break;
        case 23: s_fx_gi_radius = value < 1 ? 1 : value; break;
        case 24: s_fx_gi_steps = value < 1 ? 1 : (value > 12 ? 12 : value); break;
        case 25: s_fx_gi_rays = value < 1 ? 1 : (value > 8 ? 8 : value); break;
        case 31: s_fx_debug = value < 0 ? 0 : (value > 4 ? 4 : value); break;
        case 32: s_fx_normal_flip = value ? 1 : 0; break;
        /* SSRTX screen-space ray-traced reflections. */
        case 70: s_fx_ssrtx_on = value ? 1 : 0; break;
        case 71: s_fx_ssrtx_intensity = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 72: s_fx_ssrtx_distance = value < 50 ? 50 : (value > 2000 ? 2000 : value); break;
        case 73: s_fx_ssrtx_steps = value < 2 ? 2 : (value > 48 ? 48 : value); break;
        case 74: s_fx_ssrtx_rays = value < 1 ? 1 : (value > 8 ? 8 : value); break;
        case 75: s_fx_ssrtx_thickness = value < 5 ? 5 : (value > 300 ? 300 : value); break;
        case 76: s_fx_ssrtx_roughness = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 77: s_fx_ssrtx_fade = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        /* Realtime dynamic shadow map (SHADOW tab). */
        case 50: gfx_shadow_enabled = value ? 1 : 0; break;
        case 51: gfx_shadow_quality = value <= 512 ? 512 : (value <= 1024 ? 1024 : (value <= 2048 ? 2048 : 4096)); break;
        case 52: gfx_shadow_interval = value < 1 ? 1 : (value > 4 ? 4 : value); break;
        case 53: gfx_shadow_strength = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 54: gfx_shadow_bias = value < 0 ? 0 : (value > 200 ? 200 : value); break;
        case 55: gfx_shadow_normal_bias = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 56: gfx_shadow_softness = value < 0 ? 0 : (value > 8 ? 8 : value); break;
        case 57: gfx_shadow_taps = value <= 1 ? 1 : (value <= 4 ? 4 : (value <= 9 ? 9 : 16)); break;
        case 58: gfx_shadow_range = value < 1500 ? 1500 : (value > 9000 ? 9000 : value); break;
        case 59: gfx_shadow_yaw_deg = value < 0 ? 0 : (value > 359 ? 359 : value); break;
        case 60: gfx_shadow_pitch_deg = value < 5 ? 5 : (value > 89 ? 89 : value); break;
        case 61: gfx_shadow_debug = value < 0 ? 0 : (value > 2 ? 2 : value); break;
        default: return;
    }
    __android_log_print(ANDROID_LOG_INFO, "SM64FX",
            "set %d=%d -> ao=%d/%d dof=%d/%d gi=%d/%d dbg=%d",
            which, value, s_fx_ao_on, s_fx_ao_intensity,
            s_fx_dof_on, s_fx_dof_focus, s_fx_gi_on, s_fx_gi_intensity,
            s_fx_debug);
}

int gfx_opengl_ssfx_get(int which) {
    switch (which) {
        case 1:  return s_fx_ao_on;
        case 2:  return s_fx_ao_intensity;
        case 3:  return s_fx_ao_radius;
        case 4:  return s_fx_ao_samples;
        case 5:  return s_fx_ao_bias;
        case 6:  return s_fx_ao_power;
        case 11: return s_fx_dof_on;
        case 12: return s_fx_dof_focus;
        case 16: return s_fx_dof_auto;
        case 17: return (int) (gfx_dof_focus_auto + 0.5f);   // auto focus distance (debug)
        case 13: return s_fx_dof_aperture;
        case 14: return s_fx_dof_maxblur;
        case 15: return s_fx_dof_samples;
        case 21: return s_fx_gi_on;
        case 22: return s_fx_gi_intensity;
        case 23: return s_fx_gi_radius;
        case 24: return s_fx_gi_steps;
        case 25: return s_fx_gi_rays;
        case 31: return s_fx_debug;
        case 32: return s_fx_normal_flip;
        case 70: return s_fx_ssrtx_on;
        case 71: return s_fx_ssrtx_intensity;
        case 72: return s_fx_ssrtx_distance;
        case 73: return s_fx_ssrtx_steps;
        case 74: return s_fx_ssrtx_rays;
        case 75: return s_fx_ssrtx_thickness;
        case 76: return s_fx_ssrtx_roughness;
        case 77: return s_fx_ssrtx_fade;
        case 50: return gfx_shadow_enabled;
        case 51: return gfx_shadow_quality;
        case 52: return gfx_shadow_interval;
        case 53: return gfx_shadow_strength;
        case 54: return gfx_shadow_bias;
        case 55: return gfx_shadow_normal_bias;
        case 56: return gfx_shadow_softness;
        case 57: return gfx_shadow_taps;
        case 58: return gfx_shadow_range;
        case 59: return gfx_shadow_yaw_deg;
        case 60: return gfx_shadow_pitch_deg;
        case 61: return gfx_shadow_debug;
    }
    return -1;
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    return sh;
}

static GLuint compile_shader_checked(GLenum type, const char *src, const char *label) {
    GLuint sh = compile_shader(type, src);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        char logbuf[2048];
        if (len > (GLint)sizeof(logbuf)) len = (GLint)sizeof(logbuf);
        glGetShaderInfoLog(sh, len, NULL, logbuf);
        __android_log_print(ANDROID_LOG_ERROR, "SM64FX",
                "FX %s shader failed: %s", label, logbuf);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void stereo_fx_init(void) {
    if (s_fx_prog_state != 0) return;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_fx_vs, "vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_fx_fs, "fragment");
    if (!vs || !fs) {
        s_fx_prog_state = -1;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }
    s_fx_prog = glCreateProgram();
    glAttachShader(s_fx_prog, vs);
    glAttachShader(s_fx_prog, fs);
    glLinkProgram(s_fx_prog);
    GLint ok = 0;
    glGetProgramiv(s_fx_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        __android_log_print(ANDROID_LOG_ERROR, "SM64FX", "FX program link failed");
        glDeleteProgram(s_fx_prog);
        s_fx_prog = 0;
        s_fx_prog_state = -1;
        return;
    }
    s_fx_u.color = glGetUniformLocation(s_fx_prog, "uColor");
    s_fx_u.depth = glGetUniformLocation(s_fx_prog, "uDepth");
    s_fx_u.res = glGetUniformLocation(s_fx_prog, "uRes");
    s_fx_u.canvas = glGetUniformLocation(s_fx_prog, "uCanvas");
    s_fx_u.nearv = glGetUniformLocation(s_fx_prog, "uNear");
    s_fx_u.farv = glGetUniformLocation(s_fx_prog, "uFar");
    s_fx_u.tanhalf = glGetUniformLocation(s_fx_prog, "uTanHalf");
    s_fx_u.aspect = glGetUniformLocation(s_fx_prog, "uAspect");
    s_fx_u.frame = glGetUniformLocation(s_fx_prog, "uFrame");
    s_fx_u.ao_on = glGetUniformLocation(s_fx_prog, "uAOOn");
    s_fx_u.ao_intensity = glGetUniformLocation(s_fx_prog, "uAOIntensity");
    s_fx_u.ao_radius = glGetUniformLocation(s_fx_prog, "uAORadius");
    s_fx_u.ao_samples = glGetUniformLocation(s_fx_prog, "uAOSamples");
    s_fx_u.ao_bias = glGetUniformLocation(s_fx_prog, "uAOBias");
    s_fx_u.ao_power = glGetUniformLocation(s_fx_prog, "uAOPower");
    s_fx_u.dof_on = glGetUniformLocation(s_fx_prog, "uDOFOn");
    s_fx_u.dof_focus = glGetUniformLocation(s_fx_prog, "uFocus");
    s_fx_u.dof_aperture = glGetUniformLocation(s_fx_prog, "uAperture");
    s_fx_u.dof_maxblur = glGetUniformLocation(s_fx_prog, "uMaxBlur");
    s_fx_u.dof_samples = glGetUniformLocation(s_fx_prog, "uDOFSamples");
    s_fx_u.gi_on = glGetUniformLocation(s_fx_prog, "uGIOn");
    s_fx_u.gi_intensity = glGetUniformLocation(s_fx_prog, "uGIIntensity");
    s_fx_u.gi_radius = glGetUniformLocation(s_fx_prog, "uGIRadius");
    s_fx_u.gi_steps = glGetUniformLocation(s_fx_prog, "uGISteps");
    s_fx_u.gi_rays = glGetUniformLocation(s_fx_prog, "uGIRays");
    s_fx_u.debug = glGetUniformLocation(s_fx_prog, "uDebug");
    s_fx_u.normal_flip = glGetUniformLocation(s_fx_prog, "uNormalFlip");
    s_fx_u.shear = glGetUniformLocation(s_fx_prog, "uShear");
    s_fx_u.toe = glGetUniformLocation(s_fx_prog, "uToe");
    s_fx_u.flipy = glGetUniformLocation(s_fx_prog, "uFlipY");
    s_fx_u.shadow_map = glGetUniformLocation(s_fx_prog, "uShadowMap");
    s_fx_u.shadow_vp = glGetUniformLocation(s_fx_prog, "uShadowVP");
    s_fx_u.shadow_caminv = glGetUniformLocation(s_fx_prog, "uCamInv");
    s_fx_u.shadow_dir = glGetUniformLocation(s_fx_prog, "uShadowLightDir");
    s_fx_u.shadow_texel = glGetUniformLocation(s_fx_prog, "uShadowTexel");
    s_fx_u.shadow_on = glGetUniformLocation(s_fx_prog, "uShadowOn");
    s_fx_u.shadow_strength = glGetUniformLocation(s_fx_prog, "uShadowStrength");
    s_fx_u.shadow_bias = glGetUniformLocation(s_fx_prog, "uShadowBias");
    s_fx_u.shadow_nbias = glGetUniformLocation(s_fx_prog, "uShadowNormalBias");
    s_fx_u.shadow_softness = glGetUniformLocation(s_fx_prog, "uShadowSoftness");
    s_fx_u.shadow_taps = glGetUniformLocation(s_fx_prog, "uShadowTaps");
    s_fx_u.shadow_debug = glGetUniformLocation(s_fx_prog, "uShadowDebug");
    s_fx_u.ssrtx_on = glGetUniformLocation(s_fx_prog, "uSSRTXOn");
    s_fx_u.ssrtx_intensity = glGetUniformLocation(s_fx_prog, "uSSRTXIntensity");
    s_fx_u.ssrtx_dist = glGetUniformLocation(s_fx_prog, "uSSRTXDist");
    s_fx_u.ssrtx_steps = glGetUniformLocation(s_fx_prog, "uSSRTXSteps");
    s_fx_u.ssrtx_rays = glGetUniformLocation(s_fx_prog, "uSSRTXRays");
    s_fx_u.ssrtx_thick = glGetUniformLocation(s_fx_prog, "uSSRTXThick");
    s_fx_u.ssrtx_rough = glGetUniformLocation(s_fx_prog, "uSSRTXRough");
    s_fx_u.ssrtx_fade = glGetUniformLocation(s_fx_prog, "uSSRTXFade");
    s_fx_prog_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX", "FX program ready (%u)", s_fx_prog);
}

/* ---------------- Realtime shadow map pass ----------------
 * Renders the scene range from a directional sun into a small depth map.
 * gfx_pc.c replays the display list in "shadow pass" mode, where vertices are
 * transformed straight to light clip space and only positions are uploaded.
 */
static const char *s_shadow_vs =
    "attribute vec4 aVtxPos;\n"
    "void main(){ gl_Position = aVtxPos; }\n";

static const char *s_shadow_fs =
#ifdef USE_GLES
    "precision highp float;\n"
#endif
    "vec4 packDepth(float d) {\n"
    "  vec4 enc = vec4(1.0, 255.0, 65025.0, 16581375.0) * d;\n"
    "  enc = fract(enc);\n"
    "  enc -= enc.yzww * vec4(1.0/255.0, 1.0/255.0, 1.0/255.0, 0.0);\n"
    "  return enc;\n"
    "}\n"
    "void main(){ gl_FragColor = packDepth(gl_FragCoord.z); }\n";

static int shadow_ensure_program(void) {
    if (s_shadow_prog_state != 0) return s_shadow_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_shadow_vs, "shadow vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_shadow_fs, "shadow fragment");
    if (!vs || !fs) {
        s_shadow_prog_state = -1;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    s_shadow_prog = glCreateProgram();
    glAttachShader(s_shadow_prog, vs);
    glAttachShader(s_shadow_prog, fs);
    glLinkProgram(s_shadow_prog);
    GLint ok = 0;
    glGetProgramiv(s_shadow_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        __android_log_print(ANDROID_LOG_ERROR, "SM64SHADOW", "shadow program link failed");
        glDeleteProgram(s_shadow_prog);
        s_shadow_prog = 0;
        s_shadow_prog_state = -1;
        return 0;
    }
    s_shadow_apos = glGetAttribLocation(s_shadow_prog, "aVtxPos");
    s_shadow_prog_state = 1;
    return 1;
}

static int shadow_ensure_fbo(int res) {
    if (res < 256) res = 256;
    if (res > 4096) res = 4096;
    if (res == s_shadow_res && s_shadow_fbo != 0) return 1;

    if (s_shadow_fbo) glDeleteFramebuffers(1, &s_shadow_fbo);
    if (s_shadow_tex) glDeleteTextures(1, &s_shadow_tex);
    if (s_shadow_depth) glDeleteRenderbuffers(1, &s_shadow_depth);
    s_shadow_fbo = s_shadow_tex = s_shadow_depth = 0;

    glGenTextures(1, &s_shadow_tex);
    glBindTexture(GL_TEXTURE_2D, s_shadow_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, res, res, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &s_shadow_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_shadow_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, res, res);

    glGenFramebuffers(1, &s_shadow_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_shadow_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_shadow_depth);

    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    __android_log_print(ANDROID_LOG_INFO, "SM64SHADOW", "map %dx%d fbo=%u tex=%u status=0x%x",
                        res, res, s_shadow_fbo, s_shadow_tex, st);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_shadow_res = res;
    s_shadow_map_valid = 0;
    return st == GL_FRAMEBUFFER_COMPLETE;
}

int gfx_opengl_shadow_begin(int resolution) {
    if (!shadow_ensure_program()) return 0;
    if (!shadow_ensure_fbo(resolution)) return 0;

    // Do not sample the map while rendering into it (it stays bound on unit 2
    // from the previous frame's post pass).
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    glBindFramebuffer(GL_FRAMEBUFFER, s_shadow_fbo);
    glViewport(0, 0, s_shadow_res, s_shadow_res);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    // Cleared colour unpacks to depth 1.0 (nothing between the light and here).
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(s_shadow_prog);
    if (!opengl_vbo) glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    if (s_shadow_apos >= 0) {
        glEnableVertexAttribArray(s_shadow_apos);
        glVertexAttribPointer(s_shadow_apos, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    }
    s_shadow_rendering = 1;
    return 1;
}

void gfx_opengl_shadow_end(void) {
    if (s_shadow_apos >= 0) glDisableVertexAttribArray(s_shadow_apos);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_shadow_rendering = 0;
    s_shadow_map_valid = 1;
}

static void stereo_ensure_pp(int w, int h) {
    if (w == s_pp_w && h == s_pp_h && s_pp_fbo[0] != 0) return;
    for (int i = 0; i < 2; i++) {
        if (s_pp_fbo[i]) glDeleteFramebuffers(1, &s_pp_fbo[i]);
        if (s_pp_tex[i]) glDeleteTextures(1, &s_pp_tex[i]);
        glGenTextures(1, &s_pp_tex[i]);
        glBindTexture(GL_TEXTURE_2D, s_pp_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s_pp_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_pp_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_pp_tex[i], 0);
        __android_log_print(ANDROID_LOG_INFO, "SM64FX",
                "PP fbo %d=%u tex=%u %dx%d status=0x%x", i, s_pp_fbo[i],
                s_pp_tex[i], w, h, glCheckFramebufferStatus(GL_FRAMEBUFFER));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_pp_w = w;
    s_pp_h = h;
}

/* Runs the post pass for both eyes. Returns 1 if s_pp_tex is valid to composite. */
static int stereo_fx_pass(void) {
    if (!stereo_fx_any()) return 0;
    if (!s_depth_tex_mode) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            __android_log_print(ANDROID_LOG_WARN, "SM64FX",
                    "depth texture unavailable; screen-space FX disabled");
        }
        return 0;
    }
    stereo_fx_init();
    if (s_fx_prog_state != 1) return 0;
    stereo_ensure_pp(s_eye_w, s_eye_h);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_fx_prog);

    glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_fx_u.color, 0);
    glActiveTexture(GL_TEXTURE1);
    glUniform1i(s_fx_u.depth, 1);

    int shadow_on = gfx_shadow_enabled && s_shadow_map_valid && s_shadow_tex != 0
                    && gfx_shadow_matrices_ready;
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, shadow_on ? s_shadow_tex : 0);
    glUniform1i(s_fx_u.shadow_map, 2);
    glUniform1f(s_fx_u.shadow_on, shadow_on ? 1.0f : 0.0f);
    glUniform1f(s_fx_u.shadow_strength, (float)gfx_shadow_strength / 100.0f);
    glUniform1f(s_fx_u.shadow_bias, (float)gfx_shadow_bias);
    glUniform1f(s_fx_u.shadow_nbias, (float)gfx_shadow_normal_bias);
    glUniform1f(s_fx_u.shadow_softness, (float)gfx_shadow_softness);
    glUniform1f(s_fx_u.shadow_taps, (float)gfx_shadow_taps);
    glUniform1f(s_fx_u.shadow_debug, (float)gfx_shadow_debug);
    glUniform2f(s_fx_u.shadow_texel,
                s_shadow_res > 0 ? 1.0f / (float)s_shadow_res : 0.0f,
                s_shadow_res > 0 ? 1.0f / (float)s_shadow_res : 0.0f);
    glUniform3f(s_fx_u.shadow_dir, gfx_shadow_light_dir[0], gfx_shadow_light_dir[1],
                gfx_shadow_light_dir[2]);
    upload_mat4_rowvec(s_fx_u.shadow_vp, gfx_shadow_light_vp);
    upload_mat4_rowvec(s_fx_u.shadow_caminv, gfx_cam_inv_mtx);
    glUniform1f(s_fx_u.ssrtx_on, s_fx_ssrtx_on ? 1.0f : 0.0f);
    glUniform1f(s_fx_u.ssrtx_intensity, (float)s_fx_ssrtx_intensity);
    glUniform1f(s_fx_u.ssrtx_dist, (float)s_fx_ssrtx_distance);
    glUniform1f(s_fx_u.ssrtx_steps, (float)s_fx_ssrtx_steps);
    glUniform1f(s_fx_u.ssrtx_rays, (float)s_fx_ssrtx_rays);
    glUniform1f(s_fx_u.ssrtx_thick, (float)s_fx_ssrtx_thickness);
    glUniform1f(s_fx_u.ssrtx_rough, (float)s_fx_ssrtx_roughness / 100.0f);
    glUniform1f(s_fx_u.ssrtx_fade, (float)s_fx_ssrtx_fade);

    glUniform2f(s_fx_u.res, (float)s_eye_w, (float)s_eye_h);
    glUniform4f(s_fx_u.canvas, (float)s_canvas_x, (float)s_canvas_y,
                (float)s_canvas_w, (float)s_canvas_h);
    glUniform1f(s_fx_u.nearv, gfx_cam_near);
    glUniform1f(s_fx_u.farv, gfx_cam_far);
    glUniform1f(s_fx_u.tanhalf, gfx_cam_tan_half_fov_y);
    glUniform1f(s_fx_u.aspect, (float)s_canvas_w / (float)(s_canvas_h ? s_canvas_h : 1));
    glUniform1f(s_fx_u.frame, (float)(frame_count & 1023));
    glUniform1f(s_fx_u.ao_on, s_fx_ao_on ? 1.0f : 0.0f);
    glUniform1f(s_fx_u.ao_intensity, (float)s_fx_ao_intensity);
    glUniform1f(s_fx_u.ao_radius, (float)s_fx_ao_radius);
    glUniform1f(s_fx_u.ao_samples, (float)s_fx_ao_samples);
    glUniform1f(s_fx_u.ao_bias, (float)s_fx_ao_bias);
    glUniform1f(s_fx_u.ao_power, (float)s_fx_ao_power);
    glUniform1f(s_fx_u.dof_on, s_fx_dof_on ? 1.0f : 0.0f);
    float dof_focus = s_fx_dof_auto ? gfx_dof_focus_auto : (float)s_fx_dof_focus;
    if (dof_focus < 1.0f) dof_focus = 1.0f;
    glUniform1f(s_fx_u.dof_focus, dof_focus);
    glUniform1f(s_fx_u.dof_aperture, (float)s_fx_dof_aperture);
    glUniform1f(s_fx_u.dof_maxblur, (float)s_fx_dof_maxblur);
    glUniform1f(s_fx_u.dof_samples, (float)s_fx_dof_samples);
    glUniform1f(s_fx_u.gi_on, s_fx_gi_on ? 1.0f : 0.0f);
    glUniform1f(s_fx_u.gi_intensity, (float)s_fx_gi_intensity);
    glUniform1f(s_fx_u.gi_radius, (float)s_fx_gi_radius);
    glUniform1f(s_fx_u.gi_steps, (float)s_fx_gi_steps);
    glUniform1f(s_fx_u.gi_rays, (float)s_fx_gi_rays);
    glUniform1f(s_fx_u.debug, (float)s_fx_debug);
    glUniform1f(s_fx_u.normal_flip, (float)s_fx_normal_flip);

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);

    GLint apos = glGetAttribLocation(s_fx_prog, "aPos");
    GLint auv = glGetAttribLocation(s_fx_prog, "aUV");
    glEnableVertexAttribArray(apos);
    glVertexAttribPointer(apos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(auv);
    glVertexAttribPointer(auv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) {
            continue;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, s_pp_fbo[eye]);
        glViewport(0, 0, s_eye_w, s_eye_h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_eye_tex[eye]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_eye_depth[eye]);
        glUniform1f(s_fx_u.shear, gfx_eye_shear[eye]);
        glUniform1f(s_fx_u.toe, gfx_eye_toe[eye]);
        glUniform1f(s_fx_u.flipy, gfx_eye_flip[eye] ? 1.0f : 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glDisableVertexAttribArray(apos);
    glDisableVertexAttribArray(auv);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    return 1;
}

static void stereo_ensure_resources(int w, int h) {
    if (w == s_eye_w && h == s_eye_h && s_eye_fbo[0] != 0) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (s_eye_fbo[i]) glDeleteFramebuffers(1, &s_eye_fbo[i]);
        if (s_eye_tex[i]) glDeleteTextures(1, &s_eye_tex[i]);
        if (s_eye_depth[i]) {
            if (s_depth_tex_mode) glDeleteTextures(1, &s_eye_depth[i]);
            else glDeleteRenderbuffers(1, &s_eye_depth[i]);
        }

        glGenTextures(1, &s_eye_tex[i]);
        glBindTexture(GL_TEXTURE_2D, s_eye_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // (Re)generate the eye framebuffer: this call was accidentally dropped
        // during the ZBUF edit, leaving s_eye_fbo == 0 forever.
        glGenFramebuffers(1, &s_eye_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_eye_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_eye_tex[i], 0);

        if (s_depth_tex_mode) {
            // Depth as a texture so the composite pass can visualise it.
            glGenTextures(1, &s_eye_depth[i]);
            glBindTexture(GL_TEXTURE_2D, s_eye_depth[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, w, h, 0,
                         GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                   GL_TEXTURE_2D, s_eye_depth[i], 0);
        } else {
            glGenRenderbuffers(1, &s_eye_depth[i]);
            glBindRenderbuffer(GL_RENDERBUFFER, s_eye_depth[i]);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER, s_eye_depth[i]);
        }
        __android_log_print(ANDROID_LOG_INFO, "SM64FBO", "eye %d fbo=%u tex=%u %dx%d status=0x%x",
                            i, s_eye_fbo[i], s_eye_tex[i], w, h,
                            glCheckFramebufferStatus(GL_FRAMEBUFFER));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_eye_w = w;
    s_eye_h = h;

    if (!s_comp_prog) {
        GLuint vs = compile_shader(GL_VERTEX_SHADER, s_comp_vs);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, s_comp_fs);
        s_comp_prog = glCreateProgram();
        glAttachShader(s_comp_prog, vs);
        glAttachShader(s_comp_prog, fs);
        glLinkProgram(s_comp_prog);
        GLint linkOk = 0;
        glGetProgramiv(s_comp_prog, GL_LINK_STATUS, &linkOk);
        __android_log_print(ANDROID_LOG_INFO, "SM64FBO", "COMPOSITE_LINK ok=%d", linkOk);
        s_comp_pos = glGetAttribLocation(s_comp_prog, "aPos");
        s_comp_uv  = glGetAttribLocation(s_comp_prog, "aUV");
        s_comp_tex = glGetUniformLocation(s_comp_prog, "uTex");
        s_comp_mode = glGetUniformLocation(s_comp_prog, "uMode");
        s_comp_scale = glGetUniformLocation(s_comp_prog, "uScale");
        s_comp_solid = glGetUniformLocation(s_comp_prog, "uSolid");
        s_comp_ui = glGetUniformLocation(s_comp_prog, "uUI");
        s_comp_hasui = glGetUniformLocation(s_comp_prog, "uHasUI");
        s_comp_uibuf = glGetUniformLocation(s_comp_prog, "uUIBuf");
        glDeleteShader(vs);
        glDeleteShader(fs);
        __android_log_print(ANDROID_LOG_INFO, "SM64FBO", "composite prog=%u pos=%d uv=%d",
                            s_comp_prog, s_comp_pos, s_comp_uv);
    }
}

/* ---------------- UI after post: per-eye UI buffer ----------------
 * The 2D UI range of the frame (HUD, dialogs, menus, transitions, borders) is
 * rendered here instead of into the eye buffer, so the FX pass never sees it.
 * RGBA so the composite can use alpha as premultiplied coverage.
 */
static GLuint s_ui_fbo[2] = {0, 0};
static GLuint s_ui_tex[2] = {0, 0};
static GLuint s_ui_depth[2] = {0, 0};
static int    s_ui_w = 0, s_ui_h = 0;

static void stereo_ensure_ui_resources(int w, int h) {
    if (w == s_ui_w && h == s_ui_h && s_ui_fbo[0] != 0) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (s_ui_fbo[i]) glDeleteFramebuffers(1, &s_ui_fbo[i]);
        if (s_ui_tex[i]) glDeleteTextures(1, &s_ui_tex[i]);
        if (s_ui_depth[i]) glDeleteRenderbuffers(1, &s_ui_depth[i]);

        glGenTextures(1, &s_ui_tex[i]);
        glBindTexture(GL_TEXTURE_2D, s_ui_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &s_ui_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_ui_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_ui_tex[i], 0);

        // Own depth buffer: the scene depth texture is read by the FX pass.
        glGenRenderbuffers(1, &s_ui_depth[i]);
        glBindRenderbuffer(GL_RENDERBUFFER, s_ui_depth[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, s_ui_depth[i]);

        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        __android_log_print(ANDROID_LOG_INFO, "SM64FBO", "ui %d fbo=%u tex=%u %dx%d status=0x%x",
                            i, s_ui_fbo[i], s_ui_tex[i], w, h, st);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_ui_w = w;
    s_ui_h = h;
}

/* Called by gfx_pc.c after the scene range, before the UI range. */
void gfx_opengl_ui_begin(int eye, int w, int h, int cx, int cy, int cw, int ch) {
    stereo_ensure_ui_resources(w, h);
    s_fbo_mode = 1;
    glBindFramebuffer(GL_FRAMEBUFFER, s_ui_fbo[eye & 1]);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, w, h);
    glViewport(cx, cy, cw, ch);
    // Keep the colour premultiplied by coverage, and accumulate coverage in
    // alpha: the buffer starts at (0,0,0,0) and is composited as
    // ui.rgb + scene.rgb * (1 - ui.a).
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void gfx_opengl_ui_end(void) {
    // Restore the blend function the game pipeline assumes.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

/* Called by gfx_pc.c before each eye's display list. */
void gfx_opengl_stereo_begin(int eye, int w, int h, int cx, int cy, int cw, int ch) {
    if (s_eye_fbo[0] == 0 && !s_depth_tex_checked) {
        s_depth_tex_checked = 1;
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        s_depth_tex_mode = ext && strstr(ext, "GL_OES_depth_texture") != NULL;
        __android_log_print(ANDROID_LOG_INFO, "SM64FBO",
                "GL_VERSION=%s RENDERER=%s OES_depth_texture=%d",
                glGetString(GL_VERSION), glGetString(GL_RENDERER),
                s_depth_tex_mode);
    }
    s_canvas_x = cx;
    s_canvas_y = cy;
    s_canvas_w = cw;
    s_canvas_h = ch;
    stereo_ensure_resources(w, h);
    s_fbo_mode = 1;
    glBindFramebuffer(GL_FRAMEBUFFER, s_eye_fbo[eye & 1]);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, w, h);
    glViewport(cx, cy, cw, ch);
}

/* Blit both eye textures (with the UI composited on top) into the 2x1 SBS
 * default framebuffer. `has_ui` says whether the UI buffer was captured. */
extern int gfx_ui_buf;

void gfx_opengl_stereo_composite(int w, int h, int full_w, int full_h, int has_ui) {
    {
        static long last_ms = 0;
    static int frames = 0;
    long now = (long)(frame_count * (1000.0 / 30.0));
    frames++;
    // wall-clock timing via clock_gettime
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (last_ms == 0) last_ms = ms;
    if (ms - last_ms >= 500) {
        gfx_stereo_fps = frames * 1000.0f / (float)(ms - last_ms);
        last_ms = ms;
        frames = 0;
    }
    }

    // Post-process each eye (MXAO / CinematicDOF / SSGI-lite) before weaving.
    // ZBUF is a raw debug view, so it bypasses the FX chain.
    int fx_ready = gfx_stereo_zbuf ? 0 : stereo_fx_pass();

    s_fbo_mode = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, full_w, full_h);

    glUseProgram(s_comp_prog);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_comp_tex, 0);
    glUniform1i(s_comp_ui, 1);
    glUniform1f(s_comp_hasui, has_ui ? 1.0f : 0.0f);
    glUniform1f(s_comp_uibuf, gfx_ui_buf ? 1.0f : 0.0f);

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) {
        glGenBuffers(1, &s_comp_vbo);
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(s_comp_pos);
    glVertexAttribPointer(s_comp_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(s_comp_uv);
    glVertexAttribPointer(s_comp_uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    glUniform1f(s_comp_mode, 0.0f);
    glUniform1f(s_comp_scale, (float)gfx_stereo_in_scale_pct / 100.0f);
    int zview = gfx_stereo_zbuf && s_depth_tex_mode;
    glUniform1f(s_comp_mode, zview ? 2.0f : 0.0f);
    for (int eye = 0; eye < 2; eye++) {
        int src = gfx_single_eye >= 0 ? gfx_single_eye : eye;
        GLuint scene_tex = zview ? s_eye_depth[src]
                                 : (fx_ready ? s_pp_tex[src] : s_eye_tex[src]);
        glViewport(eye * (full_w / 2), 0, full_w / 2, full_h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (has_ui && s_ui_tex[src]) ? s_ui_tex[src] : scene_tex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glActiveTexture(GL_TEXTURE0);

    if (gfx_stereo_debug_tint) {
        // 50% red over the left half, 50% blue over the right half: shows
        // exactly which pixels come from which eye in the SBS buffer.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform1f(s_comp_mode, 1.0f);
        glViewport(0, 0, full_w / 2, full_h);
        glUniform4f(s_comp_solid, 1.0f, 0.0f, 0.0f, 0.5f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glViewport(full_w / 2, 0, full_w / 2, full_h);
        glUniform4f(s_comp_solid, 0.0f, 0.2f, 1.0f, 0.5f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
        glUniform1f(s_comp_mode, 0.0f);
    }

    glDisableVertexAttribArray(s_comp_pos);
    glDisableVertexAttribArray(s_comp_uv);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}
#endif

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
#ifdef __ANDROID__
    if (s_fbo_mode) {
        glViewport(s_canvas_x + x, s_canvas_y + y, width, height);
        return;
    }
#endif
#ifdef __ANDROID__
    {
        static int vp_log_n = 0;
        if ((vp_log_n++ % 120) == 0) {
            __android_log_print(ANDROID_LOG_INFO, "SM64VP",
                "set_viewport eye=%d x=%d y=%d w=%d h=%d eyeW=%d",
                gfx_stereo_eye, x, y, width, height, gfx_stereo_eye_width);
        }
    }
#endif
    int ox = x;
#ifndef __ANDROID__
    ox = x + (gfx_stereo_enabled ? gfx_stereo_eye * gfx_stereo_eye_width : 0);
#endif
    if (gfx_stereo_enabled && gfx_stereo_eye >= 0 && gfx_stereo_eye < 2) {
        gfx_stereo_vp[gfx_stereo_eye][0] = ox;
        gfx_stereo_vp[gfx_stereo_eye][1] = width;
    }
    glViewport(ox, y, width, height);
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
#ifdef __ANDROID__
    if (s_fbo_mode) {
        glScissor(s_canvas_x + x, s_canvas_y + y, width, height);
        return;
    }
#endif
    glScissor(x + (gfx_stereo_enabled ? gfx_stereo_eye * gfx_stereo_eye_width : 0), y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

static GLuint s_wire_ibo = 0;
static GLsizei s_wire_capacity = 0;

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    //printf("flushing %d tris\n", buf_vbo_num_tris);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    bool wire_now = gfx_stereo_wire;
#ifdef __ANDROID__
    wire_now = wire_now && !s_shadow_rendering;
#endif
    if (gfx_splat_active && opengl_prg != NULL) {
        // Splat pass: one point sprite per triangle, plus interpolated points on
        // large triangles.  Density is measured in screen pixels so walls and
        // floors (two huge triangles each) get an even Poisson-ish fill.
        static float *s_splat_out = NULL;
        static size_t s_splat_out_cap = 0;
        static int *s_splat_extra = NULL;
        static size_t s_splat_extra_cap = 0;
        const size_t nf = opengl_prg->num_floats;
        const size_t nt = buf_vbo_num_tris;
        const size_t max_points = 120000;
        GLint vp[4] = { 0, 0, 1, 1 };
        size_t total = 0, t;

        glGetIntegerv(GL_VIEWPORT, vp);
        {
            float vw = vp[2] > 0 ? (float)vp[2] : 1.0f;
            float vh = vp[3] > 0 ? (float)vp[3] : 1.0f;
            float spacing = (float)gfx_splat_size_px * 1.6f;
            float inv_spacing2 = 1.0f / (spacing * spacing + 1.0f);
            float density = (float)gfx_splat_density / 100.0f;

            if (s_splat_extra_cap < nt) {
                int *p = (int *)realloc(s_splat_extra, nt * sizeof(int));
                if (p == NULL) return;
                s_splat_extra = p;
                s_splat_extra_cap = nt;
            }

            for (t = 0; t < nt; t++) {
                const float *v = buf_vbo + t * 3 * nf;
                int extra = 0;

                if (density > 0.0f && nf >= 4 && v[3] > 0.0001f
                    && v[nf + 3] > 0.0001f && v[2 * nf + 3] > 0.0001f) {
                    float ax = v[0] / v[3], ay = v[1] / v[3];
                    float bx = v[nf] / v[nf + 3], by = v[nf + 1] / v[nf + 3];
                    float cx = v[2 * nf] / v[2 * nf + 3], cy = v[2 * nf + 1] / v[2 * nf + 3];
                    float area = 0.5f * fabsf((bx - ax) * (cy - ay) - (by - ay) * (cx - ax));
                    float pixels = area * vw * vh;
                    extra = (int)(pixels * density * inv_spacing2);
                    if (extra > 400) extra = 400;
                }
                if (total + 1 + (size_t)extra > max_points) {
                    extra = (total + 1 >= max_points) ? 0
                          : (int)(max_points - total - 1);
                }
                s_splat_extra[t] = extra;
                total += 1 + (size_t)extra;
                if (total >= max_points) break;
            }
            for (; t < nt; t++) s_splat_extra[t] = 0;
            total = 0;
            for (t = 0; t < nt; t++) total += 1 + (size_t)s_splat_extra[t];
        }

        if (total > 0 && nf > 0) {
            if (s_splat_out_cap < total * nf) {
                float *p = (float *)realloc(s_splat_out, total * nf * sizeof(float));
                if (p == NULL) return;
                s_splat_out = p;
                s_splat_out_cap = total * nf;
            }

            size_t o = 0;
            for (t = 0; t < nt; t++) {
                const float *v = buf_vbo + t * 3 * nf;
                int extra = s_splat_extra[t];
                int pick = (int)(t % 3);
                size_t f;
                float jitter = (float)((t * 2654435761u) & 0xFFFF) / 65536.0f;

                memcpy(s_splat_out + o * nf, v + pick * nf, nf * sizeof(float));
                o++;

                for (int i = 0; i < extra; i++) {
                    // Low-discrepancy (golden-ratio) samples mapped through a
                    // unit-triangle transform: even coverage, no regular grid.
                    float u = fmodf((i + 0.5f) * 0.7548776662f + jitter, 1.0f);
                    float w = fmodf((i + 0.5f) * 0.5698402910f + jitter * 0.37f, 1.0f);
                    float r = sqrtf(u);
                    float b0 = 1.0f - r;
                    float b1 = r * (1.0f - w);
                    float b2 = r * w;
                    for (f = 0; f < nf; f++) {
                        s_splat_out[o * nf + f] =
                            v[f] * b0 + v[nf + f] * b1 + v[2 * nf + f] * b2;
                    }
                    o++;
                }
            }

            glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
            glBufferData(GL_ARRAY_BUFFER, total * nf * sizeof(float),
                         s_splat_out, GL_STREAM_DRAW);
            if (opengl_prg->splat_size_loc >= 0)
                glUniform1f(opengl_prg->splat_size_loc, (float)gfx_splat_size_px);
            if (opengl_prg->splat_on_loc >= 0)
                glUniform1f(opengl_prg->splat_on_loc, 1.0f);
            if (opengl_prg->splat_opacity_loc >= 0)
                glUniform1f(opengl_prg->splat_opacity_loc, gfx_splat_opacity / 100.0f);
            if (opengl_prg->splat_softness_loc >= 0)
                glUniform1f(opengl_prg->splat_softness_loc, gfx_splat_softness / 100.0f);
            if (opengl_prg->splat_shape_loc >= 0)
                glUniform1f(opengl_prg->splat_shape_loc, gfx_splat_shape ? 1.0f : 0.0f);

#ifndef USE_GLES
            glEnable(GL_POINT_SPRITE);
            glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
#endif
            /* The tagged list's own render-mode command disables GL_BLEND
             * before the points are flushed; force it back on so opacity and
             * the softness alpha falloff actually apply. */
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthFunc(GL_LEQUAL);
            glDrawArrays(GL_POINTS, 0, (GLsizei)total);
#ifndef USE_GLES
            glDisable(GL_POINT_SPRITE);
            glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
#endif
        }
        return;
    }
    if (wire_now) {
        // ES2 has no glPolygonMode: expand each triangle into its 3 edges.
        if (s_wire_capacity < (GLsizei)buf_vbo_num_tris * 6) {
            if (s_wire_ibo) glDeleteBuffers(1, &s_wire_ibo);
            glGenBuffers(1, &s_wire_ibo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_wire_ibo);
            s_wire_capacity = (GLsizei)buf_vbo_num_tris * 6;
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         s_wire_capacity * sizeof(GLushort), NULL, GL_STREAM_DRAW);
        }
        static GLushort *cpu_idx = NULL;
        static size_t cpu_cap = 0;
        size_t need = (size_t)buf_vbo_num_tris * 6;
        if (need > cpu_cap) {
            free(cpu_idx);
            cpu_idx = (GLushort *)malloc(need * sizeof(GLushort));
            cpu_cap = cpu_idx ? need : 0;
        }
        if (cpu_idx != NULL) {
            for (size_t t = 0; t < buf_vbo_num_tris; t++) {
                GLushort b = (GLushort)(3 * t);
                cpu_idx[6 * t + 0] = b + 0;
                cpu_idx[6 * t + 1] = b + 1;
                cpu_idx[6 * t + 2] = b + 1;
                cpu_idx[6 * t + 3] = b + 2;
                cpu_idx[6 * t + 4] = b + 2;
                cpu_idx[6 * t + 5] = b + 0;
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_wire_ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, need * sizeof(GLushort),
                         cpu_idx, GL_STREAM_DRAW);
            glDrawElements(GL_LINES, (GLsizei)need, GL_UNSIGNED_SHORT, NULL);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
    }
}

static inline bool gl_get_version(int *major, int *minor, bool *is_es) {
    const char *vstr = (const char *)glGetString(GL_VERSION);
    if (!vstr || !vstr[0]) return false;

    if (!strncmp(vstr, "OpenGL ES ", 10)) {
        vstr += 10;
        *is_es = true;
    } else if (!strncmp(vstr, "OpenGL ES-CM ", 13)) {
        vstr += 13;
        *is_es = true;
    }

    return (sscanf(vstr, "%d.%d", major, minor) == 2);
}

static void gfx_opengl_init(void) {
#if FOR_WINDOWS || defined(OSX_BUILD)
    GLenum err;
    if ((err = glewInit()) != GLEW_OK)
        sys_fatal("could not init GLEW:\n%s", glewGetErrorString(err));
#endif

    tex_cache_size = TEX_CACHE_STEP;
    tex_cache = calloc(tex_cache_size, sizeof(struct GLTexture));
    if (!tex_cache) sys_fatal("out of memory allocating texture cache");

    // check GL version
    int vmajor, vminor;
    bool is_es = false;
    gl_get_version(&vmajor, &vminor, &is_es);
    if (vmajor < 2 && vminor < 1 && !is_es)
        sys_fatal("OpenGL 2.1+ is required.\nReported version: %s%d.%d", is_es ? "ES" : "", vmajor, vminor);

    glGenBuffers(1, &opengl_vbo);
    
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}


static void gfx_opengl_start_frame(void) {
    frame_count++;
#ifdef __ANDROID__
    if (s_fbo_mode) {
        // Each eye framebuffer is cleared by gfx_opengl_stereo_begin().
        return;
    }
#endif
    glDisable(GL_SCISSOR_TEST);
    if (gfx_stereo_enabled && gfx_stereo_eye > 0) {
        // Second eye: wipe ONLY its own half, color AND depth, so the two eyes
        // never share a depth buffer (that caused eye-vs-eye z-fighting).
        glEnable(GL_SCISSOR_TEST);
        glScissor(gfx_stereo_eye * gfx_stereo_eye_width, 0,
                  gfx_stereo_eye_width, 0x10000);
        glDepthMask(GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    } else {
        glDepthMask(GL_TRUE); // Must be set to clear Z-buffer
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glEnable(GL_SCISSOR_TEST);
}

/* ---------------- through-portal camera view ---------------- */
#include "game/portal.h"
extern float gfx_eye_shear[2];
extern int gfx_get_portal_view_proj(float out[4][4]);
extern int gfx_get_portal_view_proj_index(int index, float out[4][4]);
extern int gfx_portal_debug_blit;
extern int g_portal_debug_color;

/* Two relay images: one per portal, so a side-by-side pair shows a window in
 * both surfaces instead of only the one being looked at. */
#define PORTAL_VIEWS 2
#define PORTAL_EYES  2
static GLuint s_portal_fbo[PORTAL_VIEWS][PORTAL_EYES];
static GLuint s_portal_tex[PORTAL_VIEWS][PORTAL_EYES];
static GLuint s_portal_depth[PORTAL_VIEWS][PORTAL_EYES];
static int s_portal_w[PORTAL_VIEWS][PORTAL_EYES], s_portal_h[PORTAL_VIEWS][PORTAL_EYES];
static GLuint s_portal_prog = 0;
static GLint s_portal_apos = -1, s_portal_auv = -1, s_portal_utex = -1;

static const char *s_portal_vs =
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 1.0);\n"
    "  vUV = aUV;\n"
    "}\n";

static const char *s_portal_fs =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(uTex, vUV);\n"
    "}\n";

static int portal_ensure_resources(int index, int eye, int w, int h) {
    if (s_portal_prog == 0) {
        GLuint vs = compile_shader(GL_VERTEX_SHADER, s_portal_vs);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, s_portal_fs);
        s_portal_prog = glCreateProgram();
        glAttachShader(s_portal_prog, vs);
        glAttachShader(s_portal_prog, fs);
        glLinkProgram(s_portal_prog);
        GLint ok = 0;
        glGetProgramiv(s_portal_prog, GL_LINK_STATUS, &ok);
        __android_log_print(ANDROID_LOG_INFO, "SM64PORTAL", "portal shader link=%d", ok);
        glDeleteShader(vs);
        glDeleteShader(fs);
        s_portal_apos = glGetAttribLocation(s_portal_prog, "aPos");
        s_portal_auv = glGetAttribLocation(s_portal_prog, "aUV");
        s_portal_utex = glGetUniformLocation(s_portal_prog, "uTex");
    }
    if (s_portal_fbo[index][eye] != 0 && w == s_portal_w[index][eye] && h == s_portal_h[index][eye]) {
        return 1;
    }
    if (s_portal_fbo[index][eye]) glDeleteFramebuffers(1, &s_portal_fbo[index][eye]);
    if (s_portal_tex[index][eye]) glDeleteTextures(1, &s_portal_tex[index][eye]);
    if (s_portal_depth[index][eye]) glDeleteRenderbuffers(1, &s_portal_depth[index][eye]);
    s_portal_fbo[index][eye] = s_portal_tex[index][eye] = s_portal_depth[index][eye] = 0;
    s_portal_w[index][eye] = s_portal_h[index][eye] = 0;

    glGenTextures(1, &s_portal_tex[index][eye]);
    glBindTexture(GL_TEXTURE_2D, s_portal_tex[index][eye]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &s_portal_fbo[index][eye]);
    glBindFramebuffer(GL_FRAMEBUFFER, s_portal_fbo[index][eye]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_portal_tex[index][eye], 0);
    glGenRenderbuffers(1, &s_portal_depth[index][eye]);
    glBindRenderbuffer(GL_RENDERBUFFER, s_portal_depth[index][eye]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_portal_depth[index][eye]);

    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    __android_log_print(ANDROID_LOG_INFO, "SM64PORTAL", "fbo%d/%d=%u tex=%u %dx%d status=0x%x",
                        index, eye, s_portal_fbo[index][eye], s_portal_tex[index][eye], w, h, st);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        return 0;
    }
    s_portal_w[index][eye] = w;
    s_portal_h[index][eye] = h;
    return 1;
}

int gfx_opengl_portal_begin(int index, int eye, int w, int h) {
    if (index < 0 || index >= PORTAL_VIEWS || eye < 0 || eye >= PORTAL_EYES) {
        return 0;
    }
    if (w < 1 || h < 1 || !portal_ensure_resources(index, eye, w, h)) {
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_portal_fbo[index][eye]);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    if (g_portal_debug_color) {
        glClearColor(0.10f, 0.85f, 0.25f, 1.0f);   /* solid relay test */
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, w, h);
    return 1;
}

void gfx_opengl_portal_fill_green(void) {
    glClearColor(0.10f, 0.85f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void gfx_opengl_portal_end(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gfx_opengl_portal_overlay(int eye, int index) {
    const PortalViewState *vs;
    float vp[4][4];
    float buf[((PORTAL_VIEW_SEGS + 2) * 5)];
    int n = 0;
    /* Gameplay 3D is post-scaled per axis inside gfx_sp_vertex (the panel
     * calibration).  The decal has to use the same scale or it is drawn wider
     * than the portal and samples the relay image from the wrong place. */
    extern float gfx_geo_x_scale, gfx_geo_y_scale;
    const float gsx = gfx_geo_x_scale;
    const float gsy = gfx_geo_y_scale;

    if (index < 0 || index >= PORTAL_VIEWS) {
        return;
    }
    vs = (index == 1) ? &g_portal_view2 : &g_portal_view;
    if (!vs->active || s_portal_tex[index][eye & 1] == 0 || s_portal_prog == 0) {
        return;
    }
    if (gfx_portal_debug_blit) {
        static const float q[20] = {
            -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 0.0f, 1.0f,
             1.0f,  1.0f, 0.0f, 1.0f, 1.0f,
        };
        glUseProgram(s_portal_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_portal_tex[index][eye & 1]);
        glUniform1i(s_portal_utex, 0);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        /* The game leaves opengl_vbo bound; on GLES a bound VBO makes these
         * client pointers look like buffer offsets and the draw produces
         * nothing. Unbind for the overlay, then restore it. */
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(s_portal_apos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q);
        glVertexAttribPointer(s_portal_auv, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q + 3);
        glEnableVertexAttribArray(s_portal_apos);
        glEnableVertexAttribArray(s_portal_auv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(s_portal_apos);
        glDisableVertexAttribArray(s_portal_auv);
        glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
        glEnable(GL_DEPTH_TEST);
        glUseProgram(0);
        return;
    }
    if (!gfx_get_portal_view_proj_index(index, vp)) {
        return;
    }

    for (int i = -1; i < vs->count; i++) {
        const float *p = (i < 0) ? vs->center : vs->rim[i];
        float x = p[0] * vp[0][0] + p[1] * vp[1][0] + p[2] * vp[2][0] + vp[3][0];
        float y = p[0] * vp[0][1] + p[1] * vp[1][1] + p[2] * vp[2][1] + vp[3][1];
        float z = p[0] * vp[0][2] + p[1] * vp[1][2] + p[2] * vp[2][2] + vp[3][2];
        float w = p[0] * vp[0][3] + p[1] * vp[1][3] + p[2] * vp[2][3] + vp[3][3];
        float u, v, nx, ny, nz;
        if (w > 1e-5f) {
            nx = x / w;
            ny = y / w;
            nz = z / w;
        } else {
            nx = 0.0f; ny = 0.0f; nz = 1.0f;
        }
        nx *= gsx;
        ny *= gsy;
        /* Per-eye relay: the image was rendered with this eye's shear already
         * baked in, so the shear must be applied before the UV is derived and
         * must not be added again below. */
        nx += (gfx_stereo_enabled && eye >= 0) ? gfx_eye_shear[eye & 1] : 0.0f;
        u = nx * 0.5f + 0.5f;
        v = ny * 0.5f + 0.5f;

        /* Nudge the decal towards the camera along the portal normal.  The
         * portal plane sits ~4 units off the wall, which is far below the
         * eye buffer's 16-bit depth resolution at 1000+ units, so without
         * this the wall wins most of the depth test and only a thin sliver of
         * the decal survives. */
        {
            float ox = p[0] + vs->normal[0] * vs->decal_offset;
            float oy = p[1] + vs->normal[1] * vs->decal_offset;
            float oz = p[2] + vs->normal[2] * vs->decal_offset;
            float px = ox * vp[0][0] + oy * vp[1][0] + oz * vp[2][0] + vp[3][0];
            float py = ox * vp[0][1] + oy * vp[1][1] + oz * vp[2][1] + vp[3][1];
            float pz = ox * vp[0][2] + oy * vp[1][2] + oz * vp[2][2] + vp[3][2];
            float pw = ox * vp[0][3] + oy * vp[1][3] + oz * vp[2][3] + vp[3][3];
            if (pw > 1e-5f) {
                nx = px / pw * gsx;
                ny = py / pw * gsy;
                nz = pz / pw;
            }
        }

        buf[n++] = nx;
        buf[n++] = ny;
        buf[n++] = nz;
        buf[n++] = u;
        buf[n++] = v;
    }
    /* close the fan */
    for (int k = 0; k < 5; k++) {
        buf[n] = buf[5 + k];
        n++;
    }

    {
        static int s_plog;
        if ((s_plog++ % 120) == 0) {
            GLint gvp[4] = { 0, 0, 0, 0 };
            float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
            glGetIntegerv(GL_VIEWPORT, gvp);
            for (int i = -1; i < vs->count; i++) {
                const float *p = (i < 0) ? vs->center : vs->rim[i];
                float x = p[0]*vp[0][0] + p[1]*vp[1][0] + p[2]*vp[2][0] + vp[3][0];
                float y = p[0]*vp[0][1] + p[1]*vp[1][1] + p[2]*vp[2][1] + vp[3][1];
                float w = p[0]*vp[0][3] + p[1]*vp[1][3] + p[2]*vp[2][3] + vp[3][3];
                if (w > 1e-5f) {
                    float nx = x / w, ny = y / w;
                    if (nx < mnx) mnx = nx; if (nx > mxx) mxx = nx;
                    if (ny < mny) mny = ny; if (ny > mxy) mxy = ny;
                }
            }
            __android_log_print(ANDROID_LOG_INFO, "SM64PORTAL",
                "decal vp=%d,%d %dx%d ndc=[%.3f..%.3f, %.3f..%.3f] err=0x%x",
                gvp[0], gvp[1], gvp[2], gvp[3], mnx, mxx, mny, mxy, glGetError());
        }
    }
    glUseProgram(s_portal_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_portal_tex[index][eye & 1]);
    glUniform1i(s_portal_utex, 0);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    /* The decal must live in the depth buffer so nearer geometry (Mario,
     * pillars, the portal frame) occludes it.  It is coplanar with the wall it
     * is mounted on, so nudge it towards the camera in clip space with a
     * polygon offset instead of relying on world-space separation alone.
     * Knob `decal.depth 0` turns the test off for A/B debugging. */
    {
        extern int g_ptc_decal_depth;
        if (g_ptc_decal_depth) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-4.0f, -4.0f);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }
    glDepthMask(GL_FALSE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(s_portal_apos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), buf);
    glVertexAttribPointer(s_portal_auv, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), buf + 3);
    glEnableVertexAttribArray(s_portal_apos);
    glEnableVertexAttribArray(s_portal_auv);
    glDrawArrays(GL_TRIANGLE_FAN, 0, PORTAL_VIEW_SEGS + 2);

    /* Depth-limited screen-space recursion: draw the relay image again,
     * inset towards the portal centre, alternating with the other portal's
     * image so the window shows a window.  Cheap (no extra scene passes) and
     * bounded by relay.recursion. */
    {
        extern int g_ptc_portal_recursion;
        int levels = g_ptc_portal_recursion;
        if (levels > 4) levels = 4;
        if (levels > 0 && s_portal_tex[1 - index][eye & 1] != 0) {
            const float cnx = buf[0], cny = buf[1];
            const float cu = buf[3], cv = buf[4];
            float tmp[((PORTAL_VIEW_SEGS + 2) * 5)];
            for (int lvl = 1; lvl <= levels; lvl++) {
                float sc = 1.0f;
                for (int q = 0; q < lvl; q++) sc *= 0.78f;
                for (int k = 0; k < PORTAL_VIEW_SEGS + 2; k++) {
                    float *d = &tmp[k * 5];
                    const float *v = &buf[k * 5];
                    d[0] = cnx + (v[0] - cnx) * sc;
                    d[1] = cny + (v[1] - cny) * sc;
                    d[2] = v[2];
                    d[3] = cu + (v[3] - cu) * sc;
                    d[4] = cv + (v[4] - cv) * sc;
                }
                glBindTexture(GL_TEXTURE_2D,
                              s_portal_tex[(lvl & 1) ? (1 - index) : index][eye & 1]);
                glVertexAttribPointer(s_portal_apos, 3, GL_FLOAT, GL_FALSE,
                                      5 * sizeof(float), tmp);
                glVertexAttribPointer(s_portal_auv, 2, GL_FLOAT, GL_FALSE,
                                      5 * sizeof(float), tmp + 3);
                glDrawArrays(GL_TRIANGLE_FAN, 0, PORTAL_VIEW_SEGS + 2);
            }
        }
    }
    glDisableVertexAttribArray(s_portal_apos);
    glDisableVertexAttribArray(s_portal_auv);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
}

static void gfx_opengl_shutdown(void) {
}

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_opengl_z_is_from_0_to_1,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_new_texture,
    gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_test,
    gfx_opengl_set_depth_mask,
    gfx_opengl_set_zmode_decal,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_start_frame,
    gfx_opengl_shutdown
};

#endif // RAPI_GL
