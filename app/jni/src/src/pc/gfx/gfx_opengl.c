#ifdef RAPI_GL

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Stereo state lives in gfx_pc.c (set by the Leia bridge).
extern int gfx_stereo_enabled;
extern int gfx_stereo_eye;
extern int gfx_single_eye;
extern int gfx_stereo_eye_width;
extern int gfx_stereo_full_w;
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
// Emissive tag state (gfx_pc.c, driven by GFX_OP_EMISSIVE_MARKER).
extern int gfx_emissive_active;
extern int gfx_emissive_rgb[3];
extern int gfx_emissive_strength;

/* Occluder punch-through.  The scene fragment shader discards occluding
 * fragments inside a screen-space circle around Mario.  gfx_pc.c supplies the
 * frame's camera view*projection and Mario's centre/front world points; the
 * per-eye shear/toe/flip is applied here when the uniforms are uploaded. */
extern int gfx_ui_pass;
extern float gfx_sky_scale_x;
extern float gfx_sky_scale_y;
extern float gfx_sky_off_x;
extern float gfx_sky_off_y;
extern int gfx_portal_pass;
extern int gfx_stereo_enabled;
extern int gfx_stereo_2d;
extern float gfx_eye_shear[2];
extern float gfx_eye_toe[2];
extern int gfx_eye_flip[2];
static float s_occl_vp[4][4];
static float s_occl_centre[3];
static float s_occl_front[3];
static int   s_occl_geo_valid;
/* Dials (fx keys 274-278). */
static int   s_occl_mode = 0;        /* 274: 0 off, 1 dropout, 2 partial, 3 cutout */
static int   s_occl_radius = 150;    /* 275: circle radius, target pixels */
static int   s_occl_feather = 35;    /* 276: edge feather % of the radius */
static int   s_occl_cover = 50;      /* 277: dither coverage % (mode 2) */
static int   s_occl_bias = 5;        /* 278: depth bias 0.0002 per unit */
/* Flat path: the game's last scene viewport (gl_FragCoord pixels). */
static int   s_occl_vp_x = 0, s_occl_vp_y = 0, s_occl_vp_w = 1, s_occl_vp_h = 1;

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
    GLint occl_mode_loc;
    GLint occl_centre_loc;
    GLint occl_radius_loc;
    GLint occl_depth_loc;
    GLint occl_feather_loc;
    GLint occl_cover_loc;
    GLint occl_bias_loc;
    GLint occl_aspect_loc;
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

static void gfx_opengl_set_occluder_uniforms(struct ShaderProgram *prg);

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
#ifdef __ANDROID__
    gfx_opengl_set_occluder_uniforms(prg);
#endif
}

static inline void gfx_opengl_set_texture_uniforms(struct ShaderProgram *prg, const int tile) {
    if (prg == NULL) {
        /* A texture bind with no program loaded (e.g. right after an internal
         * pass unbound its shader): nothing to update. */
        static int logged;
        if (logged < 4) {
            logged++;
            __android_log_print(ANDROID_LOG_WARN, "SM64VEL",
                                "texture bind with no shader program (tile %d)", tile);
        }
        return;
    }
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

    char vs_buf[2048];
    /* The generated fragment shader (combiner formula + 3-point filtering +
     * splat block + occluder block) is close to the old 2 KB limit; overflow
     * here corrupts the stack on the render thread. */
    char fs_buf[8192];
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
    /* Occluder punch-through: discards fragments between the camera and Mario
     * inside a screen-space circle.  Values are per eye; mode 0 makes the
     * whole block a single coherent branch that is false for every fragment. */
    append_line(fs_buf, &fs_len, "uniform float uOccludeMode;");
    append_line(fs_buf, &fs_len, "uniform vec2 uOccludeCentre;");
    append_line(fs_buf, &fs_len, "uniform float uOccludeRadius;");
    append_line(fs_buf, &fs_len, "uniform float uOccludeDepth;");
    append_line(fs_buf, &fs_len, "uniform float uOccludeFeather;");
    append_line(fs_buf, &fs_len, "uniform float uOccludeCover;");
    append_line(fs_buf, &fs_len, "uniform float uOccludeBias;");
    /* x compensation for the panel's single-view stretch (2D mode shows one
     * 1280-wide eye stretched across the 2560-wide panel). */
    append_line(fs_buf, &fs_len, "uniform float uOccludeAspect;");
    /* Interleaved gradient noise: static per pixel (no frame term), so the
     * dither pattern does not crawl when the scene is still. */
    append_line(fs_buf, &fs_len,
            "float occl_hash(vec2 p) {\n"
            "  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));\n"
            "}");
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
    /* Occluder punch-through.  Runs last, on fragments that would reach the
     * framebuffer.  Only fragments *nearer* than Mario are affected
     * (gl_FragCoord.z < uOccludeDepth - bias), so Mario's own faces and
     * everything behind him are untouched.  The z > 0.0001 guard excludes HUD
     * texture rectangles, which are emitted at clip z = -1. */
    append_line(fs_buf, &fs_len,
            "if (uOccludeMode > 0.5 && gl_FragCoord.z > 0.0001 && gl_FragCoord.z < uOccludeDepth - uOccludeBias) {");
    append_line(fs_buf, &fs_len,
            "  vec2 occl_q = gl_FragCoord.xy - uOccludeCentre;");
    append_line(fs_buf, &fs_len,
            "  occl_q.x /= max(uOccludeAspect, 0.01);");
    append_line(fs_buf, &fs_len,
            "  float occl_d = length(occl_q / max(uOccludeRadius, 1.0));");
    append_line(fs_buf, &fs_len,
            "  if (uOccludeMode < 1.5) { if (occl_d < 1.0) discard; }");
    append_line(fs_buf, &fs_len,
            "  else if (occl_d < 1.0) {");
    append_line(fs_buf, &fs_len,
            "    float occl_cov = 1.0 - smoothstep(1.0 - uOccludeFeather, 1.0, occl_d);");
    append_line(fs_buf, &fs_len,
            "    float occl_k = uOccludeMode < 2.5 ? occl_cov * uOccludeCover : occl_cov;");
    append_line(fs_buf, &fs_len,
            "    if (occl_hash(gl_FragCoord.xy) < occl_k) discard;");
    append_line(fs_buf, &fs_len, "  }");
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
    prg->occl_mode_loc = glGetUniformLocation(shader_program, "uOccludeMode");
    prg->occl_centre_loc = glGetUniformLocation(shader_program, "uOccludeCentre");
    prg->occl_radius_loc = glGetUniformLocation(shader_program, "uOccludeRadius");
    prg->occl_depth_loc = glGetUniformLocation(shader_program, "uOccludeDepth");
    prg->occl_feather_loc = glGetUniformLocation(shader_program, "uOccludeFeather");
    prg->occl_cover_loc = glGetUniformLocation(shader_program, "uOccludeCover");
    prg->occl_bias_loc = glGetUniformLocation(shader_program, "uOccludeBias");
    prg->occl_aspect_loc = glGetUniformLocation(shader_program, "uOccludeAspect");

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

/* Project a world point through the frame's view*projection, then apply the
 * same per-eye shear/toe/flip the scene pass bakes into its projection.
 * Row-vector convention: v' = v * M. */
static int occl_project(const float p[3], float out[3], int eye) {
    const float (*m)[4] = (const float (*)[4]) s_occl_vp;
    float x = p[0] * m[0][0] + p[1] * m[1][0] + p[2] * m[2][0] + m[3][0];
    float y = p[0] * m[0][1] + p[1] * m[1][1] + p[2] * m[2][1] + m[3][1];
    float z = p[0] * m[0][2] + p[1] * m[1][2] + p[2] * m[2][2] + m[3][2];
    float w = p[0] * m[0][3] + p[1] * m[1][3] + p[2] * m[2][3] + m[3][3];

    if (w <= 1.0e-5f) {
        return 0;
    }
    out[0] = x / w;
    out[1] = y / w;
    out[2] = z / w;
    if (gfx_stereo_enabled && !gfx_stereo_2d) {
        out[0] += gfx_eye_shear[eye] + gfx_eye_toe[eye] * out[2];
        if (gfx_eye_flip[eye]) {
            out[1] = -out[1];
        }
    }
    return 1;
}

/* Push the per-eye occluder circle into one scene program.  Called on every
 * shader load and again when the frame's projection is captured, so a program
 * that stays bound across frames or eyes still gets fresh values. */
static void gfx_opengl_set_occluder_uniforms(struct ShaderProgram *prg) {
    int eye;
    float mode;
    float c[3], f[3];

    if (prg == NULL || prg->occl_mode_loc < 0) {
        return;
    }
    /* Never punch holes in the HUD, the portal image or the shadow pass. */
    eye = gfx_stereo_enabled ? (gfx_stereo_eye & 1) : 0;
    mode = (gfx_ui_pass || gfx_portal_pass) ? 0.0f : (float) s_occl_mode;
    if (!s_occl_geo_valid
        || !occl_project(s_occl_centre, c, eye)
        || !occl_project(s_occl_front, f, eye)) {
        mode = 0.0f;
    }
    glUniform1f(prg->occl_mode_loc, mode);
    if (mode <= 0.5f) {
        return;
    }
    {
        float px, py;
        if (s_fbo_mode) {
            /* gl_FragCoord lives in the eye FBO; the scene is drawn into the
             * canvas sub-rect (see gfx_opengl_stereo_begin). */
            px = (float) s_canvas_x + (c[0] * 0.5f + 0.5f) * (float) s_canvas_w;
            py = (float) s_canvas_y + (c[1] * 0.5f + 0.5f) * (float) s_canvas_h;
        } else {
            px = (float) s_occl_vp_x + (c[0] * 0.5f + 0.5f) * (float) s_occl_vp_w;
            py = (float) s_occl_vp_y + (c[1] * 0.5f + 0.5f) * (float) s_occl_vp_h;
        }
        glUniform2f(prg->occl_centre_loc, px, py);
        glUniform1f(prg->occl_radius_loc, (float) s_occl_radius);
        glUniform1f(prg->occl_depth_loc, f[2] * 0.5f + 0.5f);
        glUniform1f(prg->occl_feather_loc, (float) s_occl_feather / 100.0f);
        glUniform1f(prg->occl_cover_loc, (float) s_occl_cover / 100.0f);
        glUniform1f(prg->occl_bias_loc, (float) s_occl_bias * 0.0002f);
        /* The Leia panel interlaces one 1280-wide eye across the 2560-wide
         * panel, so a screen-space circle must be narrower in x inside the
         * eye to look round on the panel.  The flat path is 1:1. */
        {
            float aspect = 1.0f;
            if (gfx_stereo_enabled && gfx_stereo_full_w > 0
                && gfx_stereo_eye_width > 0) {
                aspect = (float) gfx_stereo_eye_width / (float) gfx_stereo_full_w;
            }
            glUniform1f(prg->occl_aspect_loc, aspect);
        }
        /* One-line setup probe for headless verification. */
        {
            static int logged;
            if (logged < 200) {
                logged++;
                __android_log_print(ANDROID_LOG_INFO, "SM64OCCL",
                        "eye=%d mode=%d px=%.1f py=%.1f depth=%.4f r=%d a=%.2f",
                        eye, s_occl_mode, px, py,
                        f[2] * 0.5f + 0.5f, s_occl_radius,
                        gfx_stereo_enabled && gfx_stereo_full_w > 0
                            ? (float) gfx_stereo_eye_width / (float) gfx_stereo_full_w
                            : 1.0f);
            }
        }
    }
}

/* Called from gfx_pc.c when the scene's perspective projection is loaded. */
void gfx_opengl_occluder_geo(const float vp[4][4], const float centre[3],
                             const float front[3], int valid) {
    memcpy(s_occl_vp, vp, sizeof(s_occl_vp));
    memcpy(s_occl_centre, centre, sizeof(s_occl_centre));
    memcpy(s_occl_front, front, sizeof(s_occl_front));
    s_occl_geo_valid = valid;
    /* Mid-scene a scene program is bound; refresh it now so the uniforms are
     * never a frame (or an eye) stale when the program is reused. */
    if (opengl_prg != NULL) {
        gfx_opengl_set_occluder_uniforms(opengl_prg);
    }
}


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
/* 390-396: reduced-resolution AO chain (see "Half-resolution AO" below). */
static int s_fx_ao_path = 1;      /* 390: 0 = full-res in-shader, 1 = low-res  */
static int s_fx_ao_div = 2;       /* 391: low-res divisor (2 = half)           */
/* 392: 0 = per-frame rotation (the legacy pattern: one direction set for the
 * whole frame, no static grain but temporal shimmer), 1 = static per-pixel
 * IGN rotation.  Measured 2026-11-26: per-pixel rotation costs +1.9 to +3.1 ms
 * in 3D (neighbouring pixels stop sharing tap directions, so the fetch pattern
 * goes incoherent) and leaves visible speckle on flat walls at half res, so
 * the shipping default stays with the look the legacy path already had. */
static int s_fx_ao_jitter = 0;    /* 392: 1 = static per-pixel rotation        */
static int s_fx_ao_up = 0;        /* 393: 0 = 4-tap depth-aware, 1 = bilinear  */
static int s_fx_ao_sky = 1;       /* 394: 1 = skip the gather on sky/far       */
static int s_fx_ao_trig = 0;      /* 395: 0 = rotation matrix, 1 = per-tap trig*/
static int s_fx_ao_probe = 0;     /* 396: 1 = prepass only, 2 = no apply       */

static int s_fx_dof_on = 0;
static int s_fx_dof_focus = 800;      // world units (manual mode)
static int s_fx_dof_auto = 1;         // 1 = focus on Mario (default)
// Distance to Mario's centre minus his hitbox radius, written by the game code
// each tick (rendering_graph_node.c) so auto-focus tracks him.
float gfx_dof_focus_auto = 800.0f;
static int s_fx_dof_aperture = 100;   // percent
static int s_fx_dof_maxblur = 12;     // pixels
static int s_fx_dof_samples = 16;
static int s_fx_bokeh_on = 0;
static int s_fx_bokeh_aspect = 100;   /* percent width/height */
static int s_fx_bokeh_blades = 6;
static int s_fx_bokeh_rot = 0;
static int s_fx_anamorphic = 100;     /* percent horizontal stretch */
static int s_fx_bokeh_highlight = 100;
static int s_fx_cat_eye = 0;
/* C.DOF cost reduction (fx 380-389).
 *
 * The legacy path is a single full-eye-resolution gather inside the big FX
 * shader: 16 taps, each fetching full-res depth and re-deriving linear z and a
 * CoC, for an output that is defocused by definition.  The reduced-resolution
 * path below mirrors the denoiser prepass (see s_dn_*):
 *
 *   prepare  full eye colour+depth -> eye/div colour (a = signed CoC) and a
 *            packed linear-z buffer (upsample edge stop + legacy z test)
 *   gather   the same spiral / bokeh kernel at eye/div; the tap CoC comes from
 *            the colour alpha, so a tap is one fetch and no linZ/division
 *   upsample full-res depth-aware bilateral mix; the pixel's own CoC decides
 *            how much of the sharp scene survives (in-focus stays sharp)
 *
 * The FX shader then receives the finished colour as uColor and its DOF block
 * stays off.  Everything is skipped when DOF is off, so the MAIN path is
 * unchanged.  Defaults pick the new path; every step has a dial for an A/B.
 */
static int s_fx_dof_half = 1;       /* 380: 0 legacy in-shader, 1 reduced-res */
static int s_fx_dof_div = 2;        /* 381: reduced-res divisor (2..4) */
static int s_fx_dof_near = 1;       /* 382: 0 none, 1 CoC-aware, 2 low-res z test */
static int s_fx_dof_taps = 0;       /* 383: 0 = key 15, else prepass tap count */
static int s_fx_dof_up = 1;         /* 384: 0 bilinear, 1 depth-aware bilateral */
static int s_fx_dof_coc = 1;        /* 385: 0 recompute CoC per tap, 1 precomputed */
static int s_fx_dof_bokeh_pre = 1;  /* 386: 0 in-shader atan mask, 1 precomputed */
static int s_fx_dof_bilinear = 1;   /* 387: 0 point taps, 1 bilinear taps */
static int s_fx_dof_stage = 0;      /* 388: 1 log per-stage ms incl. glFinish */
static int s_fx_dof_sharp = 100;    /* 389: in-focus sharpness pass-through % */
/* Alternative denoised rendering path. */
static int s_denoise_path = 0;
static int s_denoise_on = 0;
static int s_denoise_intensity = 60;
static int s_denoise_passes = 2;
static int s_denoise_radius = 1;
static int s_denoise_frames = 1;
static int s_denoise_bounces = 1;
static int s_denoise_caption = 1;
/* Denoise resolution divisor: 1 = full res (legacy path), 2..4 = the scene
 * colour is downsampled to scene_res/div, filtered there and upsampled back
 * bilaterally.  4 is the default (DENOISER-RESEARCH.md §4.1). */
static int s_denoise_div = 4;
/* Motion-vector heatmap normalisation (pixels of motion = full scale). */
static int s_fx_vec_gain = 120;
extern int gfx_vel_enabled;
extern int gfx_vel_divisor;
extern int gfx_vel_stride;
/* Motion-vector / sub-frame blur dials. */
static int s_vel_source = 1;        /* 130: 1 = vertex vectors, 0 = camera only */
static int s_mb_shutter = 180;      /* 131: degrees */
static int s_mb_substeps = 4;       /* 132: taps along the vector */
static int s_mb_maxpx = 16;         /* 133: max blur radius, px */
static int s_vel_divisor_ui = 4;    /* 134: velocity target divisor */
static int s_vel_stride_ui = 1;     /* 135: replay every N frames */
static int s_mb_jitter = 100;       /* 136: per-frame sub-step jitter % */
static int s_mb_depth_reject = 20;  /* 137: depth rejection threshold */
static int s_mb_clamp_px = 60;      /* 138: velocity clamp, px */
/* 240: tile-max bound quality. 0 = off (skips the pass and all samples),
 * 1 = one sample (default, cheapest), 2 = 3x3 neighbour max. */
static int s_velmax_mode = 1;
static int s_vec_style = 2;         /* 139: 0 magnitude, 1 direction, 2 segments */

static int s_fx_gi_on = 0;
static int s_fx_gi_intensity = 100;   // percent
static int s_fx_gi_radius = 150;      // world units
static int s_fx_gi_steps = 6;
static int s_fx_gi_rays = 4;

/* Reduced-resolution SSGI (fx 400-405).  The gather runs at canvas/div and a
 * dedicated pass adds it back; div 1 keeps the original full-res in-shader
 * loop as the A/B.  400 divisor, 401 low-res blur passes, 402 rotated-grid +
 * per-frame jitter, 403 low-res debug view, 404 far/sky skip percent,
 * 405 normal-aware tap reject. */
static int s_fx_gi_div = 4;
static int s_fx_gi_blur = 1;
static int s_fx_gi_jitter = 1;
static int s_fx_gi_debug = 0;
static int s_fx_gi_skip = 95;
static int s_fx_gi_norm = 0;

/* ---------------- Tone mapping + colour grading (GRADE tab, fx 280-289) ---
 * Runs at the very end of the FX shader, after AO/DOF/shadow/GI/SSRTX/lights/
 * rays have been composited into `outc` and after the debug-view chain, so
 * every debug buffer stays raw and additive light is tonemapped exactly once.
 * Everything is off by default: with s_grade_on == 0 the shader takes one
 * uniform branch and the picture is bit-identical to before.
 *
 * 280 master, 281 tonemap operator, 282 exposure EV*100, 283 white point %,
 * 284 look, 285 look strength %, 286 saturation %, 287 contrast %,
 * 288 tint, 289 pre/post split view.
 */
static int s_grade_on = 0;
static int s_grade_op = 2;          /* 0 linear, 1 reinhard, 2 ACES, 3 filmic */
static int s_grade_exposure = 0;    /* EV * 100 (-300..300) */
static int s_grade_white = 100;     /* percent -> 1.0..4.0 scene white point */
static int s_grade_look = 0;        /* 0 none, 1 sepia, 2 cool, 3 warm, 4 bleach, 5 cel, 6 LUT */
static int s_grade_strength = 100;  /* percent */
static int s_grade_sat = 100;       /* percent */
static int s_grade_contrast = 0;    /* -100..100 -> 0.5x..2x around mid grey */
static int s_grade_tint = 0;        /* -100 cool .. +100 warm */
static int s_grade_debug = 0;       /* 1 = left half pre-tonemap, right half post */

// SSRTX: screen-space ray-traced reflections (ReShade SSR/SSRT-style).
static int s_fx_ssrtx_on = 0;
static int s_fx_ssrtx_intensity = 100;  // percent
static int s_fx_ssrtx_distance = 600;   // max ray length (world units)
static int s_fx_ssrtx_steps = 20;
static int s_fx_ssrtx_rays = 1;
static int s_fx_ssrtx_thickness = 80;   // depth window for a hit (world units)
static int s_fx_ssrtx_roughness = 0;    // percent cone spread (needs rays > 1)
static int s_fx_ssrtx_fade = 100;       // screen-edge fade percent

/* Vector motion blur + dynamic lights + probe debug. */
static int s_fx_mb_on = 0;
static int s_fx_mb_strength = 60;    // percent of the velocity vector
static int s_fx_mb_samples = 8;
static int s_light_count = 0;
static int s_light_manual = 0;   // PTC lightshow owns the slots (legacy API)
static int s_light_shadows = 0;
static int s_light_intensity = 120;  // percent
static int s_light_shadow_steps = 6;
static float s_light_pos[4][3] = {
    { 0.0f, 2200.0f, -2800.0f }, { 1400.0f, 1800.0f, -2800.0f },
    { -1200.0f, 1600.0f, -2400.0f }, { 0.0f, 900.0f, -2000.0f } };
static float s_light_color[4][3] = {
    { 1.0f, 0.55f, 0.25f }, { 0.30f, 0.60f, 1.0f },
    { 0.90f, 0.25f, 0.80f }, { 1.0f, 1.0f, 1.0f } };
static float s_light_radius[4] = { 3200.0f, 2800.0f, 2400.0f, 2000.0f };
static int s_probe_vis = 0;
static float s_probe_center[3] = { 0.0f, 700.0f, -2800.0f };
#define PTC_PROBE_MAX 16
static float s_probe_pos_u[PTC_PROBE_MAX][3];
static float s_probe_col_u[PTC_PROBE_MAX][3];
static int s_probe_spacing = 700;
static int s_probe_dim = 3;      /* preset: 1 = single, 2 = 2x2x2 (see 115-117) */
static int s_probe_cols = 2;     /* probes along X */
static int s_probe_rows = 2;     /* probes along Y */
static int s_probe_depth = 2;    /* probes along Z (repeats) */
static int s_probe_radius = 55;
static int s_probe_res = 16;
static float s_fx_cur_vp[4][4];
static float s_fx_prev_vp[4][4];
static float s_prev_cam_inv[4][4];
static float s_fx_cur_cam_inv[4][4];
static int s_have_prev_cam_inv = 0;
static int s_fx_have_prev = 0;
static uint32_t s_fx_vp_frame = 0xFFFFFFFFu;

static int s_fx_debug = 0;            // 0 off, 1 AO, 2 CoC, 3 GI
static int s_fx_normal_flip = 0;

/* Emissive materials + bloom dials (FX GLOW tab; declared here because
 * stereo_fx_any() above the implementation needs s_glow_on). */
int gfx_emissive_on = 1;      // FX_EMISSIVE_ON: master switch for tags
static int s_glow_on = 0;          // FX_GLOW_ON: bloom chain
static int s_glow_debug = 0;       // 0 off, 1 emissive buffer, 2 bloom buffer
static int s_glow_threshold = 48;  // 0..255 -> 0..2.0 luma threshold
static int s_glow_intensity = 120; // percent added back to the frame
static int s_glow_radius = 3;      // tap spacing in 1/2-res texels
static int s_glow_passes = 2;      // 1..3 blur iterations
static int s_glow_taps = 9;        // 5 or 9 gaussian taps
static int s_glow_gain = 100;      // emissive strength multiplier (percent)
static int s_glow_scene = 0;       // also bloom scene pixels above threshold

/* Jetpack nozzle light dials (movekit.c reads these through gfx_pc.h). */
int gfx_jet_light_on = 1;
int gfx_jet_light_intensity = 150;   // percent
int gfx_jet_light_radius = 220;      // world units at full thrust

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
extern float gfx_last_persp_proj[4][4];
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
    "uniform float uAOProbe;\n"
    "uniform float uDOFOn;\n"
    "uniform float uFocus;\n"
    "uniform float uAperture;\n"
    "uniform float uMaxBlur;\n"
    "uniform float uDOFSamples;\n"
    "uniform float uDofPre;\n"
    "uniform float uGIOn;\n"
    "uniform float uGIIntensity;\n"
    "uniform float uGIRadius;\n"
    "uniform float uGISteps;\n"
    "uniform float uGIRays;\n"
    "uniform float uDebug;\n"
    "uniform float uVecGain;\n"
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
    "uniform mat4 uCurVP;\n"
    "uniform mat4 uPrevVP;\n"
    "uniform mat4 uPrevCamInv;\n"
    "uniform float uMBOn;\n"
    "uniform float uMBStrength;\n"
    "uniform float uMBSamples;\n"
    "uniform sampler2D uVelocity;\n"
    "uniform float uVelOn;\n"
    "uniform float uVelScale;\n"
    "uniform float uMBShutter;\n"
    "uniform float uMBSubSteps;\n"
    "uniform float uMBMaxPx;\n"
    "uniform float uMBJitter;\n"
    "uniform float uMBDepthReject;\n"
    "uniform float uMBClampPx;\n"
    "uniform float uVelMaxMode;\n"
    "uniform float uVecStyle;\n"
    "uniform sampler2D uVelMax;\n"
    "uniform float uVelMaxOn;\n"
    "uniform vec2 uVelMaxGrid;\n"
    "uniform float uLightCount;\n"
    "uniform vec3 uLightPos[4];\n"
    "uniform vec3 uLightPosView[4];\n"
    "uniform vec3 uLightColor[4];\n"
    "uniform float uLightRadius[4];\n"
    "uniform float uLightIntensity;\n"
    "uniform float uLightShadows;\n"
    "uniform float uLightShadowSteps;\n"
    "uniform float uProbeVis;\n"
    "uniform vec3 uProbeCenter;\n"
    "uniform float uProbeSpacing;\n"
    "uniform float uProbeDim;\n"
    "uniform float uProbeRadiusW;\n"
    "uniform float uProbeRes;\n"
    "uniform vec3 uProbePos[16];\n"
    "uniform vec3 uProbeCol[16];\n"
    "uniform float uProbeCount;\n"
    "uniform float uBokehOn;\n"
    "uniform float uBokehAspect;\n"
    "uniform float uBokehBlades;\n"
    "uniform float uBokehRot;\n"
    "uniform float uAnamorphic;\n"
    "uniform float uBokehHighlight;\n"
    "uniform float uCatEye;\n"
    "uniform float uDenoisePath;\n"
    "uniform float uDenoiseOn;\n"
    "uniform float uDenoiseIntensity;\n"
    "uniform float uDenoisePasses;\n"
    "uniform float uDenoiseRadius;\n"
    "uniform float uDenoiseFrames;\n"
    "uniform float uDenoiseBounces;\n"
    "uniform sampler2D uRayTex;\n"
    "uniform float uRayOn;\n"
    "uniform float uRayIntensity;\n"
    "uniform float uRayDebug;\n"
    "uniform vec3 uRayColor;\n"
    "uniform vec2 uRayLightUV;\n"
    "uniform float uGradeOn;\n"
    "uniform float uGradeOp;\n"
    "uniform float uGradeExposure;\n"
    "uniform float uGradeWhite;\n"
    "uniform float uGradeLook;\n"
    "uniform float uGradeStrength;\n"
    "uniform float uGradeSat;\n"
    "uniform float uGradeContrast;\n"
    "uniform float uGradeTint;\n"
    "uniform float uGradeDebug;\n"
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
    "vec3 worldToView(vec3 pw) {\n"
    "  vec3 d = pw - uCamInv[3].xyz;\n"
    "  mat3 R = mat3(uCamInv);\n"
    "  return vec3(dot(R[0], d), dot(R[1], d), dot(R[2], d));\n"
    "}\n"    "vec3 worldToViewM(vec3 pw, mat4 M) {\n"
    "  vec3 d = pw - M[3].xyz;\n"
    "  mat3 R = mat3(M);\n"
    "  return vec3(dot(R[0], d), dot(R[1], d), dot(R[2], d));\n"
    "}\n"
    "vec2 viewToNdc(vec3 pv) {\n"
    "  float z = max(-pv.z, 1.0);\n"
    "  vec2 ndc = vec2(pv.x / (z * uTanHalf * uAspect), pv.y / (z * uTanHalf));\n"
    "  float ndcz = (uFar + uNear - 2.0 * uNear * uFar / z) / (uFar - uNear);\n"
    "  ndc.x += uShear + uToe * ndcz;\n"
    "  if (uFlipY > 0.5) ndc.y = -ndc.y;\n"
    "  return ndc;\n"
    "}\n"
    "float lightVis(vec3 Pv, vec3 Lv) {\n"
    "  float dist = length(Lv);\n"
    "  if (dist < 1.0 || uLightShadowSteps < 0.5) return 1.0;\n"
    "  vec3 dir = Lv / dist;\n"
    "  for (int s = 1; s <= 8; s++) {\n"
    "    if (float(s) > uLightShadowSteps) break;\n"
    "    float t = (float(s) / 8.0) * dist;\n"
    "    vec3 Ps = Pv + dir * t;\n"
    "    float rz = -Ps.z;\n"
    "    if (rz <= uNear) break;\n"
    "    vec2 cuv = viewToCanvas(Ps);\n"
    "    if (cuv.x < 0.0 || cuv.x > 1.0 || cuv.y < 0.0 || cuv.y > 1.0) break;\n"
    "    float sd = linZ(texture2D(uDepth, eyeUVfromCanvas(cuv)).r);\n"
    "    if (rz > sd + 3.0) return 0.0;\n"
    "  }\n"
    "  return 1.0;\n"
    "}\n"
    "vec3 denoiseTap(vec2 uv, float cz, vec3 cc, float stepPx) {\n"
    "  vec3 acc = vec3(0.0);\n"
    "  float ws = 0.0;\n"
    "  for (int k = 0; k < 5; k++) {\n"
    "    int dx = (k == 1) ? 1 : ((k == 2) ? -1 : 0);\n"
    "    int dy = (k == 3) ? 1 : ((k == 4) ? -1 : 0);\n"
    "    {\n"
    "      vec2 suv = uv + vec2(float(dx), float(dy)) * stepPx / uRes;\n"
    "      vec2 scuv = canvasUV(suv);\n"
    "      if (scuv.x < 0.0 || scuv.x > 1.0 || scuv.y < 0.0 || scuv.y > 1.0) continue;\n"
    "      float sz = linZ(texture2D(uDepth, suv).r);\n"
    "      vec3 sc = texture2D(uColor, suv).rgb;\n"
    "      float w = exp(-abs(sz - cz) * 0.10) * exp(-length(sc - cc) * 2.0);\n"
    "      acc += sc * w;\n"
    "      ws += w;\n"
    "    }\n"
    "  }\n"
    "  return ws > 0.0 ? acc / ws : cc;\n"
    "}\n"
    "vec3 heatColour(float t) {\n"
    "  t = clamp(t, 0.0, 1.0);\n"
    "  vec3 c = mix(vec3(0.04, 0.10, 0.85), vec3(0.0, 0.85, 0.95), smoothstep(0.0, 0.30, t));\n"
    "  c = mix(c, vec3(0.10, 0.95, 0.25), smoothstep(0.25, 0.55, t));\n"
    "  c = mix(c, vec3(1.0, 0.85, 0.10), smoothstep(0.50, 0.80, t));\n"
    "  c = mix(c, vec3(1.0, 0.15, 0.05), smoothstep(0.75, 1.0, t));\n"
    "  return c;\n"
    "}\n"
    "vec3 probeEnv(vec3 dir, vec3 state) {\n"
    "  float up = dir.y * 0.5 + 0.5;\n"
    "  vec3 sky = mix(vec3(0.03, 0.04, 0.07), vec3(0.30, 0.42, 0.60), up);\n"
    "  vec3 ground = vec3(0.16, 0.13, 0.10) * (1.0 - up);\n"
    "  return (sky + ground) * (0.25 + state * 1.6);\n"
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
    // Tone mapping + colour grading.  Display-referred chain, applied once to
    // the assembled scene colour: exposure -> tonemap -> look -> saturation ->
    // contrast -> tint.  Alpha is never touched (the pass writes 1.0).
    "float gradeReinhard(float x) { return x / (1.0 + x); }\n"
    "float gradeACES(float x) {\n"
    "  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);\n"
    "}\n"
    "float gradeFilmicPart(float x) {\n"
    "  const float A = 0.15; const float B = 0.50; const float C = 0.10;\n"
    "  const float D = 0.20; const float E = 0.02; const float F = 0.30;\n"
    "  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;\n"
    "}\n"
    // 0 linear, 1 Reinhard, 2 ACES (Narkowicz fit), 3 Hable/Uncharted2 filmic.
    "vec3 gradeTonemapOp(vec3 c) {\n"
    "  if (uGradeOp < 0.5) return c;\n"
    "  if (uGradeOp < 1.5) return vec3(gradeReinhard(c.r), gradeReinhard(c.g), gradeReinhard(c.b));\n"
    "  if (uGradeOp < 2.5) return vec3(gradeACES(c.r), gradeACES(c.g), gradeACES(c.b));\n"
    "  return vec3(gradeFilmicPart(c.r * 2.0), gradeFilmicPart(c.g * 2.0),\n"
    "              gradeFilmicPart(c.b * 2.0)) / max(gradeFilmicPart(22.4), 0.0001);\n"
    "}\n"
    // Looks are analytic; look 6 (LUT) is a separate pass (see below) because
    // adding a seventh sampler to this already heavy shader measured ~30 ms.
    "vec3 gradeLookColour(vec3 c) {\n"
    "  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "  if (uGradeLook < 0.5) return c;\n"
    "  if (uGradeLook < 1.5) {\n"
    "    return vec3(dot(c, vec3(0.393, 0.769, 0.189)),\n"
    "                dot(c, vec3(0.349, 0.686, 0.168)),\n"
    "                dot(c, vec3(0.272, 0.534, 0.131)));\n"
    "  }\n"
    "  if (uGradeLook < 2.5) return c * vec3(0.88, 1.01, 1.16) + vec3(0.0, 0.012, 0.03);\n"
    "  if (uGradeLook < 3.5) return c * vec3(1.14, 1.02, 0.86) + vec3(0.03, 0.01, 0.0);\n"
    "  if (uGradeLook < 4.5) {\n"
    // Bleach bypass: overlay(luma, colour) banks the highlights and drains
    // saturation, then a small straight colour mix keeps skin from going grey.
    "    vec3 fl = vec3(l);\n"
    "    vec3 ov = mix(2.0 * c * fl, 1.0 - 2.0 * (1.0 - c) * (1.0 - fl), step(0.5, fl));\n"
    "    return mix(c, ov, 0.7);\n"
    "  }\n"
    "  if (uGradeLook < 5.5) {\n"
    // Cel: per-pixel luminance step ramp (5 hard steps).  Quantising luminance
    // and rescaling the colour by q/l keeps hue and chroma edges; there is no
    // spatial filter, so geometry edges are untouched.
    "    float steps = 5.0;\n"
    "    float q = floor(clamp(l, 0.0, 1.0) * steps + 0.5) / steps;\n"
    "    float ratio = clamp(q / max(l, 0.02), 0.0, 4.0);\n"
    "    return clamp(c * ratio, 0.0, 1.0);\n"
    "  }\n"
    "  return c;\n"
    "}\n"
    "vec3 gradeApply(vec3 c) {\n"
    "  float ev = exp2(uGradeExposure / 100.0);\n"
    "  float w = 1.0 + max(uGradeWhite - 100.0, 0.0) / 100.0 * 3.0;\n"
    "  vec3 tc = gradeTonemapOp(max(c, 0.0) * ev);\n"
    "  tc = tc / max(gradeTonemapOp(vec3(w)), vec3(0.0001));\n"
    "  vec3 looked = gradeLookColour(clamp(tc, 0.0, 1.0));\n"
    "  c = mix(tc, looked, clamp(uGradeStrength / 100.0, 0.0, 1.0));\n"
    "  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "  c = mix(vec3(l), c, uGradeSat / 100.0);\n"
    "  c = (c - 0.5) * exp2(uGradeContrast / 100.0) + 0.5;\n"
    "  float t = uGradeTint / 100.0;\n"
    "  c *= vec3(1.0 + 0.22 * t, 1.0 - 0.06 * abs(t), 1.0 - 0.22 * t);\n"
    "  return clamp(c, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  vec3 baseColor = texture2D(uColor, vUV).rgb;\n"
    "  if (uDenoisePath > 0.5 && uDenoiseOn > 0.5) {\n"
    "    float cz = linZ(texture2D(uDepth, vUV).r);\n"
    "    vec3 dc = baseColor;\n"
    "    float stepPx = max(uDenoiseRadius, 1.0);\n"
    "    for (int p = 0; p < 3; p++) {\n"
    "      if (float(p) >= uDenoisePasses) break;\n"
    "      dc = denoiseTap(vUV, cz, dc, stepPx);\n"
    "      stepPx *= 2.0;\n"
    "    }\n"
    "    float acc = 1.0 / max(uDenoiseFrames, 1.0);\n"
    "    float a = clamp((uDenoiseIntensity / 100.0) * (0.45 + 0.55 * acc), 0.0, 1.0);\n"
    "    baseColor = mix(baseColor, dc, a);\n"
    "  }\n"
    "  vec2 cuv = canvasUV(vUV);\n"
    "  if (uDebug < 0.5 && (cuv.x < 0.0 || cuv.x > 1.0 || cuv.y < 0.0 || cuv.y > 1.0)) {\n"
    "    gl_FragColor = vec4(baseColor, 1.0);\n"
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
    "  vec3 color = baseColor;\n"
    "  vec2 mvPx = vec2(0.0);\n"
    "  if (uVelOn > 0.5) {\n"
    "    vec4 e = texture2D(uVelocity, vUV);\n"
    "    vec2 d = vec2(e.r * 255.0 * 256.0 + e.g * 255.0,\n"
    "                  e.b * 255.0 * 256.0 + e.a * 255.0) / 65535.0;\n"
    "    mvPx = (d * 2.0 - 1.0) * uRes;\n"
    "  } else {\n"
    "    vec3 pvn = worldToViewM(Pw, uPrevCamInv);\n"
    "    mvPx = (viewToNdc(worldToView(Pw)) - viewToNdc(pvn)) * 0.5 * uRes;\n"
    "    mvPx *= uVelScale;\n"
    "  }\n"
    "  float mvLen = length(mvPx);\n"
    "  if (uVelMaxOn > 0.5) {\n"
    "    vec2 base = floor(vUV * uVelMaxGrid);\n"
    "    vec2 tc0 = clamp(base, vec2(0.0), uVelMaxGrid - 1.0) + 0.5;\n"
    "    float bound = texture2D(uVelMax, tc0 / uVelMaxGrid).r;\n"
    "    if (uVelMaxMode > 1.5) {\n"
    "      for (int oy = -1; oy <= 1; oy++) {\n"
    "        for (int ox = -1; ox <= 1; ox++) {\n"
    "          vec2 tc = clamp(base + vec2(float(ox), float(oy)), vec2(0.0),\n"
    "                          uVelMaxGrid - 1.0) + 0.5;\n"
    "          bound = max(bound, texture2D(uVelMax, tc / uVelMaxGrid).r);\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    float boundPx = bound * max(uRes.x, uRes.y);\n"
    "    if (mvLen > boundPx) mvPx *= max(boundPx, 1.0) / max(mvLen, 0.001);\n"
    "  }\n"
    "  mvLen = length(mvPx);\n"
    "  if (mvLen > uMBClampPx) mvPx *= uMBClampPx / mvLen;\n"
    "  if (uMBOn > 0.5) {\n"
    "    vec2 v = mvPx / uRes;\n"
    "    float shutter = uMBShutter / 360.0;\n"
    "    float px = length(v * uRes) * shutter * (uMBStrength / 100.0);\n"
    "    if (px > 0.4) {\n"
    "      float ns = clamp(uMBSubSteps, 1.0, 16.0);\n"
    "      float jit = (uMBJitter / 100.0) * fract(sin(dot(vUV * uRes,\n"
    "                  vec2(12.9898, 78.233))) * 43758.5453 + uFrame * 0.618);\n"
    "      vec3 acc = baseColor;\n"
    "      float ws = 1.0;\n"
    "      for (int i = 0; i < 16; i++) {\n"
    "        if (float(i) >= ns) break;\n"
    "        float t = ns > 1.5 ? ((float(i) + jit) / (ns - 1.0) - 0.5) : 0.0;\n"
    "        vec2 suv = vUV + v * t * shutter * (uMBStrength / 100.0);\n"
    "        vec2 sc = clamp(suv, vec2(0.0), vec2(1.0));\n"
    "        float sd = linZ(texture2D(uDepth, sc).r);\n"
    "        float rel = abs(sd - z) / max(z, 1.0);\n"
    "        float w = rel > (uMBDepthReject / 100.0) * 2.0 ? 0.0 : 1.0;\n"
    "        acc += texture2D(uColor, suv).rgb * w;\n"
    "        ws += w;\n"
    "      }\n"
    "      color = acc / max(ws, 1.0);\n"
    "    }\n"
    "  }\n"
    "  float shadow = 1.0;\n"
    "  if (uShadowOn > 0.5) shadow = shadowLookup(Pw, Nw);\n"
    "  vec3 refl = vec3(0.0);\n"
    "  if (uSSRTXOn > 0.5) refl = ssrtxReflect(P, N, cuv);\n"
    "  float ao = 1.0;\n"
    "  if (uAOOn > 0.5 && uAOProbe < 0.5) {\n"
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
    "  vec3 lightAcc = vec3(0.0);\n"
    "  vec3 specAcc = vec3(0.0);\n"
    "  vec3 camPos = uCamInv[3].xyz;\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    if (float(i) >= uLightCount) break;\n"
    "    vec3 Lw = uLightPos[i] - Pw;\n"
    "    float dl = length(Lw);\n"
    "    if (dl < 0.001 || dl > uLightRadius[i]) continue;\n"
    "    vec3 L = Lw / dl;\n"
    "    float att = 1.0 - dl / max(uLightRadius[i], 1.0);\n"
    "    att *= att;\n"
    "    float vis = uLightShadows > 0.5 ? lightVis(P, uLightPosView[i] - P) : 1.0;\n"
    "    float ndl = max(dot(Nw, L), 0.0);\n"
    "    lightAcc += uLightColor[i] * ndl * att * vis;\n"
    "    vec3 Vd = normalize(camPos - Pw);\n"
    "    vec3 H = normalize(L + Vd);\n"
    "    specAcc += uLightColor[i] * pow(max(dot(Nw, H), 0.0), 32.0) * att * vis;\n"
    "  }\n"
    "  vec3 dofColor = color;\n"
    "  float cocPx = 0.0;\n"
    "  if (uDOFOn > 0.5 || (uDofPre > 0.5 && uDebug > 1.5)) {\n"
    "    float coc = abs(z - uFocus) / max(z, 1.0);\n"
    "    cocPx = clamp(coc * (uAperture / 100.0) * 220.0, 0.0, uMaxBlur);\n"
    "    if (uDOFOn > 0.5 && cocPx > 0.5) {\n"
    "      vec3 acc = vec3(0.0);\n"
    "      float ws = 0.0;\n"
    "      float ns = max(uDOFSamples, 1.0);\n"
    "      float blades = max(uBokehBlades, 3.0);\n"
    "      float segA = 6.2831853 / blades;\n"
    "      float edge = smoothstep(0.15, 1.0, abs(vUV.x * 2.0 - 1.0));\n"
    "      for (int i = 0; i < 32; i++) {\n"
    "        if (float(i) >= ns) break;\n"
    "        float fi = (float(i) + 0.5) / ns;\n"
    "        float ang = float(i) * 2.39996323 + radians(uBokehRot);\n"
    "        vec2 p = vec2(cos(ang), sin(ang)) * sqrt(fi);\n"
    "        if (uBokehOn > 0.5) {\n"
    "          p.x *= (uBokehAspect / 100.0) * (uAnamorphic / 100.0);\n"
    "          p.x *= 1.0 - (uCatEye / 100.0) * 0.65 * edge;\n"
    "          float aa = mod(atan(p.y, p.x) + segA * 0.5, segA) - segA * 0.5;\n"
    "          float rpoly = cos(segA * 0.5) / max(cos(aa), 0.001);\n"
    "          if (length(p) > rpoly) continue;\n"
    "        }\n"
    "        vec2 suv = vUV + p * cocPx / uRes;\n"
    "        vec2 scuv = canvasUV(suv);\n"
    "        if (scuv.x < 0.0 || scuv.x > 1.0 || scuv.y < 0.0 || scuv.y > 1.0) continue;\n"
    "        vec3 sc = texture2D(uColor, suv).rgb;\n"
    "        float sz = linZ(texture2D(uDepth, suv).r);\n"
    "        float scoc = abs(sz - uFocus) / max(sz, 1.0) * (uAperture / 100.0) * 220.0;\n"
    "        float w = (sz < z - 1.0 && scoc < 1.0) ? 0.15 : 1.0;\n"
    "        float lum = dot(sc, vec3(0.299, 0.587, 0.114));\n"
    "        w *= 1.0 + (uBokehHighlight / 100.0) * lum * lum * 2.0;\n"
    "        acc += sc * w; ws += w;\n"
    "      }\n"
    "      dofColor = ws > 0.0 ? acc / ws : color;\n"
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
    "  if (uRayOn > 0.5 && uRayDebug > 1.5) gl_FragColor = vec4(vec3(texture2D(uRayTex, vUV).r), 1.0);\n"
    "  else if (uRayOn > 0.5 && uRayDebug > 0.5) {\n"
    "    vec2 rd = (vUV - uRayLightUV) * vec2(uAspect, 1.0);\n"
    "    float ramp = 1.0 / (1.0 + 4.0 * dot(rd, rd));\n"
    "    gl_FragColor = vec4(texture2D(uRayTex, vUV).rgb * uRayColor * ramp * 3.0, 1.0);\n"
    "  }\n"
    "  else if (uShadowDebug > 1.5) gl_FragColor = vec4(vec3(unpackDepth(texture2D(uShadowMap, clamp(cuv, 0.0, 1.0)))), 1.0);\n"
    "  else if (uShadowDebug > 0.5) gl_FragColor = vec4(vec3(shadow), 1.0);\n"
    "  else if (uDebug > 8.5) {\n"
    "    float tcol = sqrt(clamp(mvLen / max(uVecGain, 1.0), 0.0, 1.0));\n"
    "    if (uVecStyle > 1.5) {\n"
    "      vec3 hc = vec3(0.0);\n"
    "      vec2 seg = mvPx;\n"
    "      if (mvLen > 26.0) seg = seg * (26.0 / mvLen);\n"
    "      for (int oy = -2; oy <= 2; oy++) {\n"
    "        for (int ox = -2; ox <= 2; ox++) {\n"
    "          vec2 a = vec2(float(ox), float(oy));\n"
    "          float tt = clamp(dot(-a, seg) / max(dot(seg, seg), 0.001), 0.0, 1.0);\n"
    "          float dist = length(a + seg * tt);\n"
    "          if (dist < 0.85 && mvLen > 0.5) hc = max(hc, heatColour(tcol));\n"
    "        }\n"
    "      }\n"
    "      gl_FragColor = vec4(hc, 1.0);\n"
    "    } else if (uVecStyle > 0.5) {\n"
    "      float ang = atan(mvPx.y, mvPx.x) / 6.2831853 + 0.5;\n"
    "      vec3 k = mod(vec3(5.0, 3.0, 1.0) + ang * 6.0, 6.0);\n"
    "      vec3 hue = 1.0 - clamp(min(k, 4.0 - k), 0.0, 1.0);\n"
    "      gl_FragColor = vec4(hue * clamp(tcol * 1.4, 0.0, 1.0), 1.0);\n"
    "    } else {\n"
    "      gl_FragColor = vec4(heatColour(tcol), 1.0);\n"
    "    }\n"
    "  }\n"
    "  else if (uDebug > 7.5) gl_FragColor = vec4(vec3(clamp(length(uLightPos[0] - Pw) / 3000.0, 0.0, 1.0)), 1.0);\n"
    "  else if (uDebug > 6.5) gl_FragColor = vec4(vec3(uProbeVis), 1.0);\n"
    "  else if (uDebug > 5.5) gl_FragColor = vec4(lightAcc, 1.0);\n"
    "  else if (uDebug > 4.5) gl_FragColor = vec4(vec3(uLightCount / 4.0), 1.0);\n"
    "  else if (uDebug > 3.5) gl_FragColor = vec4(refl * 3.0, 1.0);\n"
    "  else if (uDebug > 2.5) gl_FragColor = vec4(gi * 3.0 * (uGIIntensity / 100.0), 1.0);\n"
    "  else if (uDebug > 1.5) gl_FragColor = vec4(vec3(clamp(cocPx / max(uMaxBlur, 1.0), 0.0, 1.0)), 1.0);\n"
    "  else if (uDebug > 0.5) gl_FragColor = vec4(vec3(ao), 1.0);\n"
    "  else {\n"
    "    vec3 outc = dofColor * ao * shadow + gi * (uGIIntensity / 100.0) * 0.6\n"
    "              + refl * mix(0.35, 1.0, ao);\n"
    "    outc += (lightAcc + specAcc * 0.6) * (uLightIntensity / 100.0);\n"
    // Crepuscular rays: additive light, like the dynamic lights above, after
    // AO/DOF/shadow have shaped the base colour.  There is no tonemap or colour
    // grade in this pass, so this is the last point where light is added.
    "    if (uRayOn > 0.5) {\n"
    "      vec2 rd = (vUV - uRayLightUV) * vec2(uAspect, 1.0);\n"
    "      float ramp = 1.0 / (1.0 + 4.0 * dot(rd, rd));\n"
    "      outc += texture2D(uRayTex, vUV).rgb * uRayColor * (uRayIntensity / 100.0) * ramp;\n"
    "    }\n"
    "    if (uProbeVis > 0.5) {\n"
    "      float worldR = max(uProbeRadiusW, 1.0);\n"
    "      for (int pi = 0; pi < 16; pi++) {\n"
    "        if (float(pi) >= uProbeCount) break;\n"
    "        vec3 pp = uProbePos[pi];\n"
    "        vec3 state = uProbeCol[pi];\n"
    "        vec3 pv = worldToView(pp);\n"
    "        if (pv.z > -1.0) continue;\n"
    "        vec2 pcuv = viewToCanvas(pv);\n"
    "        vec2 puv = eyeUVfromCanvas(pcuv);\n"
    "        vec2 dpx = (puv - vUV) * uRes;\n"
    "        float rpx = worldR * (uRes.y * 0.5 / uTanHalf) / max(-pv.z, 1.0);\n"
    "        if (rpx < 2.0) continue;\n"
    "        float r2 = dot(dpx, dpx) / (rpx * rpx);\n"
    "        if (r2 > 1.0) continue;\n"
    "        vec3 n = normalize(vec3(dpx / rpx, sqrt(max(1.0 - r2, 0.0))));\n"
    "        vec3 viewDir = normalize(Pw - pp);\n"
    "        vec3 reflDir = reflect(viewDir, n);\n"
    "        vec3 env = probeEnv(reflDir, state);\n"
    "        float fres = 0.15 + 0.85 * pow(1.0 - clamp(dot(-viewDir, n), 0.0, 1.0), 3.0);\n"
    "        float spec = pow(max(dot(reflect(-viewDir, n), normalize(vec3(0.4, 0.8, 0.3))), 0.0), 8.0 + max(uProbeRes, 1.0) * 3.0);\n"
    "        outc = mix(outc, env * fres + spec * 1.5 + state * 0.2, 0.92);\n"
    "      }\n"
    "    }\n"
    // Tone mapping + grading: the last thing that happens to the scene colour,
    // after every additive term (lights, rays, probes) is in and after the
    // debug-view chain above, so debug views still show raw linear buffers.
    // One uniform branch when the tab is off.
    "    if (uGradeOn > 0.5) {\n"
    "      vec3 preGrade = clamp(outc, 0.0, 1.0);\n"
    "      vec3 graded = gradeApply(outc);\n"
    "      if (uGradeDebug > 0.5) {\n"
    "        outc = cuv.x < 0.5 ? preGrade : graded;\n"
    "        if (abs(cuv.x - 0.5) * max(uCanvas.z, 1.0) < 1.5) outc = vec3(1.0, 0.0, 1.0);\n"
    "      } else {\n"
    "        outc = graded;\n"
    "      }\n"
    "    }\n"
    "    gl_FragColor = vec4(outc, 1.0);\n"
    "  }\n"
    "}\n";

/* ---------------- Reduced-resolution denoise prepass ---------------------
 * Runs before the per-eye FX pass when the alternative path has the denoiser
 * on and the resolution divisor is > 1 (DENOISER-RESEARCH.md §4.1):
 *
 *   mode 0  downsample scene colour  -> RGBA8 low-res colour   (ping)
 *   mode 1  downsample depth         -> RGBA8 low-res packed z (8.8 fixed)
 *   mode 2  one edge-aware à-trous pass over the low-res colour, steered by
 *           the low-res depth (5x5 B3 kernel, 25 taps)
 *   mode 3  bilateral (depth-aware) upsample back to full resolution
 *
 * The FX shader then only sees mode 3's mixed result as its scene colour
 * (uColor); doing the 4-tap upsample inside the big FX shader cost far more
 * on this GPU than a dedicated pass, and an extra sampler there slowed the
 * whole post chain down.  All of it is skipped on the MAIN path and when
 * divisor == 1, which keeps the original full-resolution denoiseTap()
 * in-shader path available as the A/B.
 */
static const char *s_dn_vs =
    "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *s_dn_fs =
    "precision highp float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uSrc;\n"        /* full-res eye colour      (mode 0) */
    "uniform sampler2D uSrcDepth;\n"   /* full-res eye depth       (mode 1) */
    "uniform sampler2D uLowCol;\n"     /* low-res colour in        (mode 2) */
    "uniform sampler2D uLowDep;\n"     /* low-res packed z         (mode 2) */
    "uniform float uMode;\n"
    "uniform vec2 uTexel;\n"           /* 1 / low-res size (UV texel) */
    "uniform float uStep;\n"           /* à-trous step, low-res texels */
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "uniform float uDepthSigma;\n"
    "uniform float uColourSigma;\n"
    "uniform float uUpSigma;\n"
    "uniform float uMixA;\n"
    "float linZ(float d) {\n"
    "  float z = d * 2.0 - 1.0;\n"
    "  return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));\n"
    "}\n"
    "vec2 packZ(float z) {\n"
    "  float v = clamp(z / uFar, 0.0, 1.0) * 65025.0;\n"
    "  float hi = floor(v / 255.0);\n"
    "  float lo = v - hi * 255.0;\n"
    "  return vec2(hi / 255.0, lo / 255.0);\n"
    "}\n"
    "float unpackZ(vec4 c) {\n"
    "  return ((c.r * 255.0) * 255.0 + c.g * 255.0) / 65025.0 * uFar;\n"
    "}\n"
    /* Binomial [1,4,6,4,1]/16 (the B3 spline sampled at integers) without
     * array indexing, which keeps the ES2 compilers happy. */
    "float b3w(float i) {\n"
    "  float a = abs(i);\n"
    "  return (a < 0.5) ? 0.375 : ((a < 1.5) ? 0.25 : 0.0625);\n"
    "}\n"
    "void main() {\n"
    "  if (uMode < 0.5) {\n"           /* colour downsample: 4 bilinear taps */
    "    vec2 o = uTexel * 0.25;\n"
    "    vec3 c = texture2D(uSrc, vUV + vec2(-o.x, -o.y)).rgb;\n"
    "    c += texture2D(uSrc, vUV + vec2( o.x, -o.y)).rgb;\n"
    "    c += texture2D(uSrc, vUV + vec2(-o.x,  o.y)).rgb;\n"
    "    c += texture2D(uSrc, vUV + vec2( o.x,  o.y)).rgb;\n"
    "    gl_FragColor = vec4(c * 0.25, 1.0);\n"
    "  } else if (uMode < 1.5) {\n"   /* depth downsample: linear z, packed */
    "    float z = linZ(texture2D(uSrcDepth, vUV).r);\n"
    "    gl_FragColor = vec4(packZ(z), 0.0, 1.0);\n"
    "  } else {\n"
    "    if (uMode < 2.5) {\n"        /* one à-trous iteration */
    "    vec3 cc = texture2D(uLowCol, vUV).rgb;\n"
    "    float cz = unpackZ(texture2D(uLowDep, vUV));\n"
    "    vec3 acc = vec3(0.0);\n"
    "    float ws = 0.0;\n"
    "    for (int y = 0; y < 5; y++) {\n"
    "      for (int x = 0; x < 5; x++) {\n"
    "        float ox = float(x) - 2.0;\n"
    "        float oy = float(y) - 2.0;\n"
    "        vec2 suv = vUV + vec2(ox, oy) * uStep * uTexel;\n"
    "        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;\n"
    "        vec3 sc = texture2D(uLowCol, suv).rgb;\n"
    "        float sz = unpackZ(texture2D(uLowDep, suv));\n"
    "        float w = b3w(ox) * b3w(oy) * exp(-abs(sz - cz) * uDepthSigma)\n"
    "                * exp(-length(sc - cc) * uColourSigma);\n"
    "        acc += sc * w;\n"
    "        ws += w;\n"
    "      }\n"
    "    }\n"
    "    gl_FragColor = vec4(ws > 0.0 ? acc / ws : cc, 1.0);\n"
    "    } else {\n"                  /* bilateral (depth-aware) upsample */
    // Four low-res taps around this pixel, weighted by their bilinear
    // coverage and by |z_tap - z_pixel|, so the upsample does not smear
    // across silhouettes.  The centre depth comes from the same low-res
    // field: at full resolution the extra depth fetches are cache misses
    // and cost an order of magnitude more than the whole à-trous chain.
    "    vec2 low = 1.0 / max(uTexel, vec2(1.0e-6));\n"
    "    vec2 p = vUV * low - 0.5;\n"
    "    vec2 f = p - floor(p);\n"
    "    vec2 base = (floor(p) + 0.5) / low;\n"
    "    float cz = unpackZ(texture2D(uLowDep, vUV));\n"
    "    vec3 acc = vec3(0.0);\n"
    "    float ws = 0.0;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      float ox = (i == 1 || i == 3) ? 1.0 : 0.0;\n"
    "      float oy = (i >= 2) ? 1.0 : 0.0;\n"
    "      vec2 suv = base + vec2(ox, oy) * uTexel;\n"
    "      vec3 sc = texture2D(uLowCol, suv).rgb;\n"
    "      float sz = unpackZ(texture2D(uLowDep, suv));\n"
    "      float bw = mix(1.0 - f.x, f.x, ox) * mix(1.0 - f.y, f.y, oy);\n"
    "      float w = bw * exp(-abs(sz - cz) * uUpSigma);\n"
    "      acc += sc * w;\n"
    "      ws += w;\n"
    "    }\n"
    "    vec3 sharp = texture2D(uSrc, vUV).rgb;\n"
    "    gl_FragColor = vec4(ws > 0.0001 ? mix(sharp, acc / ws, uMixA)\n"
    "                                     : sharp, 1.0);\n"
    "    }\n"
    "  }\n"
    "}\n";

/* ---------------- Reduced-resolution SSGI prepass -----------------------
 * The full-res in-shader GI loop costs 30.7 ms in 3D at 6x4 (findings/
 * fx-sweep-11-25-26.md §4) for a low-frequency colour bleed with a 150-unit
 * radius, so it runs here at canvas_res/div instead and is added back by its
 * own full-res pass.  Same shape as the denoise chain above (and for the same
 * reason: putting an extra sampler / the upsample into the big FX shader cost
 * ~30 ms once already).
 *
 *   mode 0  downsample scene colour  -> low-res colour
 *   mode 1  linear z + view normal   -> low-res packed z (.rg) + oct normal (.ba)
 *   mode 2  the ray/tap gather       -> low-res GI colour
 *   mode 3  depth-aware 3x3 blur     -> low-res GI colour (ping-pong)
 *   mode 4  bilateral upsample, added into the FX output with GL_ONE/GL_ONE
 *   mode 5  debug view of the low-res buffer (replace, no blend)
 *
 * All offsets inside mode 2 are in canvas UV, i.e. derived from the NDC offset
 * the full-res path uses, so the 150-unit radius stays 150 world units at every
 * divisor: only the number of pixels changes, not the footprint.  Modes 0/1/2/3
 * run at 1/div^2 the pixels, so a rotated-grid + per-frame jitter offset and a
 * blur pass buy back the sample count the resolution took away.
 */
static const char *s_gi_vs =
    "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *s_gi_fs =
    "precision highp float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uColor;\n"      /* full-res eye colour          (mode 0) */
    "uniform sampler2D uDepth;\n"      /* full-res eye depth       (0,1,4,5) */
    "uniform sampler2D uCol;\n"        /* low-res scene colour         (mode 2) */
    "uniform sampler2D uDep;\n"        /* low-res packed z + normal (2,3,4,5) */
    "uniform sampler2D uSrcG;\n"       /* low-res GI in                (mode 3) */
    "uniform vec2 uRes;\n"             /* full-res eye size */
    "uniform vec2 uLow;\n"             /* low-res canvas size */
    "uniform vec4 uCanvas;\n"          /* canvas x,y,w,h in eye pixels */
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "uniform float uTanHalf;\n"
    "uniform float uAspect;\n"
    "uniform float uMode;\n"
    "uniform float uRadius;\n"
    "uniform float uSteps;\n"
    "uniform float uRays;\n"
    "uniform float uJitter;\n"
    "uniform float uFrame;\n"
    "uniform float uSkip;\n"           /* far/sky early-out, percent of uFar */
    "uniform float uNorm;\n"           /* normal-aware tap reject 0/1 */
    "uniform float uFlipY;\n"
    "uniform float uShear;\n"
    "uniform float uToe;\n"
    "uniform float uStep;\n"           /* blur step, low-res texels */
    "uniform float uGain;\n"           /* composite gain (intensity * 0.6 / 100) */
    "uniform float uDebug;\n"          /* 1 GI, 2 depth, 3 normals */
    "float linZ(float d) {\n"
    "  float z = d * 2.0 - 1.0;\n"
    "  return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));\n"
    "}\n"
    "vec2 packZ(float z) {\n"
    "  float v = clamp(z / uFar, 0.0, 1.0) * 65025.0;\n"
    "  float hi = floor(v / 255.0);\n"
    "  float lo = v - hi * 255.0;\n"
    "  return vec2(hi / 255.0, lo / 255.0);\n"
    "}\n"
    "float unpackZ(vec4 c) {\n"
    "  return ((c.r * 255.0) * 255.0 + c.g * 255.0) / 65025.0 * uFar;\n"
    "}\n"
    "vec2 octEnc(vec3 n) {\n"
    "  n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-5);\n"
    "  if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy + vec2(1.0e-6));\n"
    "  return n.xy * 0.5 + 0.5;\n"
    "}\n"
    "vec3 octDec(vec2 e) {\n"
    "  e = e * 2.0 - 1.0;\n"
    "  vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));\n"
    "  if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sign(n.xy + vec2(1.0e-6));\n"
    "  return normalize(n);\n"
    "}\n"
    "float hash12(vec2 p) {\n"
    "  return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "}\n"
    "vec3 viewPos(vec2 cuv, float z) {\n"
    "  vec2 ndc = cuv * 2.0 - 1.0;\n"
    "  float ndcz = (uFar + uNear - 2.0 * uNear * uFar / z) / (uFar - uNear);\n"
    "  if (uFlipY > 0.5) ndc.y = -ndc.y;\n"
    "  ndc.x -= uShear + uToe * ndcz;\n"
    "  return vec3(ndc.x * z * uTanHalf * uAspect,\n"
    "              ndc.y * z * uTanHalf, -z);\n"
    "}\n"
    "vec2 eyeUV(vec2 cuv) {\n"
    "  return (cuv * uCanvas.zw + uCanvas.xy) / uRes;\n"
    "}\n"
    "vec2 canvasUV(vec2 euv) {\n"
    "  return (euv * uRes - uCanvas.xy) / uCanvas.zw;\n"
    "}\n"
    "void main() {\n"
    /* 0: the scene colour for the gather, at low res. */
    "  if (uMode < 0.5) {\n"
    "    gl_FragColor = vec4(texture2D(uColor, eyeUV(vUV)).rgb, 1.0);\n"
    /* 1: linear depth + view-space normal, computed once per low-res texel so
     *    the gather never reconstructs a normal per tap. */
    "  } else if (uMode < 1.5) {\n"
    "    float z = linZ(texture2D(uDepth, eyeUV(vUV)).r);\n"
    "    vec2 duv = 1.0 / uLow;\n"
    "    float zx = linZ(texture2D(uDepth, eyeUV(vUV + vec2(duv.x, 0.0))).r);\n"
    "    float zy = linZ(texture2D(uDepth, eyeUV(vUV + vec2(0.0, duv.y))).r);\n"
    "    vec3 P = viewPos(vUV, z);\n"
    "    vec3 Px = viewPos(vUV + vec2(duv.x, 0.0), zx);\n"
    "    vec3 Py = viewPos(vUV + vec2(0.0, duv.y), zy);\n"
    "    vec3 N = normalize(cross(Px - P, Py - P));\n"
    "    if (N.z < 0.0) N = -N;\n"
    "    gl_FragColor = vec4(packZ(z), octEnc(N));\n"
    /* 2: the gather. Same tap window and weights as the in-shader path; the
     *    offsets are canvas-UV so the world radius is resolution independent. */
    "  } else if (uMode < 2.5) {\n"
    "    vec4 d0 = texture2D(uDep, vUV);\n"
    "    float z = unpackZ(d0);\n"
    "    vec3 selfc = texture2D(uCol, vUV).rgb;\n"
    /* Sky/far: its neighbours are sky too, so the legacy path's result is the
     * pixel's own colour.  Returning it keeps the look and skips the taps. */
    "    if (z / uFar > uSkip / 100.0) { gl_FragColor = vec4(selfc, 1.0); return; }\n"
    "    vec3 N = octDec(d0.ba);\n"
    "    float nr = max(uRays, 1.0);\n"
    "    float nst = max(uSteps, 1.0);\n"
    "    float jit = uJitter > 0.5 ? hash12(vUV * uLow + vec2(uFrame * 1.37, uFrame * 2.11)) : 0.0;\n"
    "    vec3 gi = vec3(0.0);\n"
    "    float ws = 0.0;\n"
    "    for (int r = 0; r < 8; r++) {\n"
    "      if (float(r) >= nr) break;\n"
    "      float ang = (float(r) + jit) / nr * 6.2831853;\n"
    "      vec2 dir = vec2(cos(ang), sin(ang));\n"
    "      for (int s = 1; s <= 12; s++) {\n"
    "        if (float(s) > nst) break;\n"
    "        float t = float(s) / nst;\n"
    "        vec2 ndcOff = dir * (t * uRadius) / (z * uTanHalf);\n"
    "        ndcOff.x /= uAspect;\n"
    "        vec2 cuv = vUV + ndcOff * 0.5;\n"
    "        if (cuv.x < 0.0 || cuv.x > 1.0 || cuv.y < 0.0 || cuv.y > 1.0) continue;\n"
    "        vec4 ds = texture2D(uDep, cuv);\n"
    "        float sz = unpackZ(ds);\n"
    "        float diff = sz - z;\n"
    "        if (diff > -uRadius * 0.5 && diff < uRadius * 2.0) {\n"
    /* Opposite-facing surfaces are two sides of one silhouette; bleeding
     * between them is the classic half-res GI artefact, so reject them. */
    "          if (uNorm > 0.5 && dot(N, octDec(ds.ba)) < -0.2) continue;\n"
    "          vec3 sc = texture2D(uCol, cuv).rgb;\n"
    "          float dw = exp(-abs(diff) / (uRadius + 1.0));\n"
    "          float tw = 1.0 - t;\n"
    "          gi += sc * dw * tw;\n"
    "          ws += dw * tw;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    gl_FragColor = vec4(ws > 0.0 ? gi / ws : vec3(0.0), 1.0);\n"
    /* 3: edge-aware 3x3 blur over the low-res GI buffer. */
    "  } else if (uMode < 3.5) {\n"
    "    float z = unpackZ(texture2D(uDep, vUV));\n"
    "    vec3 acc = texture2D(uSrcG, vUV).rgb;\n"
    "    float wsum = 1.0;\n"
    "    for (int k = 0; k < 9; k++) {\n"
    "      if (k == 4) continue;\n"
    /* GLES2 forbids integer '%' outside pack/unpack shaders on this driver
     * ("ERROR: '%' : supported in pack/unpack shaders only"), so the 3x3
     * offsets are spelled out instead of k%3 / k/3. */
    "      int dx = (k == 2 || k == 5 || k == 8) ? 1\n"
    "             : ((k == 0 || k == 3 || k == 6) ? -1 : 0);\n"
    "      int dy = (k >= 6) ? 1 : ((k <= 2) ? -1 : 0);\n"
    "      vec2 cuv = vUV + vec2(float(dx), float(dy)) * uStep / uLow;\n"
    "      if (cuv.x < 0.0 || cuv.x > 1.0 || cuv.y < 0.0 || cuv.y > 1.0) continue;\n"
    "      float sz = unpackZ(texture2D(uDep, cuv));\n"
    "      float w = exp(-abs(sz - z) * (1.5 / max(uRadius, 1.0)));\n"
    "      acc += texture2D(uSrcG, cuv).rgb * w;\n"
    "      wsum += w;\n"
    "    }\n"
    "    gl_FragColor = vec4(acc / wsum, 1.0);\n"
    /* 4: bilateral upsample, drawn into the FX output with additive blending.
     *    The full-res depth is the edge stop, so a low-res texel from the
     *    background cannot bleed across a silhouette. */
    "  } else if (uMode < 4.5) {\n"
    "    vec2 cuv = canvasUV(vUV);\n"
    "    if (cuv.x < 0.0 || cuv.x > 1.0 || cuv.y < 0.0 || cuv.y > 1.0) {\n"
    "      gl_FragColor = vec4(0.0);\n"
    "      return;\n"
    "    }\n"
    "    float z = linZ(texture2D(uDepth, vUV).r);\n"
    "    vec2 base = cuv * uLow - 0.5;\n"
    "    vec2 f = base - floor(base);\n"
    "    vec2 i0 = floor(base);\n"
    "    vec3 acc = vec3(0.0);\n"
    "    float wsum = 0.0;\n"
    "    float sig = 2.0 / max(uRadius, 1.0);\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "      float ox = (k == 1 || k == 3) ? 1.0 : 0.0;\n"
    "      float oy = (k >= 2) ? 1.0 : 0.0;\n"
    "      vec2 tc = clamp((i0 + vec2(ox, oy) + 0.5) / uLow, vec2(0.0), vec2(1.0));\n"
    "      float sz = unpackZ(texture2D(uDep, tc));\n"
    "      vec3 sc = texture2D(uSrcG, tc).rgb;\n"
    "      float bw = mix(1.0 - f.x, f.x, ox) * mix(1.0 - f.y, f.y, oy);\n"
    "      float w = bw * exp(-abs(sz - z) * sig);\n"
    "      acc += sc * w;\n"
    "      wsum += w;\n"
    "    }\n"
    "    gl_FragColor = vec4(wsum > 0.0001 ? acc / wsum * uGain : vec3(0.0), 1.0);\n"
    /* 5: debug view of the low-res buffers (replace, gain applied). */
    "  } else {\n"
    "    vec2 cuv = clamp(canvasUV(vUV), vec2(0.0), vec2(1.0));\n"
    "    vec4 d = texture2D(uDep, cuv);\n"
    "    vec3 c;\n"
    "    if (uDebug < 1.5) c = texture2D(uSrcG, cuv).rgb * 3.0;\n"
    "    else if (uDebug < 2.5) c = vec3(unpackZ(d) / uFar);\n"
    "    else c = octDec(d.ba) * 0.5 + 0.5;\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "  }\n"
    "}\n";

/* ---------------- Crepuscular rays (screen-space light shafts) -----------
 * Quarter-resolution, per eye, additive.  Runs as its own small program
 * before the FX pass and hands the FX shader a blurred visibility buffer:
 *
 *   mode 0  march from this pixel toward the light's screen position, reading
 *           the eye depth buffer; depth == far (sky) counts as unoccluded,
 *           an occluder is anything closer than z0 * (1 - bias)
 *   mode 1  horizontal 9-tap gaussian at quarter res
 *   mode 2  vertical 9-tap gaussian at quarter res
 *
 * The FX pass samples the result bilinearly and adds it as light.  Nothing is
 * allocated or executed when the effect is off, so the main path is untouched.
 */
static const char *s_ray_fs =
    "precision highp float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uSrcDepth;\n"
    "uniform sampler2D uSrcCol;\n"
    "uniform sampler2D uSrcRay;\n"
    "uniform float uMode;\n"
    "uniform vec2 uTexel;\n"
    "uniform vec2 uDir;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "uniform vec2 uLightUV;\n"
    "uniform float uSamples;\n"
    "uniform float uDensity;\n"
    "uniform float uDecay;\n"
    "uniform float uWeight;\n"
    "uniform float uThreshold;\n"
    "uniform float uBias;\n"
    "uniform float uJitter;\n"
    "uniform float uFrame;\n"
    "uniform vec2 uRes;\n"
    "float linZ(float d) {\n"
    "  float z = d * 2.0 - 1.0;\n"
    "  return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));\n"
    "}\n"
    /* Interleaved gradient noise: cheap per-pixel start offset against banding. */
    "float ign(vec2 p) {\n"
    "  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));\n"
    "}\n"
    "void main() {\n"
    "  if (uMode < 0.5) {\n"
    "    float z0 = linZ(texture2D(uSrcDepth, vUV).r);\n"
    "    vec2 luv = clamp(uLightUV, vec2(0.0), vec2(1.0));\n"
    "    float n = max(uSamples, 1.0);\n"
    "    float jit = uJitter * ign(vUV * uRes + vec2(uFrame * 7.31, uFrame * 3.17));\n"
    "    float w = 1.0;\n"
    "    float acc = 0.0;\n"
    "    float ws = 0.0;\n"
    "    for (int i = 0; i < 32; i++) {\n"
    "      if (float(i) >= n) break;\n"
    "      float t = (float(i) + jit) / n;\n"
    "      t = min(t * uDensity, 1.0);\n"
    "      vec2 p = mix(vUV, luv, t);\n"
    "      float zs = linZ(texture2D(uSrcDepth, p).r);\n"
    /* Sky samples are always unoccluded (far >= z0*(1-bias) even for a sky
     * pixel); geometry closer than the pixel's own depth plane occludes. */
    "      float vis = (zs >= z0 * (1.0 - uBias)) ? 1.0 : 0.0;\n"
    "      acc += vis * w;\n"
    "      ws += w;\n"
    "      w *= uDecay;\n"
    "    }\n"
    "    float shaft = ws > 0.0001 ? acc / ws : 0.0;\n"
    "    float lum = dot(texture2D(uSrcCol, vUV).rgb, vec3(0.299, 0.587, 0.114));\n"
    "    shaft *= smoothstep(uThreshold, uThreshold + 0.20, lum);\n"
    "    gl_FragColor = vec4(vec3(shaft * uWeight), 1.0);\n"
    "  } else {\n"
    "    float w0 = 1.0, w1 = 0.8825, w2 = 0.6065, w3 = 0.3247, w4 = 0.1353;\n"
    "    float wsum = w0 + 2.0 * (w1 + w2 + w3 + w4);\n"
    "    vec3 acc = texture2D(uSrcRay, vUV).rgb * w0;\n"
    "    vec2 d1 = uDir * uTexel;\n"
    "    acc += texture2D(uSrcRay, vUV + d1).rgb * w1;\n"
    "    acc += texture2D(uSrcRay, vUV - d1).rgb * w1;\n"
    "    acc += texture2D(uSrcRay, vUV + d1 * 2.0).rgb * w2;\n"
    "    acc += texture2D(uSrcRay, vUV - d1 * 2.0).rgb * w2;\n"
    "    acc += texture2D(uSrcRay, vUV + d1 * 3.0).rgb * w3;\n"
    "    acc += texture2D(uSrcRay, vUV - d1 * 3.0).rgb * w3;\n"
    "    acc += texture2D(uSrcRay, vUV + d1 * 4.0).rgb * w4;\n"
    "    acc += texture2D(uSrcRay, vUV - d1 * 4.0).rgb * w4;\n"
    "    gl_FragColor = vec4(acc / wsum, 1.0);\n"
    "  }\n"
    "}\n";

/* March + blur dials (headless keys 250-260).  Off by default; the defaults
 * are tuned so `250 1` alone shows shafts from the synthetic sun. */
static int s_ray_on = 0;            /* 250 */
static int s_ray_intensity = 100;   /* 251: additive contribution % */
static int s_ray_density = 100;     /* 252: march length toward the light % */
static int s_ray_decay = 96;        /* 253: per-sample weight falloff % */
static int s_ray_weight = 60;       /* 254: per-sample accumulation weight % */
static int s_ray_samples = 16;      /* 255: 8-32 march samples */
static int s_ray_threshold = 20;    /* 256: scene-luminance gate % */
static int s_ray_light = 4;         /* 257: 0-3 dynamic light, 4 = sun */
static int s_ray_jitter = 100;      /* 258: start-offset jitter % */
static int s_ray_debug = 0;         /* 259: 0 normal, 1 shafts, 2 visibility */
static int s_ray_bias = 10;         /* 260: depth-plane occlusion tolerance % */

static GLuint s_ray_prog = 0;
static int    s_ray_prog_state = 0;   // 0 = untried, 1 = ok, -1 = failed
static GLint  s_ray_apos = -1, s_ray_auv = -1;
static struct {
    GLint mode, src_depth, src_col, src_ray;
    GLint texel, dir, nearv, farv;
    GLint light_uv;
    GLint samples, density, decay, weight, threshold, bias, jitter, frame;
    GLint res;
} s_ray_u;
/* [eye][0 = march/raw, 1 = horizontal blur, 2 = vertical blur] */
static GLuint s_ray_fbo[2][3] = {{0, 0, 0}, {0, 0, 0}};
static GLuint s_ray_tex[2][3] = {{0, 0, 0}, {0, 0, 0}};
static int    s_ray_w = 0, s_ray_h = 0;
static GLuint s_ray_result[2] = {0, 0};   // per-eye texture for the FX pass
static int    s_ray_valid[2] = {0, 0};
static float  s_ray_lightuv[2][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}};

/* Reduced-resolution SSGI targets: [eye][0 = low colour, 1 = packed z+normal,
 * 2/3 = GI ping-pong].  Sized in canvas pixels / div, never allocated when the
 * chain is off. */
static GLuint s_gi_prog = 0;
static int    s_gi_prog_state = 0;    // 0 = untried, 1 = ok, -1 = failed
static GLint  s_gi_apos = -1, s_gi_auv = -1;
static struct {
    GLint color, depth, col, dep, srcg;
    GLint res, low, canvas, nearv, farv, tanhalf, aspect;
    GLint mode, radius, steps, rays, jitter, frame, skip, norm;
    GLint flipy, shear, toe, step, gain, debug;
} s_gi_u;
static GLuint s_gi_fbo[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
static GLuint s_gi_tex[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
static int    s_gi_w = 0, s_gi_h = 0, s_gi_div = 0;
static int    s_gi_src[2] = {0, 0};   // last blur stage per eye
static int    s_gi_valid[2] = {0, 0};



static GLuint s_dn_prog = 0;
static int    s_dn_prog_state = 0;   // 0 = untried, 1 = ok, -1 = failed
static GLint  s_dn_apos = -1, s_dn_auv = -1;
static struct {
    GLint mode, src, src_depth, low_col, low_dep;
    GLint texel, step, nearv, farv, depth_sigma, colour_sigma, up_sigma;
    GLint mix_a;
} s_dn_u;
/* [eye][ping-pong stage] colour targets + one packed-depth target per eye. */
static GLuint s_dn_fbo[2][2] = {{0, 0}, {0, 0}};
static GLuint s_dn_col[2][2] = {{0, 0}, {0, 0}};
static GLuint s_dn_dfbo[2] = {0, 0};
static GLuint s_dn_dep[2] = {0, 0};
/* Full-res bilateral upsample result (one texture per eye) + its FBO. */
static GLuint s_dn_up[2] = {0, 0};
static GLuint s_dn_ufbo[2] = {0, 0};
static int    s_dn_w = 0, s_dn_h = 0, s_dn_div = 0;
static int    s_dn_upw = 0, s_dn_uph = 0;
static GLuint s_dn_result[2] = {0, 0};   // per-eye colour for the FX pass
static int    s_dn_valid[2] = {0, 0};
static int    s_dn_src[2] = {0, 0};      // last à-trous stage per eye

static struct {
    GLint color, depth, res, canvas, nearv, farv, tanhalf, aspect, frame;
    GLint ao_on, ao_intensity, ao_radius, ao_samples, ao_bias, ao_power, ao_probe;
    GLint dof_on, dof_focus, dof_aperture, dof_maxblur, dof_samples, dof_pre;
    GLint gi_on, gi_intensity, gi_radius, gi_steps, gi_rays;
    GLint debug, normal_flip;
    GLint shear, toe, flipy;
    GLint shadow_map, shadow_vp, shadow_caminv, shadow_dir, shadow_texel;
    GLint shadow_on, shadow_strength, shadow_bias, shadow_nbias;
    GLint shadow_softness, shadow_taps, shadow_debug;
    GLint ssrtx_on, ssrtx_intensity, ssrtx_dist, ssrtx_steps;
    GLint ssrtx_rays, ssrtx_thick, ssrtx_rough, ssrtx_fade;
    GLint mb_on, mb_strength, mb_samples, cur_vp, prev_vp, prev_cam_inv;
    GLint velocity, vel_on, mb_shutter, mb_substeps, mb_maxpx, mb_jitter;
    GLint vel_scale;
    GLint vel_max, vel_max_on, vel_max_grid;
    GLint mb_depth_reject, mb_clamp_px, vec_style, velmax_mode;
    GLint light_count, light_pos, light_pos_view, light_color, light_radius;
    GLint light_intensity, light_shadows, light_shadow_steps;
    GLint probe_vis, probe_center, probe_spacing, probe_dim, probe_radius, probe_res;
    GLint bokeh_on, bokeh_aspect, bokeh_blades, bokeh_rot, anamorphic, bokeh_highlight, cat_eye;
    GLint probe_pos, probe_col, probe_count;
    GLint denoise_path, denoise_on, denoise_intensity, denoise_passes;
    GLint denoise_radius, denoise_frames, denoise_bounces;
    GLint vec_gain;
    GLint ray_tex, ray_on, ray_intensity, ray_debug, ray_color, ray_light_uv;
    GLint grade_on, grade_op, grade_exposure, grade_white, grade_look;
    GLint grade_strength, grade_sat, grade_contrast, grade_tint, grade_debug;
} s_fx_u;

static GLuint s_fx_prog = 0;
static int    s_fx_prog_state = 0;   // 0 = untried, 1 = ok, -1 = failed
static GLuint s_pp_fbo[2] = {0, 0};
static GLuint s_pp_tex[2] = {0, 0};
static int    s_pp_w = 0, s_pp_h = 0;

/* Optional 2D stripe LUT for the GRADE look.  GLES2 has no 3D textures, so the
 * expected image is a horizontal strip of 16x16 blue slices: width 16*N,
 * height 16, red across a tile, green down it, blue picks the tile:
 *
 *      256x16 = 16 slices, 512x16 = 32 slices (also 32x16 / 64x16 / ...)
 *
 * Drop it at <external files>/luts/grade.png
 * (= /sdcard/Android/data/com.vdavid003.sm64port/files/luts/grade.png) and
 * select look 6; with no (valid) file the analytic looks are used instead.
 * Loading happens on the GL thread from the pass, because gfx_opengl_ssfx_set
 * is called from the UI thread and must not touch GL objects. */
static GLuint s_lut_tex = 0;
static int    s_lut_valid = 0;
static int    s_lut_slices = 16;
static int    s_lut_w = 0, s_lut_h = 0;
static GLuint s_lut_prog = 0;
static int    s_lut_prog_state = 0;
static GLuint s_lut_fbo[2] = {0, 0};
static GLuint s_lut_out_tex[2] = {0, 0};
static int    s_lut_out_w = 0, s_lut_out_h = 0;
static int    s_lut_out_valid[2] = {0, 0};
static GLint  s_lut_u_tex = -1, s_lut_u_lut = -1, s_lut_u_slices = -1;
static GLint  s_lut_u_h = -1, s_lut_u_strength = -1;

static int stereo_fx_any(void) {
    /* GI with intensity 0 adds nothing, and AO with intensity 0 resolves to
     * ao == 1.0, so neither must keep the pass alive (the legacy AO path used
     * to pay for the whole gather first). */
    return (s_fx_ao_on && s_fx_ao_intensity > 0) || s_fx_dof_on
        || (s_fx_gi_on && s_fx_gi_intensity > 0)
        || s_fx_debug != 0
        || (gfx_shadow_enabled && s_shadow_map_valid)
        || s_fx_ssrtx_on || s_fx_mb_on || s_light_count > 0 || s_probe_vis
        || s_glow_on            /* bloom needs the FX pass output as its base */
        || s_ray_on
        || s_grade_on           /* tonemap/grade needs the FX pass */
        || (s_denoise_path && s_denoise_on);   /* denoise needs the FX pass */
}

/* Everything stereo_fx_any() lists except AO: when this is 0 the reduced-res
 * AO chain has already produced the final colour and the big FX pass can be
 * skipped entirely (it was 8.4 ms of the 30.5 ms AO cost in 3D).  Keep this in
 * step with stereo_fx_any(): an effect that stops running at 0 intensity must
 * not keep the pass alive here either. */
static int stereo_fx_other(void) {
    return s_fx_dof_on || (s_fx_gi_on && s_fx_gi_intensity > 0) || s_fx_debug != 0
        || (gfx_shadow_enabled && s_shadow_map_valid)
        || s_fx_ssrtx_on || s_fx_mb_on || s_light_count > 0 || s_probe_vis
        || s_glow_on || s_ray_on || s_grade_on
        || (s_denoise_path && s_denoise_on);
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
        /* 390-396: reduced-res AO chain.  All of these are A/B dials, so the
         * full-res path stays one key away and every stage can be timed. */
        case 390: s_fx_ao_path = value ? 1 : 0; break;
        case 391: s_fx_ao_div = value < 1 ? 1 : (value > 4 ? 4 : value); break;
        case 392: s_fx_ao_jitter = value ? 1 : 0; break;
        case 393: s_fx_ao_up = value ? 1 : 0; break;
        case 394: s_fx_ao_sky = value ? 1 : 0; break;
        case 395: s_fx_ao_trig = value ? 1 : 0; break;
        case 396: s_fx_ao_probe = value < 0 ? 0 : (value > 2 ? 2 : value); break;
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
        /* Reduced-resolution SSGI (see the GI prepass below). */
        case 400: s_fx_gi_div = value < 1 ? 1 : (value > 4 ? 4 : value); break;
        case 401: s_fx_gi_blur = value < 0 ? 0 : (value > 3 ? 3 : value); break;
        case 402: s_fx_gi_jitter = value ? 1 : 0; break;
        case 403: s_fx_gi_debug = value < 0 ? 0 : (value > 3 ? 3 : value); break;
        case 404: s_fx_gi_skip = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 405: s_fx_gi_norm = value ? 1 : 0; break;
        case 31: s_fx_debug = value < 0 ? 0 : (value > 9 ? 9 : value); break;
        case 32: s_fx_normal_flip = value ? 1 : 0; break;
        /* Skybox fine-tune (fx 370-373): stored here so `cmd fx` and the panel
         * share one key space; gfx_pc.c applies them to skybox vertices only. */
        case 370: gfx_sky_scale_x = (float) value / 100.0f; break;
        case 371: gfx_sky_scale_y = (float) value / 100.0f; break;
        case 372: gfx_sky_off_x = (float) value; break;
        case 373: gfx_sky_off_y = (float) value; break;
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
        /* Vector motion blur. */
        case 100: s_fx_mb_on = value ? 1 : 0; break;
        case 101: s_fx_mb_strength = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 102: s_fx_mb_samples = value < 1 ? 1 : (value > 16 ? 16 : value); break;
        /* Deferred dynamic lights. */
        case 103: s_light_count = value < 0 ? 0 : (value > 4 ? 4 : value); break;
        case 104: s_light_shadows = value ? 1 : 0; break;
        case 105: s_light_intensity = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 106: s_light_shadow_steps = value < 0 ? 0 : (value > 8 ? 8 : value); break;
        /* Probe grid debug. */
        case 110: s_probe_vis = value ? 1 : 0; break;
        case 111: s_probe_spacing = value < 100 ? 100 : (value > 2000 ? 2000 : value); break;
        case 112:
            s_probe_dim = value < 1 ? 1 : (value > 4 ? 4 : value);
            s_probe_cols = s_probe_rows = s_probe_depth = s_probe_dim;
            break;
        case 113: s_probe_radius = value < 5 ? 5 : (value > 400 ? 400 : value); break;
        case 114: s_probe_res = value < 2 ? 2 : (value > 64 ? 64 : value); break;
        /* Probe grid shape: columns (X), rows (Y), repeats (Z). */
        case 115: s_probe_cols = value < 1 ? 1 : (value > 4 ? 4 : value); break;
        case 116: s_probe_rows = value < 1 ? 1 : (value > 4 ? 4 : value); break;
        case 117: s_probe_depth = value < 1 ? 1 : (value > 4 ? 4 : value); break;
        /* Motion-vector heatmap gain (px of motion mapped to full scale). */
        case 118: s_fx_vec_gain = value < 20 ? 20 : (value > 500 ? 500 : value); break;
        /* Motion-vector source + sub-frame blur dials. */
        case 140: s_vel_source = value ? 1 : 0; break;
        case 141: s_mb_shutter = value < 0 ? 0 : (value > 360 ? 360 : value); break;
        case 142: s_mb_substeps = value < 1 ? 1 : (value > 8 ? 8 : value); break;
        case 143: s_mb_maxpx = value < 0 ? 0 : (value > 128 ? 128 : value); break;
        case 144: {
            extern int gfx_vel_divisor;
            s_vel_divisor_ui = value < 1 ? 1 : (value > 8 ? 8 : value);
            gfx_vel_divisor = s_vel_divisor_ui;
            break;
        }
        case 145: {
            extern int gfx_vel_stride;
            s_vel_stride_ui = value < 1 ? 1 : (value > 4 ? 4 : value);
            gfx_vel_stride = s_vel_stride_ui;
            break;
        }
        case 146: s_mb_jitter = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 147: s_mb_depth_reject = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 148: s_mb_clamp_px = value < 4 ? 4 : (value > 300 ? 300 : value); break;
        case 240: s_velmax_mode = value < 0 ? 0 : (value > 2 ? 2 : value); break;
        case 149: s_vec_style = value < 0 ? 0 : (value > 2 ? 2 : value); break;
        /* Slow-motion debug controls (sim speed, sub-frame interpolation). */
        case 241: {
            extern int g_sim_slow_div;
            g_sim_slow_div = value < 1 ? 1 : (value > 8 ? 8 : value);
            break;
        }
        case 242: {
            extern int g_sim_interp;
            g_sim_interp = value ? 1 : 0;
            break;
        }
        case 243: {
            extern int g_sim_present_t1;
            g_sim_present_t1 = value ? 1 : 0;
            break;
        }
        /* Lens bokeh kernel. */
        case 160: s_fx_bokeh_on = value ? 1 : 0; break;
        case 161: s_fx_bokeh_aspect = value < 25 ? 25 : (value > 400 ? 400 : value); break;
        case 162: s_fx_bokeh_blades = value < 3 ? 3 : (value > 12 ? 12 : value); break;
        case 163: s_fx_bokeh_rot = value < 0 ? 0 : (value > 359 ? 359 : value); break;
        case 164: s_fx_anamorphic = value < 50 ? 50 : (value > 400 ? 400 : value); break;
        case 165: s_fx_bokeh_highlight = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 166: s_fx_cat_eye = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        /* C.DOF reduced-resolution path + staged cost dials (380-389). */
        case 380: s_fx_dof_half = value ? 1 : 0; break;
        case 381: s_fx_dof_div = value < 2 ? 2 : (value > 4 ? 4 : value); break;
        case 382: s_fx_dof_near = value < 0 ? 0 : (value > 2 ? 2 : value); break;
        case 383: s_fx_dof_taps = value < 0 ? 0 : (value > 32 ? 32 : value); break;
        case 384: s_fx_dof_up = value ? 1 : 0; break;
        case 385: s_fx_dof_coc = value ? 1 : 0; break;
        case 386: s_fx_dof_bokeh_pre = value ? 1 : 0; break;
        case 387: s_fx_dof_bilinear = value ? 1 : 0; break;
        case 388: s_fx_dof_stage = value < 0 ? 0 : (value > 2 ? 2 : value); break;
        case 389: s_fx_dof_sharp = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        /* Alternative denoised path. */
        case 210: s_denoise_path = value ? 1 : 0; break;
        case 211: s_denoise_on = value ? 1 : 0; break;
        case 212: s_denoise_intensity = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 213: s_denoise_passes = value < 1 ? 1 : (value > 3 ? 3 : value); break;
        case 214: s_denoise_radius = value < 1 ? 1 : (value > 4 ? 4 : value); break;
        case 215: s_denoise_frames = value < 1 ? 1 : (value > 8 ? 8 : value); break;
        case 216: s_denoise_bounces = value < 1 ? 1 : (value > 4 ? 4 : value); break;
        case 217: s_denoise_caption = value ? 1 : 0; break;
        /* Denoise resolution divisor (1 = full res, 2-4 = scene_res/n). */
        case 220: s_denoise_div = value < 1 ? 1 : (value > 4 ? 4 : value); break;
        /* Crepuscular rays: quarter-res screen-space light shafts. */
        case 250: s_ray_on = value ? 1 : 0; break;
        case 251: s_ray_intensity = value < 0 ? 0 : (value > 400 ? 400 : value); break;
        case 252: s_ray_density = value < 10 ? 10 : (value > 200 ? 200 : value); break;
        case 253: s_ray_decay = value < 50 ? 50 : (value > 100 ? 100 : value); break;
        case 254: s_ray_weight = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 255: s_ray_samples = value < 8 ? 8 : (value > 32 ? 32 : value); break;
        case 256: s_ray_threshold = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 257: s_ray_light = value < 0 ? 0 : (value > 4 ? 4 : value); break;
        case 258: s_ray_jitter = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 259: s_ray_debug = value < 0 ? 0 : (value > 2 ? 2 : value); break;
        case 260: s_ray_bias = value < 0 ? 0 : (value > 50 ? 50 : value); break;
        /* Occluder punch-through: discard occluders between camera and Mario
         * inside a screen-space circle.  274 = mode, 275 radius px,
         * 276 feather %, 277 dither coverage %, 278 depth bias. */
        case 274: s_occl_mode = value < 0 ? 0 : (value > 3 ? 3 : value); break;
        case 275: s_occl_radius = value < 20 ? 20 : (value > 600 ? 600 : value); break;
        case 276: s_occl_feather = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 277: s_occl_cover = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 278: s_occl_bias = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        /* Tone mapping + colour grading (GRADE tab, fx 280-289). */
        case 280:
            s_grade_on = value ? 1 : 0;
            break;
        case 281: s_grade_op = value < 0 ? 0 : (value > 3 ? 3 : value); break;
        case 282: s_grade_exposure = value < -300 ? -300 : (value > 300 ? 300 : value); break;
        case 283: s_grade_white = value < 100 ? 100 : (value > 400 ? 400 : value); break;
        case 284:
            s_grade_look = value < 0 ? 0 : (value > 6 ? 6 : value);
            break;
        case 285: s_grade_strength = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 286: s_grade_sat = value < 0 ? 0 : (value > 200 ? 200 : value); break;
        case 287: s_grade_contrast = value < -100 ? -100 : (value > 100 ? 100 : value); break;
        case 288: s_grade_tint = value < -100 ? -100 : (value > 100 ? 100 : value); break;
        case 289: s_grade_debug = value ? 1 : 0; break;
        /* Rate limiter (lives in gfx_sdl2.c). */
        case 200: { extern int gfx_rate_enabled; gfx_rate_enabled = value != 0; break; }
        case 201: { extern int gfx_rate_target_fps; gfx_rate_target_fps = value < 0 ? 0 : (value > 240 ? 240 : value); break; }
        case 202: { extern int gfx_rate_adaptive; gfx_rate_adaptive = value != 0; break; }
        case 203: { extern void gfx_rate_reset(void); gfx_rate_reset(); break; }
        /* Per-light colour + radius: 120 + i*4 (R,G,B,radius). */
        case 120: case 121: case 122: case 123:
        case 124: case 125: case 126: case 127:
        case 128: case 129: case 130: case 131:
        case 132: case 133: case 134: case 135: {
            int li = (which - 120) / 4;
            int comp = (which - 120) % 4;
            float v = value < 0 ? 0.0f : (value > 255 ? 255.0f : (float)value) / 255.0f;
            if (comp < 3) {
                s_light_color[li][comp] = v;
            } else {
                s_light_radius[li] = value < 100 ? 100.0f : (float)value;
            }
            break;
        }
        /* Emissive materials + bloom (GLOW tab, fx 261-270). */
        case 261: s_glow_on = value ? 1 : 0; break;
        case 262: s_glow_threshold = value < 0 ? 0 : (value > 255 ? 255 : value); break;
        case 263: s_glow_intensity = value < 0 ? 0 : (value > 400 ? 400 : value); break;
        case 264: s_glow_radius = value < 1 ? 1 : (value > 8 ? 8 : value); break;
        case 265: s_glow_passes = value < 1 ? 1 : (value > 3 ? 3 : value); break;
        case 266: s_glow_taps = value >= 9 ? 9 : 5; break;
        case 267: s_glow_gain = value < 0 ? 0 : (value > 400 ? 400 : value); break;
        case 268: s_glow_debug = value < 0 ? 0 : (value > 2 ? 2 : value); break;
        case 269: gfx_emissive_on = value ? 1 : 0; break;
        case 270: s_glow_scene = value ? 1 : 0; break;
        /* Jetpack nozzle light (registered through the light registry). */
        case 271: gfx_jet_light_on = value ? 1 : 0; break;
        case 272: gfx_jet_light_intensity = value < 0 ? 0 : (value > 400 ? 400 : value); break;
        case 273: gfx_jet_light_radius = value < 60 ? 60 : (value > 600 ? 600 : value); break;
        /* Camera sensitivity band / trim (CAMERA > SETTINGS picker).  These are
         * the same knobs as stereo-int 330/331, routed here so `cmd fx` works. */
        case 330: {
            extern int gfx_cam_sens_band;
            extern void gfx_cam_sens_apply(void);
            gfx_cam_sens_band = value;
            gfx_cam_sens_apply();
            break;
        }
        case 331: {
            extern float gfx_cam_sens_trim;
            extern void gfx_cam_sens_apply(void);
            gfx_cam_sens_trim = (float) value;
            gfx_cam_sens_apply();
            break;
        }
        /* Wall-run jump-off: gameplay dials live in movekit.c (fx 320-328). */
        case 320: case 321: case 322: case 323: case 324:
        case 325: case 326: case 327: case 328: case 329: {
            extern void movekit_dial_set(int which, int value);
            movekit_dial_set(which, value);
            break;
        }
        /* 90-degree corner rig dials (ptc.c; fx 410-415): 410 perfect press
         * distance, 411 spacing of the eight timings, 412 re-entry deadline
         * ticks, 413 entry height, 414 entry speed, 415 corner-stuck ticks. */
        case 410: case 411: case 412: case 413: case 414: case 415: {
            extern void ptc_wrc_dial_set(int which, int value);
            ptc_wrc_dial_set(which, value);
            break;
        }
        /* Presentation-time painting ripple mesh interpolation (fx 350).
         * 1 = interpolate with the present alpha (default), 0 = the old
         * per-sim-tick stepping, for A/B captures. */
        case 350: {
            extern int gfx_paint_interp;
            gfx_paint_interp = value ? 1 : 0;
            break;
        }
        /* Camera vs ground (customcam.c; fx 360): 0 = COLLIDE with floors and
         * ceilings (default), 1 = CLIP through them (walls still collide). */
        case 360: {
            extern int gfx_cam_ground_clip;
            gfx_cam_ground_clip = value ? 1 : 0;
            break;
        }
        /* Grapple targeting debug view: the values live in movekit.c next to
         * the aim resolver, so the drawn fan and the real attach share them. */
        case 310: case 311: case 312: case 313: case 314: case 315: {
            extern void movekit_grapple_debug_set(int which, int value);
            movekit_grapple_debug_set(which, value);
            break;
        }
        /* Cam-stick response curve, deadzone/heatmap, R-shoulder follow
         * (customcam.c; fx 290-299 is the cam-stick block). */
        case 290: case 291: case 292: case 293: case 294:
        case 295: case 296: case 297: case 298: case 299: {
            extern void custom_camera_stick_fx_set(int which, int value);
            custom_camera_stick_fx_set(which, value);
            break;
        }
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
        case 390: return s_fx_ao_path;
        case 391: return s_fx_ao_div;
        case 392: return s_fx_ao_jitter;
        case 393: return s_fx_ao_up;
        case 394: return s_fx_ao_sky;
        case 395: return s_fx_ao_trig;
        case 396: return s_fx_ao_probe;
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
        case 400: return s_fx_gi_div;
        case 401: return s_fx_gi_blur;
        case 402: return s_fx_gi_jitter;
        case 403: return s_fx_gi_debug;
        case 404: return s_fx_gi_skip;
        case 405: return s_fx_gi_norm;
        case 31: return s_fx_debug;
        case 32: return s_fx_normal_flip;
        case 370: return (int) (gfx_sky_scale_x * 100.0f + 0.5f);
        case 371: return (int) (gfx_sky_scale_y * 100.0f + 0.5f);
        case 372: return (int) gfx_sky_off_x;
        case 373: return (int) gfx_sky_off_y;
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
        case 100: return s_fx_mb_on;
        case 101: return s_fx_mb_strength;
        case 102: return s_fx_mb_samples;
        case 103: return s_light_count;
        case 104: return s_light_shadows;
        case 105: return s_light_intensity;
        case 106: return s_light_shadow_steps;
        case 110: return s_probe_vis;
        case 111: return s_probe_spacing;
        case 112: return s_probe_dim;
        case 113: return s_probe_radius;
        case 114: return s_probe_res;
        case 115: return s_probe_cols;
        case 116: return s_probe_rows;
        case 117: return s_probe_depth;
        case 118: return s_fx_vec_gain;
        case 140: return s_vel_source;
        case 141: return s_mb_shutter;
        case 142: return s_mb_substeps;
        case 143: return s_mb_maxpx;
        case 144: return s_vel_divisor_ui;
        case 145: return s_vel_stride_ui;
        case 146: return s_mb_jitter;
        case 147: return s_mb_depth_reject;
        case 148: return s_mb_clamp_px;
        case 240: return s_velmax_mode;
        case 149: return s_vec_style;
        case 241: { extern int g_sim_slow_div; return g_sim_slow_div; }
        case 242: { extern int g_sim_interp; return g_sim_interp; }
        case 243: { extern int g_sim_present_t1; return g_sim_present_t1; }
        case 160: return s_fx_bokeh_on;
        case 161: return s_fx_bokeh_aspect;
        case 162: return s_fx_bokeh_blades;
        case 163: return s_fx_bokeh_rot;
        case 164: return s_fx_anamorphic;
        case 165: return s_fx_bokeh_highlight;
        case 166: return s_fx_cat_eye;
        case 380: return s_fx_dof_half;
        case 381: return s_fx_dof_div;
        case 382: return s_fx_dof_near;
        case 383: return s_fx_dof_taps;
        case 384: return s_fx_dof_up;
        case 385: return s_fx_dof_coc;
        case 386: return s_fx_dof_bokeh_pre;
        case 387: return s_fx_dof_bilinear;
        case 388: return s_fx_dof_stage;
        case 389: return s_fx_dof_sharp;
        case 210: return s_denoise_path;
        case 211: return s_denoise_on;
        case 212: return s_denoise_intensity;
        case 213: return s_denoise_passes;
        case 214: return s_denoise_radius;
        case 215: return s_denoise_frames;
        case 216: return s_denoise_bounces;
        case 217: return s_denoise_caption;
        case 220: return s_denoise_div;
        case 250: return s_ray_on;
        case 251: return s_ray_intensity;
        case 252: return s_ray_density;
        case 253: return s_ray_decay;
        case 254: return s_ray_weight;
        case 255: return s_ray_samples;
        case 256: return s_ray_threshold;
        case 257: return s_ray_light;
        case 258: return s_ray_jitter;
        case 259: return s_ray_debug;
        case 260: return s_ray_bias;
        case 274: return s_occl_mode;
        case 275: return s_occl_radius;
        case 276: return s_occl_feather;
        case 277: return s_occl_cover;
        case 278: return s_occl_bias;
        case 350: { extern int gfx_paint_interp; return gfx_paint_interp; }
        case 360: { extern int gfx_cam_ground_clip; return gfx_cam_ground_clip ? 1 : 0; }
        case 200: { extern int gfx_rate_enabled; return gfx_rate_enabled; }
        case 201: { extern int gfx_rate_target_fps; return gfx_rate_target_fps; }
        case 202: { extern int gfx_rate_adaptive; return gfx_rate_adaptive; }
        case 261: return s_glow_on;
        case 262: return s_glow_threshold;
        case 263: return s_glow_intensity;
        case 264: return s_glow_radius;
        case 265: return s_glow_passes;
        case 266: return s_glow_taps;
        case 267: return s_glow_gain;
        case 268: return s_glow_debug;
        case 269: return gfx_emissive_on;
        case 270: return s_glow_scene;
        case 271: return gfx_jet_light_on;
        case 272: return gfx_jet_light_intensity;
        case 273: return gfx_jet_light_radius;
        case 280: return s_grade_on;
        case 281: return s_grade_op;
        case 282: return s_grade_exposure;
        case 283: return s_grade_white;
        case 284: return s_grade_look;
        case 285: return s_grade_strength;
        case 286: return s_grade_sat;
        case 287: return s_grade_contrast;
        case 288: return s_grade_tint;
        case 289: return s_grade_debug;
        case 330: {
            extern int gfx_cam_sens_state(int which);
            return gfx_cam_sens_state(0);
        }
        case 331: {
            extern int gfx_cam_sens_state(int which);
            return gfx_cam_sens_state(1);
        }
        case 332: {
            extern int gfx_cam_sens_state(int which);
            return gfx_cam_sens_state(2);
        }
        case 320: case 321: case 322: case 323: case 324:
        case 325: case 326: case 327: case 328: case 329: {
            extern int movekit_dial_get(int which);
            return movekit_dial_get(which);
        }
        case 410: case 411: case 412: case 413: case 414: case 415: {
            extern int ptc_wrc_dial_get(int which);
            return ptc_wrc_dial_get(which);
        }
        case 310: case 311: case 312: case 313: case 314: case 315: {
            extern int movekit_grapple_debug_get(int which);
            return movekit_grapple_debug_get(which);
        }
        /* Cam-stick curve / heatmap / R-follow (customcam.c). */
        case 290: case 291: case 292: case 293: case 294:
        case 295: case 296: case 297: case 298: case 299: {
            extern int custom_camera_stick_fx_get(int which);
            return custom_camera_stick_fx_get(which);
        }
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
    s_fx_u.ao_probe = glGetUniformLocation(s_fx_prog, "uAOProbe");
    s_fx_u.dof_on = glGetUniformLocation(s_fx_prog, "uDOFOn");
    s_fx_u.dof_focus = glGetUniformLocation(s_fx_prog, "uFocus");
    s_fx_u.dof_aperture = glGetUniformLocation(s_fx_prog, "uAperture");
    s_fx_u.dof_maxblur = glGetUniformLocation(s_fx_prog, "uMaxBlur");
    s_fx_u.dof_samples = glGetUniformLocation(s_fx_prog, "uDOFSamples");
    s_fx_u.dof_pre = glGetUniformLocation(s_fx_prog, "uDofPre");
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
    s_fx_u.mb_on = glGetUniformLocation(s_fx_prog, "uMBOn");
    s_fx_u.mb_strength = glGetUniformLocation(s_fx_prog, "uMBStrength");
    s_fx_u.mb_samples = glGetUniformLocation(s_fx_prog, "uMBSamples");
    s_fx_u.velocity = glGetUniformLocation(s_fx_prog, "uVelocity");
    s_fx_u.vel_on = glGetUniformLocation(s_fx_prog, "uVelOn");
    s_fx_u.vel_scale = glGetUniformLocation(s_fx_prog, "uVelScale");
    s_fx_u.mb_shutter = glGetUniformLocation(s_fx_prog, "uMBShutter");
    s_fx_u.mb_substeps = glGetUniformLocation(s_fx_prog, "uMBSubSteps");
    s_fx_u.mb_maxpx = glGetUniformLocation(s_fx_prog, "uMBMaxPx");
    s_fx_u.mb_jitter = glGetUniformLocation(s_fx_prog, "uMBJitter");
    s_fx_u.mb_depth_reject = glGetUniformLocation(s_fx_prog, "uMBDepthReject");
    s_fx_u.mb_clamp_px = glGetUniformLocation(s_fx_prog, "uMBClampPx");
    s_fx_u.velmax_mode = glGetUniformLocation(s_fx_prog, "uVelMaxMode");
    s_fx_u.vec_style = glGetUniformLocation(s_fx_prog, "uVecStyle");
    s_fx_u.vel_max = glGetUniformLocation(s_fx_prog, "uVelMax");
    s_fx_u.vel_max_on = glGetUniformLocation(s_fx_prog, "uVelMaxOn");
    s_fx_u.vel_max_grid = glGetUniformLocation(s_fx_prog, "uVelMaxGrid");
    s_fx_u.cur_vp = glGetUniformLocation(s_fx_prog, "uCurVP");
    s_fx_u.prev_vp = glGetUniformLocation(s_fx_prog, "uPrevVP");
    s_fx_u.prev_cam_inv = glGetUniformLocation(s_fx_prog, "uPrevCamInv");
    s_fx_u.light_count = glGetUniformLocation(s_fx_prog, "uLightCount");
    s_fx_u.light_pos = glGetUniformLocation(s_fx_prog, "uLightPos[0]");
    s_fx_u.light_pos_view = glGetUniformLocation(s_fx_prog, "uLightPosView[0]");
    s_fx_u.light_color = glGetUniformLocation(s_fx_prog, "uLightColor[0]");
    s_fx_u.light_radius = glGetUniformLocation(s_fx_prog, "uLightRadius[0]");
    s_fx_u.light_intensity = glGetUniformLocation(s_fx_prog, "uLightIntensity");
    s_fx_u.light_shadows = glGetUniformLocation(s_fx_prog, "uLightShadows");
    s_fx_u.light_shadow_steps = glGetUniformLocation(s_fx_prog, "uLightShadowSteps");
    s_fx_u.probe_vis = glGetUniformLocation(s_fx_prog, "uProbeVis");
    s_fx_u.probe_center = glGetUniformLocation(s_fx_prog, "uProbeCenter");
    s_fx_u.probe_spacing = glGetUniformLocation(s_fx_prog, "uProbeSpacing");
    s_fx_u.probe_dim = glGetUniformLocation(s_fx_prog, "uProbeDim");
    s_fx_u.probe_radius = glGetUniformLocation(s_fx_prog, "uProbeRadiusW");
    s_fx_u.probe_res = glGetUniformLocation(s_fx_prog, "uProbeRes");
    s_fx_u.probe_pos = glGetUniformLocation(s_fx_prog, "uProbePos[0]");
    s_fx_u.probe_col = glGetUniformLocation(s_fx_prog, "uProbeCol[0]");
    s_fx_u.probe_count = glGetUniformLocation(s_fx_prog, "uProbeCount");
    s_fx_u.ray_tex = glGetUniformLocation(s_fx_prog, "uRayTex");
    s_fx_u.ray_on = glGetUniformLocation(s_fx_prog, "uRayOn");
    s_fx_u.ray_intensity = glGetUniformLocation(s_fx_prog, "uRayIntensity");
    s_fx_u.ray_debug = glGetUniformLocation(s_fx_prog, "uRayDebug");
    s_fx_u.ray_color = glGetUniformLocation(s_fx_prog, "uRayColor");
    s_fx_u.ray_light_uv = glGetUniformLocation(s_fx_prog, "uRayLightUV");
    s_fx_u.denoise_path = glGetUniformLocation(s_fx_prog, "uDenoisePath");
    s_fx_u.denoise_on = glGetUniformLocation(s_fx_prog, "uDenoiseOn");
    s_fx_u.denoise_intensity = glGetUniformLocation(s_fx_prog, "uDenoiseIntensity");
    s_fx_u.denoise_passes = glGetUniformLocation(s_fx_prog, "uDenoisePasses");
    s_fx_u.denoise_radius = glGetUniformLocation(s_fx_prog, "uDenoiseRadius");
    s_fx_u.denoise_frames = glGetUniformLocation(s_fx_prog, "uDenoiseFrames");
    s_fx_u.denoise_bounces = glGetUniformLocation(s_fx_prog, "uDenoiseBounces");
    s_fx_u.vec_gain = glGetUniformLocation(s_fx_prog, "uVecGain");
    s_fx_u.bokeh_on = glGetUniformLocation(s_fx_prog, "uBokehOn");
    s_fx_u.bokeh_aspect = glGetUniformLocation(s_fx_prog, "uBokehAspect");
    s_fx_u.bokeh_blades = glGetUniformLocation(s_fx_prog, "uBokehBlades");
    s_fx_u.bokeh_rot = glGetUniformLocation(s_fx_prog, "uBokehRot");
    s_fx_u.anamorphic = glGetUniformLocation(s_fx_prog, "uAnamorphic");
    s_fx_u.bokeh_highlight = glGetUniformLocation(s_fx_prog, "uBokehHighlight");
    s_fx_u.cat_eye = glGetUniformLocation(s_fx_prog, "uCatEye");
    s_fx_u.grade_on = glGetUniformLocation(s_fx_prog, "uGradeOn");
    s_fx_u.grade_op = glGetUniformLocation(s_fx_prog, "uGradeOp");
    s_fx_u.grade_exposure = glGetUniformLocation(s_fx_prog, "uGradeExposure");
    s_fx_u.grade_white = glGetUniformLocation(s_fx_prog, "uGradeWhite");
    s_fx_u.grade_look = glGetUniformLocation(s_fx_prog, "uGradeLook");
    s_fx_u.grade_strength = glGetUniformLocation(s_fx_prog, "uGradeStrength");
    s_fx_u.grade_sat = glGetUniformLocation(s_fx_prog, "uGradeSat");
    s_fx_u.grade_contrast = glGetUniformLocation(s_fx_prog, "uGradeContrast");
    s_fx_u.grade_tint = glGetUniformLocation(s_fx_prog, "uGradeTint");
    s_fx_u.grade_debug = glGetUniformLocation(s_fx_prog, "uGradeDebug");
    __android_log_print(ANDROID_LOG_INFO, "SM64FX",
        "new uniforms: mb=%d lc=%d pv=%d psp=%d",
        s_fx_u.mb_on, s_fx_u.light_count, s_fx_u.probe_vis, s_fx_u.probe_spacing);
    s_fx_prog_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX", "FX program ready (%u)", s_fx_prog);
}

/* ---------------- Optional 2D stripe LUT (GRADE look 6) ------------------
 * GLES2 has no 3D textures, so the LUT is a horizontal strip of 16x16 blue
 * slices (width 16*N, height 16).  It is applied by its own tiny pass after
 * the FX pass (and after bloom) rather than as a seventh sampler inside the
 * big FX shader: adding a sampler there measured ~30 ms on this GPU (BACKLOG).
 * With no file, look 6 falls back to "none" and the analytic looks work. */
static const char *s_lut_fs =
    "precision highp float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uLut;\n"
    "uniform float uSlices;\n"
    "uniform float uHeight;\n"
    "uniform float uStrength;\n"
    "void main() {\n"
    "  vec3 c = clamp(texture2D(uTex, vUV).rgb, 0.0, 1.0);\n"
    "  float n = max(uSlices, 1.0);\n"
    "  float slice = min(floor(c.b * n), n - 1.0);\n"
    // half-texel insets keep bilinear taps inside one slice/tile
    "  float x = (slice * 16.0 + 0.5 + clamp(c.r, 0.0, 1.0) * 15.0) / (n * 16.0);\n"
    "  float y = (0.5 + clamp(c.g, 0.0, 1.0) * max(uHeight - 1.0, 1.0)) / max(uHeight, 1.0);\n"
    "  vec3 lut = texture2D(uLut, vec2(x, y)).rgb;\n"
    "  gl_FragColor = vec4(mix(c, lut, clamp(uStrength, 0.0, 1.0)), 1.0);\n"
    "}\n";

/* The image is decoded in Java (BitmapFactory) and handed here as RGBA bytes;
 * this keeps stb_image out of the build (it is only compiled when EXTERNAL_DATA
 * is set) and puts file/dimension errors in the Java log next to the file path.
 * gfx_opengl_grade_lut_submit() may be called from the UI thread, so the buffer
 * is published through an atomic exchange and consumed on the GL thread. */
typedef struct {
    int w, h;
    unsigned char *rgba;
} GradeLutUpload;

static GradeLutUpload *s_lut_pending = NULL;

/* JNI entry (any thread): takes ownership of `rgba` (malloc'd, w*h*4 bytes).
 * w == 0 means "clear the LUT" (no file / rejected). */
void gfx_opengl_grade_lut_submit(unsigned char *rgba, int w, int h) {
    GradeLutUpload *u = (GradeLutUpload *) malloc(sizeof(GradeLutUpload));
    GradeLutUpload *old;
    if (u == NULL) {
        free(rgba);
        return;
    }
    u->w = w;
    u->h = h;
    u->rgba = rgba;
    old = __atomic_exchange_n(&s_lut_pending, u, __ATOMIC_ACQ_REL);
    if (old != NULL) {
        free(old->rgba);
        free(old);
    }
}

/* GL thread: publish request -> texture. */
static void grade_lut_consume_pending(void) {
    GradeLutUpload *u = __atomic_exchange_n(&s_lut_pending, NULL, __ATOMIC_ACQ_REL);
    if (u == NULL) return;
    if (s_lut_tex) {
        glDeleteTextures(1, &s_lut_tex);
        s_lut_tex = 0;
    }
    s_lut_valid = 0;
    s_lut_slices = 16;
    s_lut_w = s_lut_h = 0;
    if (u->rgba != NULL && u->w > 0 && u->h > 0) {
        glGenTextures(1, &s_lut_tex);
        glBindTexture(GL_TEXTURE_2D, s_lut_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, u->w, u->h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, u->rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        s_lut_valid = 1;
        s_lut_slices = u->w / 16;
        s_lut_w = u->w;
        s_lut_h = u->h;
        __android_log_print(ANDROID_LOG_INFO, "SM64FX",
                "GRADE LUT uploaded (%dx%d, %d slices, tex=%u)",
                s_lut_w, s_lut_h, s_lut_slices, s_lut_tex);
    }
    free(u->rgba);
    free(u);
}

static void stereo_ensure_lut_out(int w, int h) {
    if (w == s_lut_out_w && h == s_lut_out_h && s_lut_fbo[0] != 0) return;
    for (int i = 0; i < 2; i++) {
        if (s_lut_fbo[i]) glDeleteFramebuffers(1, &s_lut_fbo[i]);
        if (s_lut_out_tex[i]) glDeleteTextures(1, &s_lut_out_tex[i]);
        glGenTextures(1, &s_lut_out_tex[i]);
        glBindTexture(GL_TEXTURE_2D, s_lut_out_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s_lut_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_lut_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_lut_out_tex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_lut_out_w = w;
    s_lut_out_h = h;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX",
            "GRADE LUT out %dx%d", w, h);
}

static int grade_lut_init(void) {
    if (s_lut_prog_state == 1) return 1;
    if (s_lut_prog_state == -1) return 0;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_fx_vs, "lut vertex");
    if (!vs) { s_lut_prog_state = -1; return 0; }
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_lut_fs, "lut fragment");
    if (!fs) { glDeleteShader(vs); s_lut_prog_state = -1; return 0; }
    s_lut_prog = glCreateProgram();
    glAttachShader(s_lut_prog, vs);
    glAttachShader(s_lut_prog, fs);
    glLinkProgram(s_lut_prog);
    GLint ok = 0;
    glGetProgramiv(s_lut_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        __android_log_print(ANDROID_LOG_WARN, "SM64FX", "GRADE LUT program failed");
        glDeleteProgram(s_lut_prog);
        s_lut_prog = 0;
        s_lut_prog_state = -1;
        return 0;
    }
    s_lut_u_tex = glGetUniformLocation(s_lut_prog, "uTex");
    s_lut_u_lut = glGetUniformLocation(s_lut_prog, "uLut");
    s_lut_u_slices = glGetUniformLocation(s_lut_prog, "uSlices");
    s_lut_u_h = glGetUniformLocation(s_lut_prog, "uHeight");
    s_lut_u_strength = glGetUniformLocation(s_lut_prog, "uStrength");
    s_lut_prog_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX", "GRADE LUT program ready");
    return 1;
}

static void stereo_grade_lut_pass(void) {
    s_lut_out_valid[0] = s_lut_out_valid[1] = 0;
    grade_lut_consume_pending();
    if (!(s_grade_on && s_grade_look == 6 && s_lut_valid && s_grade_strength > 0)) return;
    if (!grade_lut_init()) return;
    stereo_ensure_lut_out(s_pp_w, s_pp_h);
    if (s_lut_fbo[0] == 0) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_lut_prog);

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    GLint apos = glGetAttribLocation(s_lut_prog, "aPos");
    GLint auv = glGetAttribLocation(s_lut_prog, "aUV");
    glEnableVertexAttribArray(apos);
    glVertexAttribPointer(apos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(auv);
    glVertexAttribPointer(auv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_lut_u_tex, 0);
    glActiveTexture(GL_TEXTURE1);
    glUniform1i(s_lut_u_lut, 1);
    glUniform1f(s_lut_u_slices, (float)s_lut_slices);
    glUniform1f(s_lut_u_h, (float)s_lut_h);
    glUniform1f(s_lut_u_strength, (float)s_grade_strength / 100.0f);

    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;
        glBindFramebuffer(GL_FRAMEBUFFER, s_lut_fbo[eye]);
        glViewport(0, 0, s_pp_w, s_pp_h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_pp_tex[eye]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_lut_tex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        s_lut_out_valid[eye] = 1;
    }

    glDisableVertexAttribArray(apos);
    glDisableVertexAttribArray(auv);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
}


/* Programmatic control for the PTC smoke test / debug UI. */
void gfx_opengl_light_set(int index, float x, float y, float z,
                          float r, float g, float b, float radius) {
    if (index < 0 || index > 3) return;
    s_light_pos[index][0] = x;
    s_light_pos[index][1] = y;
    s_light_pos[index][2] = z;
    s_light_color[index][0] = r;
    s_light_color[index][1] = g;
    s_light_color[index][2] = b;
    s_light_radius[index] = radius < 100.0f ? 100.0f : radius;
}

void gfx_opengl_light_color(int index, float r, float g, float b) {
    if (index < 0 || index > 3) return;
    s_light_color[index][0] = r;
    s_light_color[index][1] = g;
    s_light_color[index][2] = b;
}

void gfx_opengl_light_pos(int index, float x, float y, float z) {
    if (index < 0 || index > 3) return;
    s_light_pos[index][0] = x;
    s_light_pos[index][1] = y;
    s_light_pos[index][2] = z;
}

void gfx_opengl_light_count(int count) {
    s_light_count = count < 0 ? 0 : (count > 4 ? 4 : count);
    // A direct caller (the PTC lightshow) takes ownership of all four slots.
    // The per-frame gameplay light registry then leaves them alone; count 0
    // hands them back.
    s_light_manual = s_light_count > 0;
}

/* Used by the gameplay light registry in gfx_pc.c: applies the frame's lights
 * without claiming ownership of the slots. */
void gfx_opengl_light_count_auto(int count) {
    s_light_count = count < 0 ? 0 : (count > 4 ? 4 : count);
}

int gfx_opengl_lights_manual(void) {
    return s_light_manual;
}

void gfx_opengl_light_shadows(int on) {
    s_light_shadows = on ? 1 : 0;
}

void gfx_opengl_probe_config(float x, float y, float z, int spacing, int dim,
                             int radius, int resolution, int vis) {
    s_probe_center[0] = x;
    s_probe_center[1] = y;
    s_probe_center[2] = z;
    s_probe_spacing = spacing < 100 ? 100 : (spacing > 2000 ? 2000 : spacing);
    s_probe_dim = dim < 1 ? 1 : (dim > 4 ? 4 : dim);
    s_probe_radius = radius < 5 ? 5 : (radius > 400 ? 400 : radius);
    s_probe_res = resolution < 2 ? 2 : (resolution > 64 ? 64 : resolution);
    s_probe_vis = vis ? 1 : 0;
}

int gfx_opengl_denoise_state(int which) {
    switch (which) {
        case 0: return s_denoise_path;
        case 1: return s_denoise_on;
        case 2: return s_denoise_intensity;
        case 3: return s_denoise_passes;
        case 4: return s_denoise_radius;
        case 5: return s_denoise_frames;
        case 6: return s_denoise_bounces;
        case 7: return s_denoise_caption;
        case 8: return s_denoise_div;
    }
    return -1;
}

int gfx_opengl_light_state(int which) {
    switch (which) {
        case 0: return s_light_count;
        case 1: return s_light_shadows;
        case 2: return s_fx_mb_on;
        case 3: return s_probe_vis;
        case 4: return s_probe_dim;
        case 5: return s_probe_spacing;
        case 6: return s_probe_radius;
        case 7: return s_probe_res;
        case 8: return s_light_intensity;
    }
    return -1;
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

/* ---------------- Motion-vector (velocity) pass ----------------
 * A second scene replay writes per-vertex screen motion into a small RGBA8
 * target: two 16-bit fixed-point channels per axis, 0.5 = no motion.
 * gfx_pc.c computes the delta from the object's previous modelview. */
static GLuint s_vel_fbo, s_vel_tex, s_vel_depth, s_vel_prog;
static int s_vel_prog_state;
static int s_vel_w, s_vel_h, s_vel_div = 4;
static GLint s_vel_apos = -1, s_vel_avel = -1;
static int s_vel_rendering;
static int s_vel_tex_valid;

static const char *s_vel_vs =
    "attribute vec4 aVtxPos;\n"
    "attribute vec2 aVel;\n"
    "varying vec2 vVel;\n"
    "void main(){ gl_Position = aVtxPos; vVel = aVel; }\n";

static const char *s_vel_fs =
#ifdef USE_GLES
    "precision highp float;\n"
#endif
    "varying vec2 vVel;\n"
    "vec2 enc16(float x) {\n"
    "  float v = clamp(x * 0.5 + 0.5, 0.0, 1.0) * 65535.0;\n"
    "  float hi = floor(v / 256.0);\n"
    "  return vec2(hi, v - hi * 256.0) / 255.0;\n"
    "}\n"
    "void main(){\n"
    "  vec2 v = clamp(vVel, -1.0, 1.0);\n"
    "  gl_FragColor = vec4(enc16(v.x), enc16(v.y));\n"
    "}\n";

static int velocity_ensure_program(void) {
    if (s_vel_prog_state != 0) return s_vel_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_vel_vs, "velocity vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_vel_fs, "velocity fragment");
    if (!vs || !fs) {
        s_vel_prog_state = -1;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    s_vel_prog = glCreateProgram();
    glAttachShader(s_vel_prog, vs);
    glAttachShader(s_vel_prog, fs);
    glLinkProgram(s_vel_prog);
    GLint ok = 0;
    glGetProgramiv(s_vel_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        __android_log_print(ANDROID_LOG_ERROR, "SM64VEL", "velocity program link failed");
        glDeleteProgram(s_vel_prog);
        s_vel_prog = 0;
        s_vel_prog_state = -1;
        return 0;
    }
    s_vel_apos = glGetAttribLocation(s_vel_prog, "aVtxPos");
    s_vel_avel = glGetAttribLocation(s_vel_prog, "aVel");
    s_vel_prog_state = 1;
    return 1;
}

static int velocity_ensure_fbo(int w, int h) {
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w == s_vel_w && h == s_vel_h && s_vel_fbo != 0) return 1;

    if (s_vel_fbo) glDeleteFramebuffers(1, &s_vel_fbo);
    if (s_vel_tex) glDeleteTextures(1, &s_vel_tex);
    if (s_vel_depth) glDeleteRenderbuffers(1, &s_vel_depth);
    s_vel_fbo = s_vel_tex = s_vel_depth = 0;

    glGenTextures(1, &s_vel_tex);
    glBindTexture(GL_TEXTURE_2D, s_vel_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &s_vel_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_vel_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);

    glGenFramebuffers(1, &s_vel_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_vel_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_vel_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_vel_depth);

    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_vel_w = w;
    s_vel_h = h;
    s_vel_tex_valid = 0;
    __android_log_print(ANDROID_LOG_INFO, "SM64VEL", "target %dx%d fbo=%u status=0x%x",
                        w, h, s_vel_fbo, st);
    return st == GL_FRAMEBUFFER_COMPLETE;
}

int gfx_opengl_velocity_begin(int w, int h, int divisor) {
    if (!velocity_ensure_program()) return 0;
    if (divisor < 1) divisor = 1;
    if (divisor > 8) divisor = 8;
    s_vel_div = divisor;
    if (!velocity_ensure_fbo(w / divisor, h / divisor)) return 0;

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    glBindFramebuffer(GL_FRAMEBUFFER, s_vel_fbo);
    glViewport(0, 0, s_vel_w, s_vel_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    /* 0.5 in both 16-bit channels decodes to zero motion. */
    glClearColor(128.0f / 255.0f, 0.0f, 128.0f / 255.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(s_vel_prog);
    if (!opengl_vbo) glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    s_vel_rendering = 1;
    return 1;
}

static void velocity_tilemax(void);

void gfx_opengl_velocity_end(void) {
    if (s_vel_apos >= 0) glDisableVertexAttribArray(s_vel_apos);
    if (s_vel_avel >= 0) glDisableVertexAttribArray(s_vel_avel);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_vel_rendering = 0;
    s_vel_tex_valid = 1;
    velocity_tilemax();
}

void gfx_opengl_draw_velocity(float buf[], size_t len, size_t tris) {
    if (!s_vel_rendering || !s_vel_prog || len == 0 || tris == 0) return;
    glUseProgram(s_vel_prog);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * len, buf, GL_STREAM_DRAW);
    if (s_vel_apos >= 0) {
        glEnableVertexAttribArray(s_vel_apos);
        glVertexAttribPointer(s_vel_apos, 4, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void *)0);
    }
    if (s_vel_avel >= 0) {
        glEnableVertexAttribArray(s_vel_avel);
        glVertexAttribPointer(s_vel_avel, 2, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void *)(4 * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(3 * tris));
}

/* Tile-max: reduce the velocity target to max |v| per 8x8 block. The blur
 * then bounds its kernel by the 3x3 neighbourhood (McGuire 2012), so one fast
 * pixel cannot smear the whole screen. */
static GLuint s_vmax_fbo, s_vmax_tex, s_vmax_prog;
static int s_vmax_prog_state, s_vmax_w, s_vmax_h;
static GLint s_vmax_apos = -1, s_vmax_vel = -1, s_vmax_texel = -1, s_vmax_grid = -1;
static int s_vmax_valid;

static const char *s_vmax_vs =
    "attribute vec2 aPos;\n"
    "varying vec2 vUV;\n"
    "void main(){ vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *s_vmax_fs =
#ifdef USE_GLES
    "precision highp float;\n"
#endif
    "uniform sampler2D uVel;\n"
    "uniform vec2 uTexel;\n"
    "uniform vec2 uGrid;\n"
    "varying vec2 vUV;\n"
    "void main(){\n"
    "  vec2 org = floor(vUV * uGrid) * 8.0;\n"
    "  float m = 0.0;\n"
    "  for (int y = 0; y < 8; y++) {\n"
    "    for (int x = 0; x < 8; x++) {\n"
    "      vec4 e = texture2D(uVel, (org + vec2(float(x) + 0.5, float(y) + 0.5)) * uTexel);\n"
    "      vec2 v = vec2(e.r * 255.0 * 256.0 + e.g * 255.0,\n"
    "                    e.b * 255.0 * 256.0 + e.a * 255.0) / 65535.0;\n"
    "      m = max(m, length(v * 2.0 - 1.0));\n"
    "    }\n"
    "  }\n"
    "  gl_FragColor = vec4(m, 0.0, 0.0, 1.0);\n"
    "}\n";

static int vmax_ensure_program(void) {
    if (s_vmax_prog_state != 0) return s_vmax_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_vmax_vs, "vmax vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_vmax_fs, "vmax fragment");
    if (!vs || !fs) {
        s_vmax_prog_state = -1;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    s_vmax_prog = glCreateProgram();
    glAttachShader(s_vmax_prog, vs);
    glAttachShader(s_vmax_prog, fs);
    glLinkProgram(s_vmax_prog);
    GLint ok = 0;
    glGetProgramiv(s_vmax_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        s_vmax_prog_state = -1;
        glDeleteProgram(s_vmax_prog);
        s_vmax_prog = 0;
        return 0;
    }
    s_vmax_apos = glGetAttribLocation(s_vmax_prog, "aPos");
    s_vmax_vel = glGetUniformLocation(s_vmax_prog, "uVel");
    s_vmax_texel = glGetUniformLocation(s_vmax_prog, "uTexel");
    s_vmax_grid = glGetUniformLocation(s_vmax_prog, "uGrid");
    s_vmax_prog_state = 1;
    return 1;
}

static int vmax_ensure_fbo(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == s_vmax_w && h == s_vmax_h && s_vmax_fbo != 0) return 1;
    if (s_vmax_fbo) glDeleteFramebuffers(1, &s_vmax_fbo);
    if (s_vmax_tex) glDeleteTextures(1, &s_vmax_tex);
    s_vmax_fbo = s_vmax_tex = 0;

    glGenTextures(1, &s_vmax_tex);
    glBindTexture(GL_TEXTURE_2D, s_vmax_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &s_vmax_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_vmax_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_vmax_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_vmax_w = w;
    s_vmax_h = h;
    s_vmax_valid = 0;
    return st == GL_FRAMEBUFFER_COMPLETE;
}

static void velocity_tilemax(void) {
    if (s_velmax_mode == 0) {
        s_vmax_valid = 0;
        return;
    }
    if (!s_vel_tex_valid || !s_vel_tex) return;
    if (!vmax_ensure_program()) return;
    int gw = s_vel_w / 8; if (gw < 1) gw = 1;
    int gh = s_vel_h / 8; if (gh < 1) gh = 1;
    if (!vmax_ensure_fbo(gw, gh)) return;

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, s_vel_tex);
    glActiveTexture(GL_TEXTURE0);

    glBindFramebuffer(GL_FRAMEBUFFER, s_vmax_fbo);
    glViewport(0, 0, gw, gh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_vmax_prog);
    if (s_vmax_vel >= 0) glUniform1i(s_vmax_vel, 4);
    if (s_vmax_texel >= 0)
        glUniform2f(s_vmax_texel, 1.0f / (float)s_vel_w, 1.0f / (float)s_vel_h);
    if (s_vmax_grid >= 0) glUniform2f(s_vmax_grid, (float)gw, (float)gh);

    static const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    if (!opengl_vbo) glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    if (s_vmax_apos >= 0) {
        glEnableVertexAttribArray(s_vmax_apos);
        glVertexAttribPointer(s_vmax_apos, 2, GL_FLOAT, GL_FALSE,
                              2 * sizeof(float), (void *)0);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (s_vmax_apos >= 0) glDisableVertexAttribArray(s_vmax_apos);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    s_vmax_valid = 1;
}

int gfx_opengl_velocity_wanted(void) {
    return gfx_vel_enabled && (s_fx_mb_on || s_fx_debug == 9);
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

static void fx_mat4_mul(float r[4][4], const float a[4][4], const float b[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j]
                    + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(r, t, sizeof(t));
}

/* world -> clip, row-vector convention (p' = p * VP).  Built from the
 * camera-to-world matrix the shadow pass already publishes. */
static void fx_build_vp(float out[4][4]) {
    const float (*M)[4] = gfx_cam_inv_mtx;
    float V[4][4];
    float cp[3] = { M[3][0], M[3][1], M[3][2] };

    memset(V, 0, sizeof(V));
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            V[i][j] = M[j][i];
        }
    }
    for (int j = 0; j < 3; j++) {
        V[3][j] = -(cp[0] * V[0][j] + cp[1] * V[1][j] + cp[2] * V[2][j]);
    }
    V[3][3] = 1.0f;
    fx_mat4_mul(out, V, gfx_last_persp_proj);
}

/* ---------------- Reduced-resolution denoise prepass ---------------- */

static int denoise_ensure_program(void) {
    if (s_dn_prog_state != 0) return s_dn_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_dn_vs, "denoise vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_dn_fs, "denoise fragment");
    if (!vs || !fs) {
        s_dn_prog_state = -1;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    s_dn_prog = glCreateProgram();
    glAttachShader(s_dn_prog, vs);
    glAttachShader(s_dn_prog, fs);
    glLinkProgram(s_dn_prog);
    GLint ok = 0;
    glGetProgramiv(s_dn_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(s_dn_prog, sizeof(log), NULL, log);
        __android_log_print(ANDROID_LOG_WARN, "SM64FX", "denoise link failed: %s", log);
        glDeleteProgram(s_dn_prog);
        s_dn_prog = 0;
        s_dn_prog_state = -1;
        return 0;
    }
    s_dn_apos = glGetAttribLocation(s_dn_prog, "aPos");
    s_dn_auv = glGetAttribLocation(s_dn_prog, "aUV");
    s_dn_u.mode = glGetUniformLocation(s_dn_prog, "uMode");
    s_dn_u.src = glGetUniformLocation(s_dn_prog, "uSrc");
    s_dn_u.src_depth = glGetUniformLocation(s_dn_prog, "uSrcDepth");
    s_dn_u.low_col = glGetUniformLocation(s_dn_prog, "uLowCol");
    s_dn_u.low_dep = glGetUniformLocation(s_dn_prog, "uLowDep");
    s_dn_u.texel = glGetUniformLocation(s_dn_prog, "uTexel");
    s_dn_u.step = glGetUniformLocation(s_dn_prog, "uStep");
    s_dn_u.nearv = glGetUniformLocation(s_dn_prog, "uNear");
    s_dn_u.farv = glGetUniformLocation(s_dn_prog, "uFar");
    s_dn_u.depth_sigma = glGetUniformLocation(s_dn_prog, "uDepthSigma");
    s_dn_u.colour_sigma = glGetUniformLocation(s_dn_prog, "uColourSigma");
    s_dn_u.up_sigma = glGetUniformLocation(s_dn_prog, "uUpSigma");
    s_dn_u.mix_a = glGetUniformLocation(s_dn_prog, "uMixA");
    s_dn_prog_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX", "denoise program ready (%u)", s_dn_prog);
    return 1;
}

/* Colour ping-pong + packed-depth targets at s_eye_w/div x s_eye_h/div, plus
 * the full-res bilateral upsample target per eye. */
static void stereo_ensure_denoise(int div) {
    int w = s_eye_w / div;
    int h = s_eye_h / div;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == s_dn_w && h == s_dn_h && div == s_dn_div && s_dn_fbo[0][0] != 0
        && s_dn_upw == s_eye_w && s_dn_uph == s_eye_h) {
        return;
    }
    for (int eye = 0; eye < 2; eye++) {
        for (int st = 0; st < 2; st++) {
            if (s_dn_fbo[eye][st]) glDeleteFramebuffers(1, &s_dn_fbo[eye][st]);
            if (s_dn_col[eye][st]) glDeleteTextures(1, &s_dn_col[eye][st]);
            glGenTextures(1, &s_dn_col[eye][st]);
            glBindTexture(GL_TEXTURE_2D, s_dn_col[eye][st]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &s_dn_fbo[eye][st]);
            glBindFramebuffer(GL_FRAMEBUFFER, s_dn_fbo[eye][st]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, s_dn_col[eye][st], 0);
        }
        if (s_dn_dfbo[eye]) glDeleteFramebuffers(1, &s_dn_dfbo[eye]);
        if (s_dn_dep[eye]) glDeleteTextures(1, &s_dn_dep[eye]);
        glGenTextures(1, &s_dn_dep[eye]);
        glBindTexture(GL_TEXTURE_2D, s_dn_dep[eye]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s_dn_dfbo[eye]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_dn_dfbo[eye]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_dn_dep[eye], 0);
        /* Full-res bilateral upsample destination. */
        if (s_dn_ufbo[eye]) glDeleteFramebuffers(1, &s_dn_ufbo[eye]);
        if (s_dn_up[eye]) glDeleteTextures(1, &s_dn_up[eye]);
        glGenTextures(1, &s_dn_up[eye]);
        glBindTexture(GL_TEXTURE_2D, s_dn_up[eye]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_eye_w, s_eye_h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s_dn_ufbo[eye]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_dn_ufbo[eye]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_dn_up[eye], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_dn_w = w;
    s_dn_h = h;
    s_dn_div = div;
    s_dn_upw = s_eye_w;
    s_dn_uph = s_eye_h;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX",
            "DN fbo eye0=%u,%u dep=%u up=%u %dx%d div=%d (eye %dx%d)",
            s_dn_col[0][0], s_dn_col[0][1], s_dn_dep[0], s_dn_up[0], w, h, div,
            s_eye_w, s_eye_h);
}

/* Downsample + à-trous at 1/div resolution + bilateral upsample, both eyes.
 * Leaves the full-res denoised colour in s_dn_result[eye] for the FX pass. */
static void stereo_denoise_prepass(int div) {
    s_dn_valid[0] = s_dn_valid[1] = 0;
    stereo_ensure_denoise(div);
    if (!denoise_ensure_program()) return;

    int passes = s_denoise_passes + s_denoise_bounces - 1;
    if (passes < 1) passes = 1;
    if (passes > 6) passes = 6;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_dn_prog);

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(s_dn_apos);
    glVertexAttribPointer(s_dn_apos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(s_dn_auv);
    glVertexAttribPointer(s_dn_auv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    glUniform1i(s_dn_u.src, 0);
    glUniform1i(s_dn_u.src_depth, 1);
    glUniform1i(s_dn_u.low_col, 2);
    glUniform1i(s_dn_u.low_dep, 3);
    glUniform2f(s_dn_u.texel, 1.0f / (float)s_dn_w, 1.0f / (float)s_dn_h);
    glUniform1f(s_dn_u.nearv, gfx_cam_near);
    glUniform1f(s_dn_u.farv, gfx_cam_far);
    /* The tap spacing is one reduced-res texel, i.e. `div` screen pixels, so
     * the depth acceptance is divided by `div` to keep the same surface
     * tolerance the full-res tap path uses at one screen pixel. */
    glUniform1f(s_dn_u.depth_sigma, 0.10f / (float)div);
    glUniform1f(s_dn_u.colour_sigma, 2.0f);
    /* Upsample edge-stop: ~4x tighter than the à-trous weight because the
     * four taps are only `div` pixels from the centre. */
    glUniform1f(s_dn_u.up_sigma, 0.25f / (float)div);
    /* Same blend the in-shader full-res path uses for intensity/frames; the
     * FX shader's own denoise block is disabled when this prepass runs. */
    {
        float accw = 1.0f / (float)(s_denoise_frames < 1 ? 1 : s_denoise_frames);
        float a = ((float)s_denoise_intensity / 100.0f) * (0.45f + 0.55f * accw);
        glUniform1f(s_dn_u.mix_a, a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a));
    }

    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;

        glViewport(0, 0, s_dn_w, s_dn_h);
        /* 1. scene colour -> low-res colour (stage 0) */
        glBindFramebuffer(GL_FRAMEBUFFER, s_dn_fbo[eye][0]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_eye_tex[eye]);
        glUniform1f(s_dn_u.mode, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        /* 2. depth -> low-res packed linear z */
        glBindFramebuffer(GL_FRAMEBUFFER, s_dn_dfbo[eye]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_eye_depth[eye]);
        glUniform1f(s_dn_u.mode, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        /* 3. N edge-aware à-trous iterations over the low-res colour */
        int src = 0;
        float step = (float)s_denoise_radius;
        for (int p = 0; p < passes; p++) {
            int dst = 1 - src;
            glBindFramebuffer(GL_FRAMEBUFFER, s_dn_fbo[eye][dst]);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, s_dn_col[eye][src]);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, s_dn_dep[eye]);
            glUniform1f(s_dn_u.step, step);
            glUniform1f(s_dn_u.mode, 2.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            src = dst;
            step *= 2.0f;
        }
        s_dn_src[eye] = src;
    }

    /* 4. bilateral upsample to full res, mixed with the sharp scene colour by
     * the same intensity/frames weight the in-shader path uses.  The FX pass
     * then just receives this as its uColor. */
    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;
        int src = s_dn_src[eye];
        glViewport(0, 0, s_eye_w, s_eye_h);
        glBindFramebuffer(GL_FRAMEBUFFER, s_dn_ufbo[eye]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_eye_tex[eye]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_dn_col[eye][src]);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, s_dn_dep[eye]);
        glUniform1f(s_dn_u.mode, 3.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        s_dn_result[eye] = s_dn_up[eye];
        s_dn_valid[eye] = 1;
    }

    glDisableVertexAttribArray(s_dn_apos);
    glDisableVertexAttribArray(s_dn_auv);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* ---------------- Reduced-resolution C.DOF prepass -----------------------
 * Mirrors the denoiser shape (see s_dn_*): reduced-res targets, a dedicated
 * shader with modes, and a full-res upsample as its own pass.  Modes:
 *
 *   0  prepare: eye colour (bilinear box) + signed CoC in alpha   -> low col
 *   1  prepare: full-res depth -> packed linear z (RG)            -> low dep
 *   2  gather:  spiral / bokeh kernel at eye/div, tap CoC from alpha
 *   3  upsample: full-res depth-aware bilateral mix, sharp where CoC ~ 0
 *
 * The packed-z target is only drawn when the upsample or the legacy near test
 * needs it.  Every path is dialled (fx 380-389) so each can be A/B'd.
 */
static const char *s_dof_vs =
    "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *s_dof_fs =
    "precision highp float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uSrc;\n"        /* full-res eye colour (denoised if any) */
    "uniform sampler2D uSrcDepth;\n"   /* full-res eye depth */
    "uniform sampler2D uLowCol;\n"     /* eye/div colour, alpha = signed CoC */
    "uniform sampler2D uLowDep;\n"     /* eye/div packed linear z */
    "uniform sampler2D uLowBlur;\n"    /* eye/div gather result */
    "uniform float uMode;\n"
    "uniform vec2 uLowTexel;\n"
    "uniform vec2 uRes;\n"
    "uniform vec4 uCanvas;\n"
    "uniform float uDiv;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "uniform float uFocus;\n"
    "uniform float uAperture;\n"
    "uniform float uMaxBlur;\n"
    "uniform float uSamples;\n"
    "uniform float uUpSigma;\n"
    "uniform float uBokehOn;\n"
    "uniform float uBokehAspect;\n"
    "uniform float uBokehBlades;\n"
    "uniform float uBokehAnamorphic;\n"
    "uniform float uBokehCatEye;\n"
    "uniform float uBokehHighlight;\n"
    "uniform float uBokehRot;\n"
    "uniform float uNearMode;\n"
    "uniform float uCocTex;\n"
    "uniform float uBokehPre;\n"
    "uniform float uUpMode;\n"
    "uniform float uSharp;\n"
    "uniform vec2 uDofTap[32];\n"
    "uniform float uDofMask[32];\n"
    "float linZ(float d) {\n"
    "  float z = d * 2.0 - 1.0;\n"
    "  return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));\n"
    "}\n"
    "vec2 packZ(float z) {\n"
    "  float v = clamp(z / uFar, 0.0, 1.0) * 65025.0;\n"
    "  float hi = floor(v / 255.0);\n"
    "  return vec2(hi / 255.0, (v - hi * 255.0) / 255.0);\n"
    "}\n"
    "float unpackZ(vec4 c) {\n"
    "  return ((c.r * 255.0) * 255.0 + c.g * 255.0) / 65025.0 * uFar;\n"
    "}\n"
    "float cocOf(float z) {\n"
    "  return abs(z - uFocus) / max(z, 1.0) * (uAperture / 100.0) * 220.0;\n"
    "}\n"
    "void main() {\n"
    "  if (uMode < 0.5) {\n"
    /* bilinear at the reduced-res centre = 2x2 box average of the eye colour */
    "    vec3 c = texture2D(uSrc, vUV).rgb;\n"
    "    float z = linZ(texture2D(uSrcDepth, vUV).r);\n"
    "    float s = (z - uFocus) / max(z, 1.0) * (uAperture / 100.0) * 220.0;\n"
    "    gl_FragColor = vec4(c, 0.5 + 0.5 * clamp(s / max(uMaxBlur, 1.0), -1.0, 1.0));\n"
    "  } else if (uMode < 1.5) {\n"
    "    gl_FragColor = vec4(packZ(linZ(texture2D(uSrcDepth, vUV).r)), 0.0, 1.0);\n"
    "  } else if (uMode < 2.5) {\n"
    "    vec4 c0 = texture2D(uLowCol, vUV);\n"
    "    float ca = c0.a - 0.5;\n"
    "    float coc = abs(ca) * 2.0 * max(uMaxBlur, 1.0);\n"
    "    vec3 outc = c0.rgb;\n"
    "    if (coc > 0.5) {\n"
    "      float ns = max(uSamples, 1.0);\n"
    "      float segA = 6.2831853 / max(uBokehBlades, 3.0);\n"
    "      float edge = smoothstep(0.15, 1.0, abs(vUV.x * 2.0 - 1.0));\n"
    "      vec2 cmin = uCanvas.xy / uRes;\n"
    "      vec2 cmax = (uCanvas.xy + uCanvas.zw) / uRes;\n"
    "      float cz = 0.0;\n"
    "      if (uNearMode > 1.5) cz = unpackZ(texture2D(uLowDep, vUV));\n"
    "      vec3 acc = vec3(0.0);\n"
    "      float ws = 0.0;\n"
    "      for (int i = 0; i < 32; i++) {\n"
    "        if (float(i) >= ns) break;\n"
    "        vec2 p;\n"
    "        if (uBokehPre > 0.5) {\n"
    "          p = uDofTap[i];\n"
    "        } else {\n"
    "          float fi = (float(i) + 0.5) / ns;\n"
    "          float ang = float(i) * 2.39996323 + radians(uBokehRot);\n"
    "          p = vec2(cos(ang), sin(ang)) * sqrt(fi);\n"
    "        }\n"
    "        if (uBokehOn > 0.5) {\n"
    "          p.x *= (uBokehAspect / 100.0) * (uBokehAnamorphic / 100.0);\n"
    "          if (uBokehCatEye > 0.5) {\n"
    "            p.x *= 1.0 - (uBokehCatEye / 100.0) * 0.65 * edge;\n"
    "            float aa = mod(atan(p.y, p.x) + segA * 0.5, segA) - segA * 0.5;\n"
    "            float rpoly = cos(segA * 0.5) / max(cos(aa), 0.001);\n"
    "            if (length(p) > rpoly) continue;\n"
    "          } else if (uBokehPre > 0.5) {\n"
    "            if (uDofMask[i] < 0.5) continue;\n"
    "          } else {\n"
    "            float aa = mod(atan(p.y, p.x) + segA * 0.5, segA) - segA * 0.5;\n"
    "            float rpoly = cos(segA * 0.5) / max(cos(aa), 0.001);\n"
    "            if (length(p) > rpoly) continue;\n"
    "          }\n"
    "        }\n"
    "        vec2 suv = vUV + p * (coc / uDiv) * uLowTexel;\n"
    "        if (suv.x < cmin.x || suv.x > cmax.x || suv.y < cmin.y || suv.y > cmax.y) continue;\n"
    "        vec4 sc = texture2D(uLowCol, suv);\n"
    "        float tcs;\n"
    "        if (uCocTex > 0.5) {\n"
    "          tcs = sc.a - 0.5;\n"
    "        } else {\n"
    "          float tz = unpackZ(texture2D(uLowDep, suv));\n"
    "          tcs = clamp((tz - uFocus) / max(tz, 1.0) * (uAperture / 100.0) * 220.0\n"
    "                      / max(uMaxBlur, 1.0), -1.0, 1.0);\n"
    "        }\n"
    "        float w = 1.0;\n"
    "        if (uNearMode > 0.5 && uNearMode < 1.5) {\n"
    /* CoC-aware near stop: a sharp tap next to a blurred centre must not be
     * swept into (or over) the blur.  Same intent as the depth test below,
     * without a per-tap depth fetch. */
    "          if (abs(tcs) * 2.0 * max(uMaxBlur, 1.0) < 1.0 && coc > 1.0) w = 0.15;\n"
    "        } else if (uNearMode > 1.5) {\n"
    "          float tz2 = unpackZ(texture2D(uLowDep, suv));\n"
    "          if (tz2 < cz - 1.0 && abs(tcs) * 2.0 * max(uMaxBlur, 1.0) < 1.0) w = 0.15;\n"
    "        }\n"
    "        float lum = dot(sc.rgb, vec3(0.299, 0.587, 0.114));\n"
    "        w *= 1.0 + (uBokehHighlight / 100.0) * lum * lum * 2.0;\n"
    "        acc += sc.rgb * w;\n"
    "        ws += w;\n"
    "      }\n"
    "      outc = ws > 0.0 ? acc / ws : c0.rgb;\n"
    "    }\n"
    "    gl_FragColor = vec4(outc, 1.0);\n"
    "  } else {\n"
    "    vec3 sharp = texture2D(uSrc, vUV).rgb;\n"
    "    float ca = texture2D(uLowCol, vUV).a - 0.5;\n"
    "    float m = clamp(abs(ca) * 2.0 * max(uMaxBlur, 1.0), 0.0, 1.0) * uSharp;\n"
    "    vec3 blur = texture2D(uLowBlur, vUV).rgb;\n"
    "    if (uUpMode > 0.5) {\n"
    /* Four low-res taps at texel centres, weighted by bilinear coverage and
     * |z_tap - z_pixel| so the upsample does not smear across silhouettes. */
    "      vec2 low = 1.0 / uLowTexel;\n"
    "      vec2 p = vUV * low - 0.5;\n"
    "      vec2 f = p - floor(p);\n"
    "      vec2 base0 = (floor(p) + 0.5) / low;\n"
    "      float cz = unpackZ(texture2D(uLowDep, vUV));\n"
    "      vec3 acc = vec3(0.0);\n"
    "      float ws = 0.0;\n"
    "      for (int i = 0; i < 4; i++) {\n"
    "        float ox = (i == 1 || i == 3) ? 1.0 : 0.0;\n"
    "        float oy = (i >= 2) ? 1.0 : 0.0;\n"
    "        vec2 suv = base0 + vec2(ox, oy) * uLowTexel;\n"
    "        vec3 sc = texture2D(uLowBlur, suv).rgb;\n"
    "        float sz = unpackZ(texture2D(uLowDep, suv));\n"
    "        float bw = mix(1.0 - f.x, f.x, ox) * mix(1.0 - f.y, f.y, oy);\n"
    "        float w = bw * exp(-abs(sz - cz) * uUpSigma);\n"
    "        acc += sc * w;\n"
    "        ws += w;\n"
    "      }\n"
    "      if (ws > 0.0001) blur = acc / ws;\n"
    "    }\n"
    "    gl_FragColor = vec4(mix(sharp, blur, m), 1.0);\n"
    "  }\n"
    "}\n";

static GLuint s_dof_prog = 0;
static int    s_dof_prog_state = 0;   /* 0 untried, 1 ok, -1 failed */
static GLint  s_dof_apos = -1, s_dof_auv = -1;
static struct {
    GLint mode, src, src_depth, low_col, low_dep, low_blur;
    GLint low_texel, res, canvas, div, nearv, farv;
    GLint focus, aperture, maxblur, samples, up_sigma;
    GLint bokeh_on, bokeh_aspect, bokeh_blades, bokeh_anamorphic, bokeh_cat_eye;
    GLint bokeh_highlight, bokeh_rot;
    GLint near_mode, coc_tex, bokeh_pre, up_mode, sharp;
    GLint tap, mask;
} s_dof_u;
static GLuint s_dof_col[2] = {0, 0}, s_dof_cfbo[2] = {0, 0};
static GLuint s_dof_dep[2] = {0, 0}, s_dof_dfbo[2] = {0, 0};
static GLuint s_dof_bl[2] = {0, 0},  s_dof_bfbo[2] = {0, 0};
static GLuint s_dof_up[2] = {0, 0},  s_dof_ufbo[2] = {0, 0};
static int    s_dof_w = 0, s_dof_h = 0, s_dof_div = 0;
static int    s_dof_upw = 0, s_dof_uph = 0;
static int    s_dof_valid[2] = {0, 0};
static GLuint s_dof_result[2] = {0, 0};
/* Precomputed spiral taps (direction * sqrt(fi), 32 vec2) and the blade mask. */
static float  s_dof_tap[64];
static float  s_dof_mask[32];

static double dof_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1000000.0;
}

static void dof_build_taps(int n, int blades, float rot_deg, float aspect_pct,
                           float anam_pct) {
    const float segA = 6.28318530718f / (float) (blades < 3 ? 3 : blades);
    const float rot = rot_deg * 0.01745329252f;
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    for (int i = 0; i < 32; i++) {
        float ang = (float) i * 2.39996323f + rot;
        float fi = ((float) i + 0.5f) / (float) n;
        float s = sqrtf(fi);
        float px = cosf(ang) * s;
        float py = sinf(ang) * s;
        if (i < n) {
            s_dof_tap[i * 2 + 0] = px;
            s_dof_tap[i * 2 + 1] = py;
        } else {
            s_dof_tap[i * 2 + 0] = 0.0f;
            s_dof_tap[i * 2 + 1] = 0.0f;
        }
        float ax = px * (aspect_pct / 100.0f) * (anam_pct / 100.0f);
        float len = sqrtf(ax * ax + py * py);
        /* replicate the shader: aa = mod(atan(p.y,p.x) + segA/2, segA) - segA/2 */
        float d = atan2f(py, ax) + segA * 0.5f;
        d = d - segA * floorf(d / segA) - segA * 0.5f;
        float c = cosf(d);
        float rpoly = cosf(segA * 0.5f) / (c > 0.001f ? c : 0.001f);
        s_dof_mask[i] = (len <= rpoly) ? 1.0f : 0.0f;
    }
}

static int dof_ensure_program(void) {
    if (s_dof_prog_state != 0) return s_dof_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_dof_vs, "dof vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_dof_fs, "dof fragment");
    if (!vs || !fs) {
        s_dof_prog_state = -1;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    s_dof_prog = glCreateProgram();
    glAttachShader(s_dof_prog, vs);
    glAttachShader(s_dof_prog, fs);
    glLinkProgram(s_dof_prog);
    GLint ok = 0;
    glGetProgramiv(s_dof_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[1024];
        GLsizei len = 0;
        glGetProgramInfoLog(s_dof_prog, sizeof(log), &len, log);
        __android_log_print(ANDROID_LOG_ERROR, "SM64FX",
                "dof program link failed: %.*s", (int) len, log);
        glDeleteProgram(s_dof_prog);
        s_dof_prog = 0;
        s_dof_prog_state = -1;
        return 0;
    }
    s_dof_apos = glGetAttribLocation(s_dof_prog, "aPos");
    s_dof_auv = glGetAttribLocation(s_dof_prog, "aUV");
    s_dof_u.mode = glGetUniformLocation(s_dof_prog, "uMode");
    s_dof_u.src = glGetUniformLocation(s_dof_prog, "uSrc");
    s_dof_u.src_depth = glGetUniformLocation(s_dof_prog, "uSrcDepth");
    s_dof_u.low_col = glGetUniformLocation(s_dof_prog, "uLowCol");
    s_dof_u.low_dep = glGetUniformLocation(s_dof_prog, "uLowDep");
    s_dof_u.low_blur = glGetUniformLocation(s_dof_prog, "uLowBlur");
    s_dof_u.low_texel = glGetUniformLocation(s_dof_prog, "uLowTexel");
    s_dof_u.res = glGetUniformLocation(s_dof_prog, "uRes");
    s_dof_u.canvas = glGetUniformLocation(s_dof_prog, "uCanvas");
    s_dof_u.div = glGetUniformLocation(s_dof_prog, "uDiv");
    s_dof_u.nearv = glGetUniformLocation(s_dof_prog, "uNear");
    s_dof_u.farv = glGetUniformLocation(s_dof_prog, "uFar");
    s_dof_u.focus = glGetUniformLocation(s_dof_prog, "uFocus");
    s_dof_u.aperture = glGetUniformLocation(s_dof_prog, "uAperture");
    s_dof_u.maxblur = glGetUniformLocation(s_dof_prog, "uMaxBlur");
    s_dof_u.samples = glGetUniformLocation(s_dof_prog, "uSamples");
    s_dof_u.up_sigma = glGetUniformLocation(s_dof_prog, "uUpSigma");
    s_dof_u.bokeh_on = glGetUniformLocation(s_dof_prog, "uBokehOn");
    s_dof_u.bokeh_aspect = glGetUniformLocation(s_dof_prog, "uBokehAspect");
    s_dof_u.bokeh_blades = glGetUniformLocation(s_dof_prog, "uBokehBlades");
    s_dof_u.bokeh_anamorphic = glGetUniformLocation(s_dof_prog, "uBokehAnamorphic");
    s_dof_u.bokeh_cat_eye = glGetUniformLocation(s_dof_prog, "uBokehCatEye");
    s_dof_u.bokeh_highlight = glGetUniformLocation(s_dof_prog, "uBokehHighlight");
    s_dof_u.bokeh_rot = glGetUniformLocation(s_dof_prog, "uBokehRot");
    s_dof_u.near_mode = glGetUniformLocation(s_dof_prog, "uNearMode");
    s_dof_u.coc_tex = glGetUniformLocation(s_dof_prog, "uCocTex");
    s_dof_u.bokeh_pre = glGetUniformLocation(s_dof_prog, "uBokehPre");
    s_dof_u.up_mode = glGetUniformLocation(s_dof_prog, "uUpMode");
    s_dof_u.sharp = glGetUniformLocation(s_dof_prog, "uSharp");
    s_dof_u.tap = glGetUniformLocation(s_dof_prog, "uDofTap[0]");
    s_dof_u.mask = glGetUniformLocation(s_dof_prog, "uDofMask[0]");
    s_dof_prog_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX", "dof program ready (%u)", s_dof_prog);
    return 1;
}

static void dof_make_target(GLuint *tex, GLuint *fbo, int w, int h, int linear) {
    if (*fbo) glDeleteFramebuffers(1, fbo);
    if (*tex) glDeleteTextures(1, tex);
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    GLenum f = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, *tex, 0);
}

/* Reduced-res colour + packed depth + gather target per eye, plus the full-res
 * upsample destination. */
static void stereo_ensure_dof(int div) {
    int w = s_eye_w / div;
    int h = s_eye_h / div;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == s_dof_w && h == s_dof_h && div == s_dof_div && s_dof_cfbo[0] != 0
        && s_dof_upw == s_eye_w && s_dof_uph == s_eye_h) {
        return;
    }
    for (int eye = 0; eye < 2; eye++) {
        dof_make_target(&s_dof_col[eye], &s_dof_cfbo[eye], w, h,
                        s_fx_dof_bilinear ? 1 : 0);
        dof_make_target(&s_dof_dep[eye], &s_dof_dfbo[eye], w, h, 0);
        dof_make_target(&s_dof_bl[eye], &s_dof_bfbo[eye], w, h, 1);
        dof_make_target(&s_dof_up[eye], &s_dof_ufbo[eye], s_eye_w, s_eye_h, 1);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_dof_w = w;
    s_dof_h = h;
    s_dof_div = div;
    s_dof_upw = s_eye_w;
    s_dof_uph = s_eye_h;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX",
            "DOF fbo col=%u dep=%u blur=%u up=%u %dx%d div=%d (eye %dx%d)",
            s_dof_col[0], s_dof_dep[0], s_dof_bl[0], s_dof_up[0], w, h, div,
            s_eye_w, s_eye_h);
}

/* Returns 1 when the per-eye results in s_dof_result[] are valid. */
static int stereo_dof_prepass(int div) {
    s_dof_valid[0] = s_dof_valid[1] = 0;
    stereo_ensure_dof(div);
    if (!dof_ensure_program()) return 0;

    int taps = s_fx_dof_taps > 0 ? s_fx_dof_taps : s_fx_dof_samples;
    if (taps < 1) taps = 1;
    if (taps > 32) taps = 32;
    dof_build_taps(taps, s_fx_bokeh_blades, (float) s_fx_bokeh_rot,
                   (float) s_fx_bokeh_aspect, (float) s_fx_anamorphic);

    float dof_focus = s_fx_dof_auto ? gfx_dof_focus_auto : (float) s_fx_dof_focus;
    if (dof_focus < 1.0f) dof_focus = 1.0f;
    int maxblur = s_fx_dof_maxblur < 1 ? 1 : s_fx_dof_maxblur;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_dof_prog);

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(s_dof_apos);
    glVertexAttribPointer(s_dof_apos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *) 0);
    glEnableVertexAttribArray(s_dof_auv);
    glVertexAttribPointer(s_dof_auv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *) (2 * sizeof(float)));

    glUniform1i(s_dof_u.src, 0);
    glUniform1i(s_dof_u.src_depth, 1);
    glUniform1i(s_dof_u.low_col, 2);
    glUniform1i(s_dof_u.low_dep, 3);
    glUniform1i(s_dof_u.low_blur, 4);
    glUniform2f(s_dof_u.low_texel, 1.0f / (float) s_dof_w, 1.0f / (float) s_dof_h);
    glUniform2f(s_dof_u.res, (float) s_eye_w, (float) s_eye_h);
    glUniform4f(s_dof_u.canvas, (float) s_canvas_x, (float) s_canvas_y,
                (float) s_canvas_w, (float) s_canvas_h);
    glUniform1f(s_dof_u.div, (float) div);
    glUniform1f(s_dof_u.nearv, gfx_cam_near);
    glUniform1f(s_dof_u.farv, gfx_cam_far);
    glUniform1f(s_dof_u.focus, dof_focus);
    glUniform1f(s_dof_u.aperture, (float) s_fx_dof_aperture);
    glUniform1f(s_dof_u.maxblur, (float) maxblur);
    glUniform1f(s_dof_u.samples, (float) taps);
    /* One low-res texel covers `div` eye pixels; keep the edge stop at the
     * same screen-space tolerance the denoiser uses. */
    glUniform1f(s_dof_u.up_sigma, 0.25f / (float) div);
    glUniform1f(s_dof_u.bokeh_on, s_fx_bokeh_on ? 1.0f : 0.0f);
    glUniform1f(s_dof_u.bokeh_aspect, (float) s_fx_bokeh_aspect);
    glUniform1f(s_dof_u.bokeh_blades, (float) s_fx_bokeh_blades);
    glUniform1f(s_dof_u.bokeh_anamorphic, (float) s_fx_anamorphic);
    glUniform1f(s_dof_u.bokeh_cat_eye, (float) s_fx_cat_eye);
    glUniform1f(s_dof_u.bokeh_highlight, (float) s_fx_bokeh_highlight);
    glUniform1f(s_dof_u.bokeh_rot, (float) s_fx_bokeh_rot);
    glUniform1f(s_dof_u.near_mode, (float) s_fx_dof_near);
    glUniform1f(s_dof_u.coc_tex, s_fx_dof_coc ? 1.0f : 0.0f);
    glUniform1f(s_dof_u.bokeh_pre, s_fx_dof_bokeh_pre ? 1.0f : 0.0f);
    glUniform1f(s_dof_u.up_mode, s_fx_dof_up ? 1.0f : 0.0f);
    glUniform1f(s_dof_u.sharp, (float) s_fx_dof_sharp / 100.0f);
    if (s_dof_u.tap >= 0) glUniform2fv(s_dof_u.tap, 32, s_dof_tap);
    if (s_dof_u.mask >= 0) glUniform1fv(s_dof_u.mask, 32, s_dof_mask);

    /* The bilinear dial only changes filtering, not the target size, so the
     * ensure function cannot see it change.  Re-apply it here. */
    for (int eye = 0; eye < 2; eye++) {
        GLenum f = s_fx_dof_bilinear ? GL_LINEAR : GL_NEAREST;
        glBindTexture(GL_TEXTURE_2D, s_dof_col[eye]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    int stage = s_fx_dof_stage;
    double t0 = 0.0, t1 = 0.0, t2 = 0.0, t3 = 0.0;
    if (stage) { glFinish(); t0 = dof_now_ms(); }

    /* 1. prepare colour + signed CoC, and packed linear z when needed. */
    int need_z = (s_fx_dof_up != 0) || (s_fx_dof_near >= 2) || (s_fx_dof_coc == 0);
    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;
        GLuint src = s_dn_valid[eye] ? s_dn_result[eye] : s_eye_tex[eye];
        glViewport(0, 0, s_dof_w, s_dof_h);
        glBindFramebuffer(GL_FRAMEBUFFER, s_dof_cfbo[eye]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_eye_depth[eye]);
        glUniform1f(s_dof_u.mode, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (need_z) {
            glBindFramebuffer(GL_FRAMEBUFFER, s_dof_dfbo[eye]);
            glUniform1f(s_dof_u.mode, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
    if (stage) { glFinish(); t1 = dof_now_ms(); }

    /* 2. gather at reduced resolution.  A blur target with no depth pass reads
     * garbage if the dials asked for no z; bind a valid texture anyway. */
    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;
        glViewport(0, 0, s_dof_w, s_dof_h);
        glBindFramebuffer(GL_FRAMEBUFFER, s_dof_bfbo[eye]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_dof_col[eye]);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, s_dof_dep[eye]);
        glUniform1f(s_dof_u.mode, 2.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (stage) { glFinish(); t2 = dof_now_ms(); }

    /* 3. full-res depth-aware upsample, mixed with the sharp scene colour. */
    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;
        GLuint src = s_dn_valid[eye] ? s_dn_result[eye] : s_eye_tex[eye];
        glViewport(0, 0, s_eye_w, s_eye_h);
        glBindFramebuffer(GL_FRAMEBUFFER, s_dof_ufbo[eye]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_dof_col[eye]);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, s_dof_dep[eye]);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, s_dof_bl[eye]);
        glUniform1f(s_dof_u.mode, 3.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        s_dof_result[eye] = s_dof_up[eye];
        s_dof_valid[eye] = 1;
    }
    if (stage) { glFinish(); t3 = dof_now_ms(); }

    glDisableVertexAttribArray(s_dof_apos);
    glDisableVertexAttribArray(s_dof_auv);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (stage && (frame_count % 30) == 0) {
        __android_log_print(ANDROID_LOG_INFO, "SM64FX",
                "DOFSTAGE prep=%.2f gather=%.2f up=%.2f div=%d taps=%d eye=%dx%d low=%dx%d",
                t1 - t0, t2 - t1, t3 - t2, div, taps, s_eye_w, s_eye_h,
                s_dof_w, s_dof_h);
    }
    return (s_dof_valid[0] || s_dof_valid[1]) ? 1 : 0;
}

/* ---- Reduced-resolution SSGI passes (shader + dials above) ----
 * Prep: colour + packed z/normal at 1/div, gather, optional blur.
 * Composite: full-res bilateral upsample added into the FX output.
 * Nothing is allocated until the chain is actually used. */
static int gi_ensure_program(void) {
    if (s_gi_prog_state != 0) return s_gi_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_gi_vs, "gi vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_gi_fs, "gi fragment");
    if (!vs || !fs) {
        s_gi_prog_state = -1;
        return 0;
    }
    s_gi_prog = glCreateProgram();
    glAttachShader(s_gi_prog, vs);
    glAttachShader(s_gi_prog, fs);
    glLinkProgram(s_gi_prog);
    GLint ok = 0;
    glGetProgramiv(s_gi_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(s_gi_prog, sizeof(log), NULL, log);
        __android_log_print(ANDROID_LOG_ERROR, "SM64FX", "gi link: %s", log);
        glDeleteProgram(s_gi_prog);
        s_gi_prog = 0;
        s_gi_prog_state = -1;
        return 0;
    }
    s_gi_apos = glGetAttribLocation(s_gi_prog, "aPos");
    s_gi_auv = glGetAttribLocation(s_gi_prog, "aUV");
    s_gi_u.color = glGetUniformLocation(s_gi_prog, "uColor");
    s_gi_u.depth = glGetUniformLocation(s_gi_prog, "uDepth");
    s_gi_u.col = glGetUniformLocation(s_gi_prog, "uCol");
    s_gi_u.dep = glGetUniformLocation(s_gi_prog, "uDep");
    s_gi_u.srcg = glGetUniformLocation(s_gi_prog, "uSrcG");
    s_gi_u.res = glGetUniformLocation(s_gi_prog, "uRes");
    s_gi_u.low = glGetUniformLocation(s_gi_prog, "uLow");
    s_gi_u.canvas = glGetUniformLocation(s_gi_prog, "uCanvas");
    s_gi_u.nearv = glGetUniformLocation(s_gi_prog, "uNear");
    s_gi_u.farv = glGetUniformLocation(s_gi_prog, "uFar");
    s_gi_u.tanhalf = glGetUniformLocation(s_gi_prog, "uTanHalf");
    s_gi_u.aspect = glGetUniformLocation(s_gi_prog, "uAspect");
    s_gi_u.mode = glGetUniformLocation(s_gi_prog, "uMode");
    s_gi_u.radius = glGetUniformLocation(s_gi_prog, "uRadius");
    s_gi_u.steps = glGetUniformLocation(s_gi_prog, "uSteps");
    s_gi_u.rays = glGetUniformLocation(s_gi_prog, "uRays");
    s_gi_u.jitter = glGetUniformLocation(s_gi_prog, "uJitter");
    s_gi_u.frame = glGetUniformLocation(s_gi_prog, "uFrame");
    s_gi_u.skip = glGetUniformLocation(s_gi_prog, "uSkip");
    s_gi_u.norm = glGetUniformLocation(s_gi_prog, "uNorm");
    s_gi_u.flipy = glGetUniformLocation(s_gi_prog, "uFlipY");
    s_gi_u.shear = glGetUniformLocation(s_gi_prog, "uShear");
    s_gi_u.toe = glGetUniformLocation(s_gi_prog, "uToe");
    s_gi_u.step = glGetUniformLocation(s_gi_prog, "uStep");
    s_gi_u.gain = glGetUniformLocation(s_gi_prog, "uGain");
    s_gi_u.debug = glGetUniformLocation(s_gi_prog, "uDebug");
    s_gi_prog_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX", "gi program ready (%u)", s_gi_prog);
    return 1;
}

static void stereo_ensure_gi(int div) {
    int cw = s_canvas_w > 0 ? s_canvas_w : s_eye_w;
    int ch = s_canvas_h > 0 ? s_canvas_h : s_eye_h;
    int w = cw / div;
    int h = ch / div;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w == s_gi_w && h == s_gi_h && div == s_gi_div && s_gi_fbo[0][0] != 0) return;
    for (int eye = 0; eye < 2; eye++) {
        for (int k = 0; k < 4; k++) {
            if (s_gi_fbo[eye][k]) glDeleteFramebuffers(1, &s_gi_fbo[eye][k]);
            if (s_gi_tex[eye][k]) glDeleteTextures(1, &s_gi_tex[eye][k]);
            glGenTextures(1, &s_gi_tex[eye][k]);
            glBindTexture(GL_TEXTURE_2D, s_gi_tex[eye][k]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
            /* Low-res colour is sampled bilinearly by the gather; packed
             * depth/normal and the GI result must stay unfiltered. */
            GLint filt = k == 0 ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &s_gi_fbo[eye][k]);
            glBindFramebuffer(GL_FRAMEBUFFER, s_gi_fbo[eye][k]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, s_gi_tex[eye][k], 0);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_gi_w = w;
    s_gi_h = h;
    s_gi_div = div;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX",
            "GI fbo %dx%d div=%d (eye %dx%d canvas %dx%d)",
            w, h, div, s_eye_w, s_eye_h, cw, ch);
}

/* Downsample + gather + blur per eye; leaves s_gi_valid set for the composite. */
static void stereo_gi_prepass(int div) {
    s_gi_valid[0] = s_gi_valid[1] = 0;
    stereo_ensure_gi(div);
    if (!gi_ensure_program()) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_gi_prog);

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(s_gi_apos);
    glVertexAttribPointer(s_gi_apos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(s_gi_auv);
    glVertexAttribPointer(s_gi_auv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    glUniform1i(s_gi_u.color, 0);
    glUniform1i(s_gi_u.depth, 1);
    glUniform1i(s_gi_u.col, 2);
    glUniform1i(s_gi_u.dep, 3);
    glUniform1i(s_gi_u.srcg, 4);
    glUniform2f(s_gi_u.res, (float)s_eye_w, (float)s_eye_h);
    glUniform2f(s_gi_u.low, (float)s_gi_w, (float)s_gi_h);
    glUniform4f(s_gi_u.canvas, (float)s_canvas_x, (float)s_canvas_y,
                (float)s_canvas_w, (float)s_canvas_h);
    glUniform1f(s_gi_u.nearv, gfx_cam_near);
    glUniform1f(s_gi_u.farv, gfx_cam_far);
    glUniform1f(s_gi_u.tanhalf, gfx_cam_tan_half_fov_y);
    glUniform1f(s_gi_u.aspect, (float)s_canvas_w / (float)(s_canvas_h ? s_canvas_h : 1));
    glUniform1f(s_gi_u.radius, (float)s_fx_gi_radius);
    glUniform1f(s_gi_u.steps, (float)s_fx_gi_steps);
    glUniform1f(s_gi_u.rays, (float)s_fx_gi_rays);
    glUniform1f(s_gi_u.jitter, s_fx_gi_jitter ? 1.0f : 0.0f);
    glUniform1f(s_gi_u.frame, (float)(frame_count & 1023));
    glUniform1f(s_gi_u.skip, (float)s_fx_gi_skip);
    glUniform1f(s_gi_u.norm, s_fx_gi_norm ? 1.0f : 0.0f);

    glViewport(0, 0, s_gi_w, s_gi_h);
    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;

        glUniform1f(s_gi_u.shear, gfx_eye_shear[eye]);
        glUniform1f(s_gi_u.toe, gfx_eye_toe[eye]);
        glUniform1f(s_gi_u.flipy, gfx_eye_flip[eye] ? 1.0f : 0.0f);
        GLuint scene = s_dn_valid[eye] ? s_dn_result[eye] : s_eye_tex[eye];

        /* 1. scene colour -> low-res colour */
        glBindFramebuffer(GL_FRAMEBUFFER, s_gi_fbo[eye][0]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene);
        glUniform1f(s_gi_u.mode, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        /* 2. depth -> low-res packed z + view normal */
        glBindFramebuffer(GL_FRAMEBUFFER, s_gi_fbo[eye][1]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_eye_depth[eye]);
        glUniform1f(s_gi_u.mode, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        /* 3. the gather, at low res */
        glBindFramebuffer(GL_FRAMEBUFFER, s_gi_fbo[eye][2]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_gi_tex[eye][0]);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, s_gi_tex[eye][1]);
        glUniform1f(s_gi_u.mode, 2.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        /* 4. optional edge-aware blur at the same low res */
        int src = 2;
        int passes = s_fx_gi_blur < 0 ? 0 : (s_fx_gi_blur > 3 ? 3 : s_fx_gi_blur);
        for (int p = 0; p < passes; p++) {
            int dst = src == 2 ? 3 : 2;
            glBindFramebuffer(GL_FRAMEBUFFER, s_gi_fbo[eye][dst]);
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, s_gi_tex[eye][src]);
            glUniform1f(s_gi_u.step, (float)(1 << p));
            glUniform1f(s_gi_u.mode, 3.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            src = dst;
        }
        s_gi_src[eye] = src;
        s_gi_valid[eye] = 1;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
}

/* Full-res bilateral upsample added into the FX output (or a debug view). */
static void stereo_gi_composite(void) {
    if (!s_gi_valid[0] && !s_gi_valid[1]) return;
    if (s_gi_prog_state != 1) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_gi_prog);
    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(s_gi_apos);
    glVertexAttribPointer(s_gi_apos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(s_gi_auv);
    glVertexAttribPointer(s_gi_auv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    glUniform1i(s_gi_u.depth, 1);
    glUniform1i(s_gi_u.dep, 3);
    glUniform1i(s_gi_u.srcg, 4);
    glUniform2f(s_gi_u.res, (float)s_eye_w, (float)s_eye_h);
    glUniform2f(s_gi_u.low, (float)s_gi_w, (float)s_gi_h);
    glUniform4f(s_gi_u.canvas, (float)s_canvas_x, (float)s_canvas_y,
                (float)s_canvas_w, (float)s_canvas_h);
    glUniform1f(s_gi_u.nearv, gfx_cam_near);
    glUniform1f(s_gi_u.farv, gfx_cam_far);
    glUniform1f(s_gi_u.tanhalf, gfx_cam_tan_half_fov_y);
    glUniform1f(s_gi_u.aspect, (float)s_canvas_w / (float)(s_canvas_h ? s_canvas_h : 1));
    glUniform1f(s_gi_u.radius, (float)s_fx_gi_radius);
    glUniform1f(s_gi_u.gain, (float)s_fx_gi_intensity * 0.6f / 100.0f);
    glUniform1f(s_gi_u.debug, (float)s_fx_gi_debug);

    int debug = s_fx_gi_debug > 0;
    if (debug) {
        glDisable(GL_BLEND);
    } else {
        /* Additive: the FX pass already wrote dof*ao*shadow + lights + grade,
         * exactly like the bloom chain below. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
    }
    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;
        if (!s_gi_valid[eye]) continue;
        glBindFramebuffer(GL_FRAMEBUFFER, s_pp_fbo[eye]);
        glViewport(0, 0, s_eye_w, s_eye_h);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_eye_depth[eye]);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, s_gi_tex[eye][1]);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, s_gi_tex[eye][s_gi_src[eye]]);
        glUniform1f(s_gi_u.mode, debug ? 5.0f : 4.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisable(GL_BLEND);
    glDisableVertexAttribArray(s_gi_apos);
    glDisableVertexAttribArray(s_gi_auv);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
}

/* ---------------- Half-resolution AO (fx 390-396) ------------------------
 * The legacy MXAO is a full-res 12-tap depth gather inside the big FX shader:
 * a 3-tap normal reconstruction, a sin/cos pair per tap, a divide per tap and
 * no blur, no sky early-out, at 1280x1534 per eye.  This chain moves the whole
 * thing to 1/div resolution in three small passes (the s_dn_* pattern) and
 * hands the FX shader a colour buffer that already has AO folded in, so the
 * big shader needs no extra sampler, no extra fetch and no AO block at all:
 *
 *   mode 0  prepass: full-res depth -> low-res RGBA8 (packed linear z, oct N)
 *   mode 1  gather:  low-res spiral AO -> low-res RGBA8 (ao, packed linear z)
 *   mode 2  apply:   depth-aware 4-tap upsample, multiplies the scene colour
 *   mode 3  apply:   plain bilinear upsample (1 tap, no depth test)
 *
 * The upsample is its own pass: BACKLOG records that folding a 4-tap
 * reduced-res upsample into the big shader cost ~30 ms, and that adding a
 * seventh sampler there slowed the whole post chain.
 *
 * The gather takes the tap offsets from a golden-angle spiral rotated by a
 * per-pixel interleaved-gradient-noise angle (uJitter 1, static: no temporal
 * shimmer) or by the frame counter (uJitter 0, the legacy behaviour).  The
 * spiral is advanced with a rotation matrix instead of cos/sin per tap
 * (uTrig 0); uTrig 1 keeps the per-tap trig for an A/B.
 */
static const char *s_ao_vs =
    "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *s_ao_fs =
    "precision highp float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uDepth;\n"   /* full-res eye depth            (mode 0) */
    "uniform sampler2D uZD;\n"      /* low-res packed z + oct normal (mode 1) */
    "uniform sampler2D uAO;\n"      /* low-res ao + packed z         (mode 2/3) */
    "uniform sampler2D uColor;\n"   /* full-res scene colour         (mode 2/3) */
    "uniform vec2 uRes;\n"          /* eye buffer, pixels                      */
    "uniform vec2 uLow;\n"          /* low-res buffer, pixels                  */
    "uniform vec2 uTexel;\n"        /* 1 / uRes                                */
    "uniform vec4 uCanvas;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "uniform float uTanHalf;\n"
    "uniform float uAspect;\n"
    "uniform float uShear;\n"
    "uniform float uToe;\n"
    "uniform float uFlipY;\n"
    "uniform float uNormalFlip;\n"
    "uniform float uMode;\n"
    "uniform float uAORadius;\n"
    "uniform float uAOSamples;\n"
    "uniform float uAOBias;\n"
    "uniform float uAOPower;\n"
    "uniform float uAOIntensity;\n"
    "uniform float uFrame;\n"
    "uniform float uJitter;\n"
    "uniform float uSky;\n"
    "uniform float uTrig;\n"
    "uniform float uUpSigma;\n"
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
    /* 16-bit-ish fixed point: 65025 steps over 0..far, ~0.05 world units. */
    "vec2 packZ(float z) {\n"
    "  float v = clamp(z / uFar, 0.0, 1.0) * 65025.0;\n"
    "  float hi = floor(v / 255.0);\n"
    "  float lo = v - hi * 255.0;\n"
    "  return vec2(hi / 255.0, lo / 255.0);\n"
    "}\n"
    "float unpackZ(vec4 c) {\n"
    "  return ((c.r * 255.0) * 255.0 + c.g * 255.0) / 65025.0 * uFar;\n"
    "}\n"
    "float unpackZB(vec4 c) {\n"
    "  return ((c.g * 255.0) * 255.0 + c.b * 255.0) / 65025.0 * uFar;\n"
    "}\n"
    /* Octahedral normal in two 8-bit channels (view space, z >= 0). */
    "vec2 octEncode(vec3 n) {\n"
    "  n /= max(abs(n.x) + abs(n.y) + abs(n.z), 0.0001);\n"
    "  if (n.z < 0.0) {\n"
    "    n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,\n"
    "                                    n.y >= 0.0 ? 1.0 : -1.0);\n"
    "  }\n"
    "  return n.xy * 0.5 + 0.5;\n"
    "}\n"
    "vec3 octDecode(vec2 e) {\n"
    "  vec2 f = e * 2.0 - 1.0;\n"
    "  vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));\n"
    "  float t = max(-n.z, 0.0);\n"
    "  n.x += (n.x >= 0.0) ? -t : t;\n"
    "  n.y += (n.y >= 0.0) ? -t : t;\n"
    "  return normalize(n);\n"
    "}\n"
    "float ign(vec2 p) {\n"
    "  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));\n"
    "}\n"
    "void main() {\n"
    "  if (uMode < 0.5) {\n"
    /* ---- prepass: depth + view normal once, at reduced resolution ---- */
    "    float z = linZ(texture2D(uDepth, vUV).r);\n"
    "    vec3 P = viewPos(canvasUV(vUV), z);\n"
    "    float zx = linZ(texture2D(uDepth, vUV + vec2(uTexel.x, 0.0)).r);\n"
    "    float zy = linZ(texture2D(uDepth, vUV + vec2(0.0, uTexel.y)).r);\n"
    "    vec3 Px = viewPos(canvasUV(vUV + vec2(uTexel.x, 0.0)), zx);\n"
    "    vec3 Py = viewPos(canvasUV(vUV + vec2(0.0, uTexel.y)), zy);\n"
    "    vec3 N = normalize(cross(Px - P, Py - P));\n"
    "    if (N.z < 0.0) N = -N;\n"
    "    if (uNormalFlip > 0.5) N = -N;\n"
    "    gl_FragColor = vec4(packZ(z), octEncode(N));\n"
    "  } else if (uMode < 1.5) {\n"
    /* ---- gather: golden-angle spiral, rotated per pixel ---- */
    "    vec4 c0 = texture2D(uZD, vUV);\n"
    "    float z = unpackZ(c0);\n"
    "    vec3 N = octDecode(c0.ba);\n"
    "    vec2 sc0 = canvasUV(vUV);\n"
    "    float occ = 0.0;\n"
    "    float wsum = 0.0;\n"
    "    bool offscreen = (sc0.x < 0.0 || sc0.x > 1.0 || sc0.y < 0.0 || sc0.y > 1.0);\n"
    "    bool sky = (uSky > 0.5) && (z > uFar * 0.95);\n"
    "    if (!offscreen && !sky) {\n"
    "      vec3 P = viewPos(sc0, z);\n"
    "      float ns = max(uAOSamples, 1.0);\n"
    "      float invn = 1.0 / ns;\n"
    "      float ang = (uJitter > 0.5) ? ign(vUV * uLow) * 6.2831853\n"
    "                                 : uFrame * 0.618;\n"
    "      vec2 dir = vec2(cos(ang), sin(ang));\n"
    "      float izt = 1.0 / (z * uTanHalf);\n"
    "      float ia = 1.0 / uAspect;\n"
    "      vec2 csuv = vec2(uCanvas.z, uCanvas.w) / uRes;\n"
    "      float maxd = uAORadius * 2.0;\n"
    "      for (int i = 0; i < 32; i++) {\n"
    "        if (float(i) >= ns) break;\n"
    "        float fi = (float(i) + 0.5) * invn;\n"
    "        vec2 ndcOff = vec2(dir.x * ia, dir.y) * (uAORadius * fi * izt);\n"
    "        vec2 suv = vUV + ndcOff * 0.5 * csuv;\n"
    "        wsum += 1.0;\n"
    "        if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0) {\n"
    "          vec2 scuv = canvasUV(suv);\n"
    "          if (scuv.x >= 0.0 && scuv.x <= 1.0 && scuv.y >= 0.0 && scuv.y <= 1.0) {\n"
    "            float sz = unpackZ(texture2D(uZD, suv));\n"
    "            if (abs(sz - z) >= uAOBias) {\n"
    "              vec3 d = viewPos(scuv, sz) - P;\n"
    "              float dist = length(d);\n"
    "              if (dist > 0.001 && dist < maxd) {\n"
    "                float ndl = max(dot(N, d / dist), 0.0);\n"
    "                float fall = 1.0 - dist / maxd;\n"
    "                occ += ndl * fall * fall;\n"
    "              }\n"
    "            }\n"
    "          }\n"
    "        }\n"
    "        if (uTrig < 0.5) {\n"
    /* rotate by the golden angle with a matrix: no trig in the loop */
    "          dir = vec2(dir.x * -0.7373689 - dir.y * 0.6754903,\n"
    "                     dir.x * 0.6754903 + dir.y * -0.7373689);\n"
    "        } else {\n"
    "          float a2 = ang + float(i + 1) * 2.39996323;\n"
    "          dir = vec2(cos(a2), sin(a2));\n"
    "        }\n"
    "      }\n"
    "      float amount = clamp(occ / max(wsum, 1.0) * (uAOIntensity / 100.0) * 3.0,\n"
    "                           0.0, 1.0);\n"
    "      float ao = pow(1.0 - amount, max(uAOPower, 1.0) / 100.0);\n"
    "      gl_FragColor = vec4(ao, packZ(z), 1.0);\n"
    "    } else {\n"
    "      gl_FragColor = vec4(1.0, packZ(z), 1.0);\n"
    "    }\n"
    "  } else if (uMode < 2.5) {\n"
    /* ---- apply: depth-aware 4-tap upsample, fold into the colour ---- */
    "    vec2 upstep = 1.0 / uLow;\n"
    "    vec2 p = vUV * uLow - 0.5;\n"
    "    vec2 f = p - floor(p);\n"
    "    vec2 base = (floor(p) + 0.5) / uLow;\n"
    "    float cz = 0.0;\n"
    "    float bws = 0.0;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      float ox = (i == 1 || i == 3) ? 1.0 : 0.0;\n"
    "      float oy = (i >= 2) ? 1.0 : 0.0;\n"
    "      vec2 suv = base + vec2(ox, oy) * upstep;\n"
    "      float bw = mix(1.0 - f.x, f.x, ox) * mix(1.0 - f.y, f.y, oy);\n"
    "      cz += bw * unpackZB(texture2D(uAO, suv));\n"
    "      bws += bw;\n"
    "    }\n"
    "    cz = bws > 0.0001 ? cz / bws : uFar;\n"
    "    float acc = 0.0;\n"
    "    float ws = 0.0;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      float ox = (i == 1 || i == 3) ? 1.0 : 0.0;\n"
    "      float oy = (i >= 2) ? 1.0 : 0.0;\n"
    "      vec2 suv = base + vec2(ox, oy) * upstep;\n"
    "      vec4 t = texture2D(uAO, suv);\n"
    "      float bw = mix(1.0 - f.x, f.x, ox) * mix(1.0 - f.y, f.y, oy);\n"
    "      float w = bw * exp(-abs(unpackZB(t) - cz) * uUpSigma);\n"
    "      acc += t.r * w;\n"
    "      ws += w;\n"
    "    }\n"
    "    float ao = ws > 0.0001 ? acc / ws : 1.0;\n"
    "    gl_FragColor = vec4(texture2D(uColor, vUV).rgb * ao, 1.0);\n"
    "  } else {\n"
    /* ---- apply: plain bilinear, for an A/B without the 4 taps ---- */
    "    float ao = texture2D(uAO, vUV).r;\n"
    "    gl_FragColor = vec4(texture2D(uColor, vUV).rgb * ao, 1.0);\n"
    "  }\n"
    "}\n";

/* ---------------- Half-resolution AO targets (fx 390-396) ---------------- */
static GLuint s_ao_prog = 0;
static int    s_ao_prog_state = 0;       // 0 = untried, 1 = ok, -1 = failed
static GLint  s_ao_apos = -1, s_ao_auv = -1;
static struct {
    GLint depth, zd, ao, color;
    GLint res, low, texel, canvas;
    GLint nearv, farv, tanhalf, aspect, shear, toe, flipy, normalflip;
    GLint mode, radius, samples, bias, power, intensity;
    GLint frame, jitter, sky, trig, upsigma;
} s_ao_u;
static GLuint s_ao_zdfbo[2] = {0, 0};    // low-res packed z + oct normal
static GLuint s_ao_zd[2] = {0, 0};
static GLuint s_ao_fbo[2] = {0, 0};      // low-res ao + packed z
static GLuint s_ao_tex[2] = {0, 0};
static GLuint s_ao_ofbo[2] = {0, 0};     // full-res colour with AO folded in
static GLuint s_ao_otex[2] = {0, 0};
static int    s_ao_w = 0, s_ao_h = 0, s_ao_div = 0;
static int    s_ao_valid[2] = {0, 0};    // ofbo holds this frame's AO colour

/* ---------------- Half-resolution AO prepass (fx 390-396) ----------------
 * Three passes per eye over the eye depth/colour fields, at 1/div resolution
 * for the two that cost anything:
 *
 *   0 prepass  full-res depth  -> low-res (packed linear z, octahedral normal)
 *   1 gather   low-res taps    -> low-res (ao, packed linear z)
 *   2/3 apply  low-res ao      -> full-res scene colour * ao
 *
 * s_ao_valid[eye] means s_ao_otex[eye] holds this frame's AO'd colour, which
 * the FX pass (or the composite, when AO is the only effect) reads instead of
 * the raw scene colour.  Every stage is skipped when s_fx_ao_probe asks for a
 * shorter chain, which is how the stage cost is measured from the dials.
 */
static int ao_ensure_program(void) {
    if (s_ao_prog_state != 0) return s_ao_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_ao_vs, "ao vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_ao_fs, "ao fragment");
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        s_ao_prog_state = -1;
        return 0;
    }
    s_ao_prog = glCreateProgram();
    glAttachShader(s_ao_prog, vs);
    glAttachShader(s_ao_prog, fs);
    glLinkProgram(s_ao_prog);
    GLint ok = 0;
    glGetProgramiv(s_ao_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        glDeleteProgram(s_ao_prog);
        s_ao_prog = 0;
        s_ao_prog_state = -1;
        return 0;
    }
    s_ao_apos = glGetAttribLocation(s_ao_prog, "aPos");
    s_ao_auv = glGetAttribLocation(s_ao_prog, "aUV");
    s_ao_u.depth = glGetUniformLocation(s_ao_prog, "uDepth");
    s_ao_u.zd = glGetUniformLocation(s_ao_prog, "uZD");
    s_ao_u.ao = glGetUniformLocation(s_ao_prog, "uAO");
    s_ao_u.color = glGetUniformLocation(s_ao_prog, "uColor");
    s_ao_u.res = glGetUniformLocation(s_ao_prog, "uRes");
    s_ao_u.low = glGetUniformLocation(s_ao_prog, "uLow");
    s_ao_u.texel = glGetUniformLocation(s_ao_prog, "uTexel");
    s_ao_u.canvas = glGetUniformLocation(s_ao_prog, "uCanvas");
    s_ao_u.nearv = glGetUniformLocation(s_ao_prog, "uNear");
    s_ao_u.farv = glGetUniformLocation(s_ao_prog, "uFar");
    s_ao_u.tanhalf = glGetUniformLocation(s_ao_prog, "uTanHalf");
    s_ao_u.aspect = glGetUniformLocation(s_ao_prog, "uAspect");
    s_ao_u.shear = glGetUniformLocation(s_ao_prog, "uShear");
    s_ao_u.toe = glGetUniformLocation(s_ao_prog, "uToe");
    s_ao_u.flipy = glGetUniformLocation(s_ao_prog, "uFlipY");
    s_ao_u.normalflip = glGetUniformLocation(s_ao_prog, "uNormalFlip");
    s_ao_u.mode = glGetUniformLocation(s_ao_prog, "uMode");
    s_ao_u.radius = glGetUniformLocation(s_ao_prog, "uAORadius");
    s_ao_u.samples = glGetUniformLocation(s_ao_prog, "uAOSamples");
    s_ao_u.bias = glGetUniformLocation(s_ao_prog, "uAOBias");
    s_ao_u.power = glGetUniformLocation(s_ao_prog, "uAOPower");
    s_ao_u.intensity = glGetUniformLocation(s_ao_prog, "uAOIntensity");
    s_ao_u.frame = glGetUniformLocation(s_ao_prog, "uFrame");
    s_ao_u.jitter = glGetUniformLocation(s_ao_prog, "uJitter");
    s_ao_u.sky = glGetUniformLocation(s_ao_prog, "uSky");
    s_ao_u.trig = glGetUniformLocation(s_ao_prog, "uTrig");
    s_ao_u.upsigma = glGetUniformLocation(s_ao_prog, "uUpSigma");
    s_ao_prog_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX",
            "half-res AO program ready (%u)", s_ao_prog);
    return 1;
}

static void stereo_ensure_ao(int div) {
    int w = s_eye_w / div;
    int h = s_eye_h / div;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == s_ao_w && h == s_ao_h && s_ao_div == div && s_ao_fbo[0] != 0) return;
    for (int i = 0; i < 2; i++) {
        if (s_ao_zdfbo[i]) glDeleteFramebuffers(1, &s_ao_zdfbo[i]);
        if (s_ao_zd[i]) glDeleteTextures(1, &s_ao_zd[i]);
        glGenTextures(1, &s_ao_zd[i]);
        glBindTexture(GL_TEXTURE_2D, s_ao_zd[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        /* Packed z: never blend it, the fixed-point halves do not interpolate. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s_ao_zdfbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_ao_zdfbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_ao_zd[i], 0);

        if (s_ao_fbo[i]) glDeleteFramebuffers(1, &s_ao_fbo[i]);
        if (s_ao_tex[i]) glDeleteTextures(1, &s_ao_tex[i]);
        glGenTextures(1, &s_ao_tex[i]);
        glBindTexture(GL_TEXTURE_2D, s_ao_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        /* LINEAR so the uAO 1-tap apply mode gets hardware bilinear. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s_ao_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_ao_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_ao_tex[i], 0);

        if (s_ao_ofbo[i]) glDeleteFramebuffers(1, &s_ao_ofbo[i]);
        if (s_ao_otex[i]) glDeleteTextures(1, &s_ao_otex[i]);
        glGenTextures(1, &s_ao_otex[i]);
        glBindTexture(GL_TEXTURE_2D, s_ao_otex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, s_eye_w, s_eye_h, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s_ao_ofbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_ao_ofbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_ao_otex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_ao_w = w;
    s_ao_h = h;
    s_ao_div = div;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX",
            "AO fbo eye0 zd=%u ao=%u out=%u %dx%d div=%d (eye %dx%d)",
            s_ao_zd[0], s_ao_tex[0], s_ao_otex[0], w, h, div, s_eye_w, s_eye_h);
}

static void stereo_ao_prepass(int div) {
    s_ao_valid[0] = s_ao_valid[1] = 0;
    stereo_ensure_ao(div);
    if (!ao_ensure_program()) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_ao_prog);

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(s_ao_apos);
    glVertexAttribPointer(s_ao_apos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)0);
    glEnableVertexAttribArray(s_ao_auv);
    glVertexAttribPointer(s_ao_auv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    glUniform1i(s_ao_u.depth, 0);
    glUniform1i(s_ao_u.zd, 1);
    glUniform1i(s_ao_u.ao, 2);
    glUniform1i(s_ao_u.color, 3);
    glUniform2f(s_ao_u.res, (float)s_eye_w, (float)s_eye_h);
    glUniform2f(s_ao_u.low, (float)s_ao_w, (float)s_ao_h);
    glUniform2f(s_ao_u.texel, 1.0f / (float)(s_eye_w ? s_eye_w : 1),
                1.0f / (float)(s_eye_h ? s_eye_h : 1));
    glUniform4f(s_ao_u.canvas, (float)s_canvas_x, (float)s_canvas_y,
                (float)s_canvas_w, (float)s_canvas_h);
    glUniform1f(s_ao_u.nearv, gfx_cam_near);
    glUniform1f(s_ao_u.farv, gfx_cam_far);
    glUniform1f(s_ao_u.tanhalf, gfx_cam_tan_half_fov_y);
    glUniform1f(s_ao_u.aspect, (float)s_canvas_w / (float)(s_canvas_h ? s_canvas_h : 1));
    glUniform1f(s_ao_u.normalflip, (float)s_fx_normal_flip);
    glUniform1f(s_ao_u.radius, (float)s_fx_ao_radius);
    glUniform1f(s_ao_u.samples, (float)s_fx_ao_samples);
    glUniform1f(s_ao_u.bias, (float)s_fx_ao_bias);
    glUniform1f(s_ao_u.power, (float)s_fx_ao_power);
    glUniform1f(s_ao_u.intensity, (float)s_fx_ao_intensity);
    glUniform1f(s_ao_u.frame, (float)(frame_count & 1023));
    glUniform1f(s_ao_u.jitter, s_fx_ao_jitter ? 1.0f : 0.0f);
    glUniform1f(s_ao_u.sky, s_fx_ao_sky ? 1.0f : 0.0f);
    glUniform1f(s_ao_u.trig, s_fx_ao_trig ? 1.0f : 0.0f);
    /* Same edge stop the denoiser upsample uses: taps are only `div` screen
     * pixels from the centre, so the tolerance is divided by div. */
    glUniform1f(s_ao_u.upsigma, 0.25f / (float)div);

    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;

        glViewport(0, 0, s_ao_w, s_ao_h);
        glUniform1f(s_ao_u.shear, gfx_eye_shear[eye]);
        glUniform1f(s_ao_u.toe, gfx_eye_toe[eye]);
        glUniform1f(s_ao_u.flipy, gfx_eye_flip[eye] ? 1.0f : 0.0f);

        /* 1. depth + normal -> low-res packed field */
        glBindFramebuffer(GL_FRAMEBUFFER, s_ao_zdfbo[eye]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_eye_depth[eye]);
        glUniform1f(s_ao_u.mode, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (s_fx_ao_probe == 1) continue;

        /* 2. the gather, reading only the low-res field */
        glBindFramebuffer(GL_FRAMEBUFFER, s_ao_fbo[eye]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_ao_zd[eye]);
        /* Never leave this eye's own gather target bound to a sampler. */
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUniform1f(s_ao_u.mode, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (s_fx_ao_probe == 2) continue;

        /* 3. upsample + multiply into the full-res colour */
        int apply = s_fx_ao_up ? 3 : 2;
        glBindFramebuffer(GL_FRAMEBUFFER, s_ao_ofbo[eye]);
        glViewport(0, 0, s_eye_w, s_eye_h);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_ao_tex[eye]);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D,
                      s_dof_valid[eye] ? s_dof_result[eye]
                      : (s_dn_valid[eye] ? s_dn_result[eye]
                                         : s_eye_tex[eye]));
        glUniform1f(s_ao_u.mode, (float)apply);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        s_ao_valid[eye] = 1;
    }

    glDisableVertexAttribArray(s_ao_apos);
    glDisableVertexAttribArray(s_ao_auv);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* ---------------- Emissive materials + bloom ----------------
 * Tagged draw batches (see GFX_OP_EMISSIVE_MARKER in gfx_pc.h) are copied at
 * draw time and re-drawn into a half-resolution per-eye EMISSIVE buffer. That
 * buffer is the only input to the bloom chain, so a bright white wall does not
 * bloom but a jetpack flame does: the glow source is world-space geometry, not
 * "whatever pixel happens to be bright" in the finished image.
 *
 * Chain (per eye, all off unless s_glow_on):
 *   emissive (1/2 res) -> threshold/downsample (1/2) -> blur H -> blur V
 *   -> additive upsample into the FX pass output (own pass: doing the upsample
 *   inside the big FX shader cost ~30 ms, see BACKLOG).
 */
/* Captured emissive batches for the eye currently being rendered. */
typedef struct {
    size_t first;      // vertex offset in s_em_verts
    size_t nverts;
    int rgb[3];
    int strength;
} EmissiveBatch;

#define EM_MAX_BATCHES 96
#define EM_MAX_VERTS 65536

static float *s_em_verts = NULL;
static size_t s_em_verts_n = 0, s_em_verts_cap = 0;
static EmissiveBatch s_em_batches[EM_MAX_BATCHES];
static int s_em_batch_n = 0;
static int s_em_capture_eye = -1;  // >=0 while an eye's scene range is running

static GLuint s_em_prog = 0;
static int    s_em_prog_state = 0;
static GLint  s_em_apos = -1, s_em_col = -1;
static GLuint s_em_tex[2] = {0, 0}, s_em_fbo[2] = {0, 0};
static int    s_em_w = 0, s_em_h = 0;

static GLuint s_bl_prog = 0;
static int    s_bl_prog_state = 0;
static GLint  s_bl_apos = -1, s_bl_auv = -1;
static struct {
    GLint mode, src, scene, texel, dir, threshold, gain, intensity, scene_on, taps;
} s_bl_u;
static GLuint s_bl_a[2] = {0, 0}, s_bl_b[2] = {0, 0};
static GLuint s_bl_afbo[2] = {0, 0}, s_bl_bfbo[2] = {0, 0};
static int    s_bl_w = 0, s_bl_h = 0;

static const char *s_em_vs =
    "attribute vec4 aPos;\n"
    "void main() { gl_Position = aPos; }\n";

static const char *s_em_fs =
    "precision mediump float;\n"
    "uniform vec4 uCol;\n"
    "void main() { gl_FragColor = vec4(uCol.rgb * uCol.a, 1.0); }\n";

static const char *s_bl_vs =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *s_bl_fs =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform float uMode;\n"        // 0 threshold, 1 blur, 2 additive upsample
    "uniform sampler2D uSrc;\n"
    "uniform sampler2D uScene;\n"
    "uniform vec2 uTexel;\n"
    "uniform vec2 uDir;\n"
    "uniform float uThreshold;\n"
    "uniform float uGain;\n"
    "uniform float uIntensity;\n"
    "uniform float uSceneOn;\n"
    "uniform float uTaps;\n"
    "float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "void main() {\n"
    "  if (uMode < 0.5) {\n"
    "    vec3 c = texture2D(uSrc, vUV).rgb * uGain;\n"
    "    float l = luma(c);\n"
    "    vec3 bright = l > uThreshold ? c * (1.0 - uThreshold / max(l, 0.0001)) : vec3(0.0);\n"
    "    if (uSceneOn > 0.5) {\n"
    "      vec3 sc = texture2D(uScene, vUV).rgb;\n"
    "      float sl = luma(sc);\n"
    "      if (sl > uThreshold * 1.6) bright += sc * 0.25;\n"
    "    }\n"
    "    gl_FragColor = vec4(bright, 1.0);\n"
    "  } else if (uMode < 1.5) {\n"
    "    // 5- or 9-tap separable gaussian; spacing scales with uDir*uTexel*radius.\n"
    "    vec2 st = uDir * uTexel;\n"
    "    vec3 acc = texture2D(uSrc, vUV).rgb * 0.2270270270;\n"
    "    acc += (texture2D(uSrc, vUV + st * 1.0).rgb + texture2D(uSrc, vUV - st * 1.0).rgb) * 0.1945945946;\n"
    "    acc += (texture2D(uSrc, vUV + st * 2.0).rgb + texture2D(uSrc, vUV - st * 2.0).rgb) * 0.1216216216;\n"
    "    acc += (texture2D(uSrc, vUV + st * 3.0).rgb + texture2D(uSrc, vUV - st * 3.0).rgb) * 0.0540540541;\n"
    "    if (uTaps > 7.5) acc += (texture2D(uSrc, vUV + st * 4.0).rgb + texture2D(uSrc, vUV - st * 4.0).rgb) * 0.0162162162;\n"
    "    gl_FragColor = vec4(acc, 1.0);\n"
    "  } else {\n"
    "    gl_FragColor = vec4(texture2D(uSrc, vUV).rgb * uIntensity, 1.0);\n"
    "  }\n"
    "}\n";

/* Emissive capture target: half of the eye buffer, RGBA8. */
static void em_ensure_targets(void) {
    int w = s_eye_w / 2;
    int h = s_eye_h / 2;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == s_em_w && h == s_em_h && s_em_fbo[0] != 0) return;
    for (int eye = 0; eye < 2; eye++) {
        if (s_em_fbo[eye]) glDeleteFramebuffers(1, &s_em_fbo[eye]);
        if (s_em_tex[eye]) glDeleteTextures(1, &s_em_tex[eye]);
        glGenTextures(1, &s_em_tex[eye]);
        glBindTexture(GL_TEXTURE_2D, s_em_tex[eye]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s_em_fbo[eye]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_em_fbo[eye]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_em_tex[eye], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_em_w = w;
    s_em_h = h;
}

/* Bloom ping-pong targets, same resolution as the emissive buffer. */
static void bloom_ensure_targets(void) {
    int w = s_em_w > 0 ? s_em_w : (s_eye_w / 2 > 0 ? s_eye_w / 2 : 1);
    int h = s_em_h > 0 ? s_em_h : (s_eye_h / 2 > 0 ? s_eye_h / 2 : 1);
    if (w == s_bl_w && h == s_bl_h && s_bl_afbo[0] != 0) return;
    for (int eye = 0; eye < 2; eye++) {
        GLuint *texs[2] = { &s_bl_a[eye], &s_bl_b[eye] };
        GLuint *fbos[2] = { &s_bl_afbo[eye], &s_bl_bfbo[eye] };
        for (int t = 0; t < 2; t++) {
            if (*fbos[t]) glDeleteFramebuffers(1, fbos[t]);
            if (*texs[t]) glDeleteTextures(1, texs[t]);
            glGenTextures(1, texs[t]);
            glBindTexture(GL_TEXTURE_2D, *texs[t]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, fbos[t]);
            glBindFramebuffer(GL_FRAMEBUFFER, *fbos[t]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, *texs[t], 0);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_bl_w = w;
    s_bl_h = h;
}

static int em_ensure_program(void) {
    if (s_em_prog_state != 0) return s_em_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_em_vs, "emissive vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_em_fs, "emissive fragment");
    if (!vs || !fs) {
        s_em_prog_state = -1;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    s_em_prog = glCreateProgram();
    glAttachShader(s_em_prog, vs);
    glAttachShader(s_em_prog, fs);
    glLinkProgram(s_em_prog);
    GLint ok = 0;
    glGetProgramiv(s_em_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        s_em_prog = 0;
        s_em_prog_state = -1;
        return 0;
    }
    s_em_apos = glGetAttribLocation(s_em_prog, "aPos");
    s_em_col = glGetUniformLocation(s_em_prog, "uCol");
    s_em_prog_state = 1;
    return 1;
}

static int bloom_ensure_program(void) {
    if (s_bl_prog_state != 0) return s_bl_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_bl_vs, "bloom vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_bl_fs, "bloom fragment");
    if (!vs || !fs) {
        s_bl_prog_state = -1;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    s_bl_prog = glCreateProgram();
    glAttachShader(s_bl_prog, vs);
    glAttachShader(s_bl_prog, fs);
    glLinkProgram(s_bl_prog);
    GLint ok = 0;
    glGetProgramiv(s_bl_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        s_bl_prog = 0;
        s_bl_prog_state = -1;
        return 0;
    }
    s_bl_apos = glGetAttribLocation(s_bl_prog, "aPos");
    s_bl_auv = glGetAttribLocation(s_bl_prog, "aUV");
    s_bl_u.mode = glGetUniformLocation(s_bl_prog, "uMode");
    s_bl_u.src = glGetUniformLocation(s_bl_prog, "uSrc");
    s_bl_u.scene = glGetUniformLocation(s_bl_prog, "uScene");
    s_bl_u.texel = glGetUniformLocation(s_bl_prog, "uTexel");
    s_bl_u.dir = glGetUniformLocation(s_bl_prog, "uDir");
    s_bl_u.threshold = glGetUniformLocation(s_bl_prog, "uThreshold");
    s_bl_u.gain = glGetUniformLocation(s_bl_prog, "uGain");
    s_bl_u.intensity = glGetUniformLocation(s_bl_prog, "uIntensity");
    s_bl_u.scene_on = glGetUniformLocation(s_bl_prog, "uSceneOn");
    s_bl_u.taps = glGetUniformLocation(s_bl_prog, "uTaps");
    s_bl_prog_state = 1;
    return 1;
}

/* Append the batch draw_triangles() was just handed. Only the 4 clip-space
 * position floats per vertex are kept; the emission colour comes from the
 * tag, so this does not depend on the combiner that was active. */
static void emissive_capture(const float *vbo, size_t nfloats, size_t stride,
                             size_t ntris, const int rgb[3], int strength) {
    if (s_em_batch_n >= EM_MAX_BATCHES) return;
    size_t nverts = ntris * 3;
    if (nverts == 0) return;
    if (s_em_verts_n + nverts > EM_MAX_VERTS) return;
    if (s_em_verts_n + nverts > s_em_verts_cap) {
        size_t cap = s_em_verts_cap ? s_em_verts_cap : 4096;
        while (cap < s_em_verts_n + nverts) cap *= 2;
        float *p = (float *)realloc(s_em_verts, cap * 4 * sizeof(float));
        if (p == NULL) return;
        s_em_verts = p;
        s_em_verts_cap = cap;
    }
    EmissiveBatch *b = &s_em_batches[s_em_batch_n++];
    b->first = s_em_verts_n;
    b->nverts = nverts;
    for (int i = 0; i < 3; i++) b->rgb[i] = rgb[i];
    b->strength = strength;
    for (size_t v = 0; v < nverts; v++) {
        const float *src = vbo + v * stride;
        float *dst = s_em_verts + (s_em_verts_n + v) * 4;
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
    }
    s_em_verts_n += nverts;
}

/* End of an eye's scene range: re-draw the tagged batches into that eye's
 * half-res emissive buffer. Runs even when bloom is off (the buffer is then
 * only used by the debug view) but is skipped entirely when tags are off. */
void gfx_opengl_emissive_flush(int eye) {
    s_em_capture_eye = -1;
    if (s_em_batch_n == 0) {
        if (s_em_tex[eye & 1] == 0) return;
        // Still clear so the debug view / bloom input cannot show last frame.
        if (!gfx_emissive_on) return;
    }
    if (!gfx_emissive_on) {
        s_em_batch_n = 0;
        s_em_verts_n = 0;
        return;
    }
    em_ensure_targets();
    if (!em_ensure_program() || s_em_tex[eye & 1] == 0) {
        s_em_batch_n = 0;
        s_em_verts_n = 0;
        return;
    }
    if (s_em_batch_n == 0 && !s_glow_on && !s_glow_debug) {
        return;
    }

    /* Mid-frame pass: snapshot the GL state the pipeline's caches believe in,
     * because the rest of this eye (UI when UIPOST is off) keeps drawing. */
    GLint vp[4] = { 0, 0, 1, 1 };
    glGetIntegerv(GL_VIEWPORT, vp);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    GLboolean scissor_was = glIsEnabled(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, s_em_fbo[eye & 1]);
    glViewport(0, 0, s_em_w, s_em_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(s_em_prog);
    glEnableVertexAttribArray(s_em_apos);

    float gain = (float)s_glow_gain / 100.0f;
    for (int i = 0; i < s_em_batch_n; i++) {
        EmissiveBatch *b = &s_em_batches[i];
        glUniform4f(s_em_col, (float)b->rgb[0] / 255.0f, (float)b->rgb[1] / 255.0f,
                    (float)b->rgb[2] / 255.0f,
                    gain * (float)b->strength / 255.0f);
        if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(b->nverts * 4 * sizeof(float)),
                     s_em_verts + b->first * 4, GL_STREAM_DRAW);
        glVertexAttribPointer(s_em_apos, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (void *)0);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->nverts);
    }

    glDisableVertexAttribArray(s_em_apos);
    /* Restore the eye target and the state the pipeline caches expect. */
    glBindFramebuffer(GL_FRAMEBUFFER, s_eye_fbo[eye & 1]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (depth_was) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blend_was) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (scissor_was) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    /* draw_triangles() buffers into whatever VBO is bound; hand opengl_vbo
     * back so the next frame's scene does not land in our scratch buffer. */
    if (opengl_vbo) glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    s_em_batch_n = 0;
    s_em_verts_n = 0;
}

/* Threshold -> blur -> additive upsample. Each stage is its own pass; the
 * additive upsample writes into the FX pass output (s_pp_tex) for this eye. */
static void stereo_bloom_pass(void) {
    if (!s_glow_on || s_glow_debug != 0) return;
    if (!bloom_ensure_program()) return;
    em_ensure_targets();
    bloom_ensure_targets();
    if (s_bl_afbo[0] == 0) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_bl_prog);
    glUniform1i(s_bl_u.src, 0);
    glUniform1i(s_bl_u.scene, 1);
    glUniform2f(s_bl_u.texel, 1.0f / (float)s_bl_w, 1.0f / (float)s_bl_h);
    glUniform1f(s_bl_u.threshold, (float)s_glow_threshold / 255.0f * 1.6f);
    glUniform1f(s_bl_u.gain, (float)s_glow_gain / 100.0f);
    glUniform1f(s_bl_u.intensity, (float)s_glow_intensity / 100.0f);
    glUniform1f(s_bl_u.scene_on, s_glow_scene ? 1.0f : 0.0f);
    glUniform1f(s_bl_u.taps, (float)(s_glow_taps >= 9 ? 9 : 5));

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(s_bl_apos);
    glVertexAttribPointer(s_bl_apos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)0);
    if (s_bl_auv >= 0) {
        glEnableVertexAttribArray(s_bl_auv);
        glVertexAttribPointer(s_bl_auv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (void *)(2 * sizeof(float)));
    }

    float spacing = (float)(s_glow_radius < 1 ? 1 : s_glow_radius);
    int passes = s_glow_passes < 1 ? 1 : (s_glow_passes > 3 ? 3 : s_glow_passes);

    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;
        if (s_em_tex[eye] == 0 || s_pp_tex[eye] == 0) continue;

        glViewport(0, 0, s_bl_w, s_bl_h);

        // 1. threshold + downsample of the emissive buffer
        glBindFramebuffer(GL_FRAMEBUFFER, s_bl_afbo[eye]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_em_tex[eye]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_eye_tex[eye]);
        glUniform1f(s_bl_u.mode, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // 2. separable gaussian, N passes with growing spacing
        int src = 0;
        float step = spacing;
        for (int p = 0; p < passes; p++) {
            // horizontal: src -> other
            glBindFramebuffer(GL_FRAMEBUFFER, src == 0 ? s_bl_bfbo[eye] : s_bl_afbo[eye]);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src == 0 ? s_bl_a[eye] : s_bl_b[eye]);
            glUniform1f(s_bl_u.mode, 1.0f);
            glUniform2f(s_bl_u.dir, step, 0.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            src = 1 - src;

            // vertical
            glBindFramebuffer(GL_FRAMEBUFFER, src == 0 ? s_bl_bfbo[eye] : s_bl_afbo[eye]);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src == 0 ? s_bl_a[eye] : s_bl_b[eye]);
            glUniform2f(s_bl_u.dir, 0.0f, step);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            src = 1 - src;
            step *= 1.8f;
        }

        // 3. additive bilinear upsample into the FX result (own pass)
        glBindFramebuffer(GL_FRAMEBUFFER, s_pp_fbo[eye]);
        glViewport(0, 0, s_eye_w, s_eye_h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_bl_a[eye]);   // passes end on stage 0
        glUniform1f(s_bl_u.mode, 2.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
    }

    glDisableVertexAttribArray(s_bl_apos);
    if (s_bl_auv >= 0) glDisableVertexAttribArray(s_bl_auv);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    if (opengl_vbo) glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
}

/* Runs the post pass for both eyes. Returns 1 if s_pp_tex is valid to composite. */


/* ---------------- Crepuscular rays: quarter-res prepass ---------------- */

static int ray_ensure_program(void) {
    if (s_ray_prog_state != 0) return s_ray_prog_state == 1;
    GLuint vs = compile_shader_checked(GL_VERTEX_SHADER, s_dn_vs, "rays vertex");
    GLuint fs = compile_shader_checked(GL_FRAGMENT_SHADER, s_ray_fs, "rays fragment");
    if (!vs || !fs) {
        s_ray_prog_state = -1;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    s_ray_prog = glCreateProgram();
    glAttachShader(s_ray_prog, vs);
    glAttachShader(s_ray_prog, fs);
    glLinkProgram(s_ray_prog);
    GLint ok = 0;
    glGetProgramiv(s_ray_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(s_ray_prog, sizeof(log), NULL, log);
        __android_log_print(ANDROID_LOG_WARN, "SM64FX", "rays link failed: %s", log);
        glDeleteProgram(s_ray_prog);
        s_ray_prog = 0;
        s_ray_prog_state = -1;
        return 0;
    }
    s_ray_apos = glGetAttribLocation(s_ray_prog, "aPos");
    s_ray_auv = glGetAttribLocation(s_ray_prog, "aUV");
    s_ray_u.mode = glGetUniformLocation(s_ray_prog, "uMode");
    s_ray_u.src_depth = glGetUniformLocation(s_ray_prog, "uSrcDepth");
    s_ray_u.src_col = glGetUniformLocation(s_ray_prog, "uSrcCol");
    s_ray_u.src_ray = glGetUniformLocation(s_ray_prog, "uSrcRay");
    s_ray_u.texel = glGetUniformLocation(s_ray_prog, "uTexel");
    s_ray_u.dir = glGetUniformLocation(s_ray_prog, "uDir");
    s_ray_u.nearv = glGetUniformLocation(s_ray_prog, "uNear");
    s_ray_u.farv = glGetUniformLocation(s_ray_prog, "uFar");
    s_ray_u.light_uv = glGetUniformLocation(s_ray_prog, "uLightUV");
    s_ray_u.samples = glGetUniformLocation(s_ray_prog, "uSamples");
    s_ray_u.density = glGetUniformLocation(s_ray_prog, "uDensity");
    s_ray_u.decay = glGetUniformLocation(s_ray_prog, "uDecay");
    s_ray_u.weight = glGetUniformLocation(s_ray_prog, "uWeight");
    s_ray_u.threshold = glGetUniformLocation(s_ray_prog, "uThreshold");
    s_ray_u.bias = glGetUniformLocation(s_ray_prog, "uBias");
    s_ray_u.jitter = glGetUniformLocation(s_ray_prog, "uJitter");
    s_ray_u.frame = glGetUniformLocation(s_ray_prog, "uFrame");
    s_ray_u.res = glGetUniformLocation(s_ray_prog, "uRes");
    s_ray_prog_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX", "rays program ready (%u)", s_ray_prog);
    return 1;
}

/* Raw march + two ping-pong blur targets per eye, all at quarter res. */
static int ray_ensure_fbo(int w, int h) {
    if (w == s_ray_w && h == s_ray_h && s_ray_fbo[0][0] != 0) return 1;
    for (int eye = 0; eye < 2; eye++) {
        for (int st = 0; st < 3; st++) {
            if (s_ray_fbo[eye][st]) glDeleteFramebuffers(1, &s_ray_fbo[eye][st]);
            if (s_ray_tex[eye][st]) glDeleteTextures(1, &s_ray_tex[eye][st]);
            glGenTextures(1, &s_ray_tex[eye][st]);
            glBindTexture(GL_TEXTURE_2D, s_ray_tex[eye][st]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
            GLint filt = (st == 0) ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &s_ray_fbo[eye][st]);
            glBindFramebuffer(GL_FRAMEBUFFER, s_ray_fbo[eye][st]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, s_ray_tex[eye][st], 0);
            GLenum fst = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (fst != GL_FRAMEBUFFER_COMPLETE) {
                __android_log_print(ANDROID_LOG_WARN, "SM64FX",
                        "RAY fbo eye=%d st=%d incomplete 0x%x", eye, st, fst);
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_ray_w = w;
    s_ray_h = h;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX",
            "RAY fbo eye0=%u,%u,%u %dx%d", s_ray_tex[0][0], s_ray_tex[0][1],
            s_ray_tex[0][2], w, h);
    return 1;
}

/* March + blur both eyes; leaves the per-eye shaft buffer in s_ray_result[]. */
static void stereo_rays_prepass(void) {
    s_ray_valid[0] = s_ray_valid[1] = 0;
    int w = s_eye_w / 4;
    int h = s_eye_h / 4;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (!ray_ensure_program()) return;
    if (!ray_ensure_fbo(w, h)) return;

    const float (*M)[4] = gfx_cam_inv_mtx;
    float cam[3] = { M[3][0], M[3][1], M[3][2] };
    int li = s_ray_light;
    /* 0-3 use an active dynamic light; 4, an inactive index, or a light behind
     * the camera falls back to a synthetic sun direction. */
    int sun = (li < 0 || li > 3 || li >= s_light_count);
    float lv[3] = { -0.55f, 0.80f, 0.45f };   /* view space, +z is behind */
    if (!sun) {
        float p[3] = { s_light_pos[li][0], s_light_pos[li][1], s_light_pos[li][2] };
        for (int j = 0; j < 3; j++) {
            lv[j] = (p[0] - cam[0]) * M[j][0]
                  + (p[1] - cam[1]) * M[j][1]
                  + (p[2] - cam[2]) * M[j][2];
        }
        if (lv[2] > -1.0f) sun = 1;
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_ray_prog);

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (!s_comp_vbo) glGenBuffers(1, &s_comp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(s_ray_apos);
    glVertexAttribPointer(s_ray_apos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(s_ray_auv);
    glVertexAttribPointer(s_ray_auv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    glUniform1i(s_ray_u.src_depth, 0);
    glUniform1i(s_ray_u.src_col, 1);
    glUniform1i(s_ray_u.src_ray, 2);
    glUniform2f(s_ray_u.texel, 1.0f / (float)w, 1.0f / (float)h);
    glUniform1f(s_ray_u.nearv, gfx_cam_near);
    glUniform1f(s_ray_u.farv, gfx_cam_far);
    glUniform1f(s_ray_u.samples, (float)s_ray_samples);
    glUniform1f(s_ray_u.density, (float)s_ray_density / 100.0f);
    glUniform1f(s_ray_u.decay, (float)s_ray_decay / 100.0f);
    glUniform1f(s_ray_u.weight, (float)s_ray_weight / 100.0f);
    glUniform1f(s_ray_u.threshold, (float)s_ray_threshold / 100.0f);
    glUniform1f(s_ray_u.bias, (float)s_ray_bias / 100.0f);
    glUniform1f(s_ray_u.jitter, (float)s_ray_jitter / 100.0f);
    glUniform1f(s_ray_u.frame, (float)(frame_count & 1023));
    glUniform2f(s_ray_u.res, (float)s_eye_w, (float)s_eye_h);

    /* Sun anchor in eye UV: up-and-behind-left, projected as a direction and
     * pushed outside the frame so the shafts rake across the screen.  A real
     * light is projected through the same per-eye shear/toe/flip the FX shader
     * uses; both cases are clamped to the frame edge afterwards. */
    float ax = lv[0], ay = lv[1];
    float al = sqrtf(ax * ax + ay * ay);
    if (al < 1.0e-4f) { ax = -0.4f; ay = 0.8f; al = 0.8944f; }
    ax /= al;
    ay /= al;
    float ccx = ((float)s_canvas_x + (float)s_canvas_w * 0.5f) / (float)s_eye_w;
    float ccy = ((float)s_canvas_y + (float)s_canvas_h * 0.5f) / (float)s_eye_h;
    float aspect = (float)s_canvas_w / (float)(s_canvas_h ? s_canvas_h : 1);
    float anchors[2][2];

    for (int eye = 0; eye < 2; eye++) {
        float flip = gfx_eye_flip[eye] ? -1.0f : 1.0f;
        float uvx, uvy;
        if (!sun) {
            float z = -lv[2] > 1.0f ? -lv[2] : 1.0f;
            float ndcx = lv[0] / (z * gfx_cam_tan_half_fov_y * aspect);
            float ndcy = lv[1] / (z * gfx_cam_tan_half_fov_y);
            float ndcz = (gfx_cam_far + gfx_cam_near
                          - 2.0f * gfx_cam_near * gfx_cam_far / z)
                         / (gfx_cam_far - gfx_cam_near);
            ndcx += gfx_eye_shear[eye] + gfx_eye_toe[eye] * ndcz;
            if (gfx_eye_flip[eye]) ndcy = -ndcy;
            float cuvx = ndcx * 0.5f + 0.5f;
            float cuvy = ndcy * 0.5f + 0.5f;
            uvx = (cuvx * (float)s_canvas_w + (float)s_canvas_x) / (float)s_eye_w;
            uvy = (cuvy * (float)s_canvas_h + (float)s_canvas_y) / (float)s_eye_h;
        } else {
            uvx = ccx + ax * ((float)s_canvas_w / (float)s_eye_w) * 0.95f;
            uvy = ccy + ay * flip * ((float)s_canvas_h / (float)s_eye_h) * 0.95f;
        }
        anchors[eye][0] = uvx;
        anchors[eye][1] = uvy;
        s_ray_lightuv[eye][0] = uvx < 0.0f ? 0.0f : (uvx > 1.0f ? 1.0f : uvx);
        s_ray_lightuv[eye][1] = uvy < 0.0f ? 0.0f : (uvy > 1.0f ? 1.0f : uvy);
    }

    for (int eye = 0; eye < 2; eye++) {
        if (gfx_single_eye >= 0 && eye != gfx_single_eye) continue;
        glUniform2f(s_ray_u.light_uv, anchors[eye][0], anchors[eye][1]);

        glViewport(0, 0, w, h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_eye_depth[eye]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_eye_tex[eye]);

        /* 1. march */
        glBindFramebuffer(GL_FRAMEBUFFER, s_ray_fbo[eye][0]);
        glUniform1f(s_ray_u.mode, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (s_ray_debug != 2) {
            /* 2. horizontal blur */
            glBindFramebuffer(GL_FRAMEBUFFER, s_ray_fbo[eye][1]);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, s_ray_tex[eye][0]);
            glUniform2f(s_ray_u.dir, 1.0f, 0.0f);
            glUniform1f(s_ray_u.mode, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            /* 3. vertical blur */
            glBindFramebuffer(GL_FRAMEBUFFER, s_ray_fbo[eye][2]);
            glBindTexture(GL_TEXTURE_2D, s_ray_tex[eye][1]);
            glUniform2f(s_ray_u.dir, 0.0f, 1.0f);
            glUniform1f(s_ray_u.mode, 2.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            s_ray_result[eye] = s_ray_tex[eye][2];
        } else {
            /* Visibility debug: skip the blur so the raw march is visible. */
            s_ray_result[eye] = s_ray_tex[eye][0];
        }
        s_ray_valid[eye] = 1;
    }

    glDisableVertexAttribArray(s_ray_apos);
    glDisableVertexAttribArray(s_ray_auv);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* Runs the post pass for both eyes. Returns 1 if s_pp_tex is valid to composite. */

static int stereo_fx_pass(void) {
    /* Recomputed every frame: the composite reads s_ao_otex only when this
     * frame's chain wrote it. */
    s_ao_valid[0] = s_ao_valid[1] = 0;
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
    /* AO path choice (fx 390).  The reduced-resolution chain cannot serve the
     * AO debug view (it leaves no `ao` value in the FX shader) and SSRTX still
     * wants the AO factor for its blend weight, so those two keep the full-res
     * in-shader gather. */
    int ao_on_eff = s_fx_ao_on && s_fx_ao_intensity > 0;
    int ao_legacy = ao_on_eff && (s_fx_ao_path == 0 || s_fx_debug != 0
                                  || s_fx_ssrtx_on);
    int ao_chain = ao_on_eff && !ao_legacy;
    int ao_only = ao_chain && !stereo_fx_other();

    if (!ao_only) {
        stereo_fx_init();
        if (s_fx_prog_state != 1) return 0;
        stereo_ensure_pp(s_eye_w, s_eye_h);
    }

    /* Alternative path + denoiser at divisor > 1: the reduced-res chain runs
     * offscreen and its mixed result becomes the FX pass's scene colour.
     * Divisor 1 keeps the original in-shader full-res denoiseTap() path. */
    int dn_div = (s_denoise_path && s_denoise_on && s_denoise_div > 1) ? s_denoise_div : 0;
    if (dn_div > 0) {
        stereo_denoise_prepass(dn_div);
        /* If the prepass could not build its program/targets, fall back to the
         * in-shader full-res path rather than sampling empty textures. */
        if (!s_dn_valid[0] && !s_dn_valid[1]) dn_div = 0;
    } else {
        s_dn_valid[0] = s_dn_valid[1] = 0;
    }

    /* C.DOF reduced-res prepass.  The gather output is finished colour, so the
     * FX shader's own DOF block stays off; on failure it falls back to the
     * legacy in-shader full-res gather. */
    int dof_div = 0;
    if (s_fx_dof_on && s_fx_dof_half) {
        int d = s_fx_dof_div < 2 ? 2 : (s_fx_dof_div > 4 ? 4 : s_fx_dof_div);
        if (stereo_dof_prepass(d)) {
            dof_div = d;
        } else {
            s_dof_valid[0] = s_dof_valid[1] = 0;
        }
    } else {
        s_dof_valid[0] = s_dof_valid[1] = 0;
    }

    /* Reduced-resolution AO, after the denoise and C.DOF prepasses so it
     * darkens their finished colour.  s_ao_otex[eye] then replaces the FX
     * pass's uColor, and when AO is the only enabled effect the pass itself is
     * skipped (the composite samples s_ao_otex directly). */
    if (ao_chain) {
        stereo_ao_prepass(s_fx_ao_div);
        if (ao_only && (s_ao_valid[0] || s_ao_valid[1])) {
            return 0;
        }
        if (ao_only) {
            /* chain failed to build: fall back to the in-shader gather */
            ao_chain = 0;
            stereo_fx_init();
            if (s_fx_prog_state != 1) return 0;
            stereo_ensure_pp(s_eye_w, s_eye_h);
        }
    }

    /* Crepuscular rays: quarter-res march + blur per eye.  Skipped entirely
     * (no FBO allocation, no draw) when the effect is off. */
    if (s_ray_on) {
        stereo_rays_prepass();
    } else {
        s_ray_valid[0] = s_ray_valid[1] = 0;
    }

    /* Reduced-resolution SSGI: gather at canvas/div, blur there, then add back
     * with a dedicated full-res pass after the FX draws below.  Divisor 1 (or
     * the GI debug view) keeps the original in-shader full-res loop. */
    int gi_low = s_fx_gi_on && s_fx_gi_intensity > 0 && s_fx_gi_div > 1
                 && s_fx_debug != 3;
    if (gi_low) {
        stereo_gi_prepass(s_fx_gi_div);
        if (!s_gi_valid[0] && !s_gi_valid[1]) gi_low = 0;
    } else {
        s_gi_valid[0] = s_gi_valid[1] = 0;
    }

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
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, (s_vel_tex_valid && s_vel_source) ? s_vel_tex : 0);
    if (s_fx_u.velocity >= 0) glUniform1i(s_fx_u.velocity, 3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, (s_vmax_valid && s_vel_source) ? s_vmax_tex : 0);
    if (s_fx_u.vel_max >= 0) glUniform1i(s_fx_u.vel_max, 4);
    glActiveTexture(GL_TEXTURE0);
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
    {
        float vp[4][4];
        float pvpos[4][3];
        float camPos[3] = { gfx_cam_inv_mtx[3][0], gfx_cam_inv_mtx[3][1],
                            gfx_cam_inv_mtx[3][2] };
        fx_build_vp(vp);
        if (s_fx_vp_frame != frame_count) {
            if (s_fx_have_prev) {
                memcpy(s_fx_prev_vp, s_fx_cur_vp, sizeof(vp));
            } else {
                memcpy(s_fx_prev_vp, vp, sizeof(vp));
                s_fx_have_prev = 1;
            }
            memcpy(s_fx_cur_vp, vp, sizeof(vp));
            /* Same one-per-frame guard for the camera matrix used by motion
             * blur; updating it per pass would erase the velocity. */
            if (!s_have_prev_cam_inv) {
                memcpy(s_fx_cur_cam_inv, gfx_cam_inv_mtx, sizeof(s_fx_cur_cam_inv));
                memcpy(s_prev_cam_inv, s_fx_cur_cam_inv, sizeof(s_prev_cam_inv));
                s_have_prev_cam_inv = 1;
            } else {
                memcpy(s_prev_cam_inv, s_fx_cur_cam_inv, sizeof(s_prev_cam_inv));
                memcpy(s_fx_cur_cam_inv, gfx_cam_inv_mtx, sizeof(s_fx_cur_cam_inv));
            }
            s_fx_vp_frame = frame_count;
        }
        upload_mat4_rowvec(s_fx_u.cur_vp, s_fx_cur_vp);
        upload_mat4_rowvec(s_fx_u.prev_vp, s_fx_prev_vp);
        upload_mat4_rowvec(s_fx_u.prev_cam_inv, s_prev_cam_inv);
        (void) camPos;

        /* world-space lights -> view space for the screen-space shadow march */
        for (int i = 0; i < 4; i++) {
            float p[3] = { s_light_pos[i][0], s_light_pos[i][1], s_light_pos[i][2] };
            for (int j = 0; j < 3; j++) {
                pvpos[i][j] = (p[0] - camPos[0]) * gfx_cam_inv_mtx[j][0]
                            + (p[1] - camPos[1]) * gfx_cam_inv_mtx[j][1]
                            + (p[2] - camPos[2]) * gfx_cam_inv_mtx[j][2];
            }
        }
        glUniform1f(s_fx_u.mb_on, s_fx_mb_on ? 1.0f : 0.0f);
        glUniform1f(s_fx_u.mb_strength, (float)s_fx_mb_strength);
        glUniform1f(s_fx_u.mb_samples, (float)s_fx_mb_samples);
        glUniform1f(s_fx_u.vel_on,
                    (s_vel_tex_valid && s_vel_source && gfx_vel_enabled) ? 1.0f : 0.0f);
        {
            /* Per-vertex vectors already describe the presented sub-frame
             * step; only the camera-only fallback spans a whole sim step. */
            extern float gfx_cam_vel_scale;
            glUniform1f(s_fx_u.vel_scale,
                        (s_vel_tex_valid && s_vel_source && gfx_vel_enabled)
                        ? 1.0f : gfx_cam_vel_scale);
        }
        glUniform1f(s_fx_u.mb_shutter, (float)s_mb_shutter);
        glUniform1f(s_fx_u.mb_substeps, (float)s_mb_substeps);
        glUniform1f(s_fx_u.mb_maxpx, (float)s_mb_maxpx);
        glUniform1f(s_fx_u.mb_jitter, (float)s_mb_jitter);
        glUniform1f(s_fx_u.mb_depth_reject, (float)s_mb_depth_reject);
        glUniform1f(s_fx_u.mb_clamp_px, (float)s_mb_clamp_px);
        glUniform1f(s_fx_u.velmax_mode, (float)s_velmax_mode);
        glUniform1f(s_fx_u.vec_style, (float)s_vec_style);
        glUniform1f(s_fx_u.vel_max_on,
                    (s_vmax_valid && s_vel_tex_valid && s_vel_source && gfx_vel_enabled
                     && s_velmax_mode > 0) ? 1.0f : 0.0f);
        glUniform2f(s_fx_u.vel_max_grid,
                    (float)(s_vel_w / 8 > 0 ? s_vel_w / 8 : 1),
                    (float)(s_vel_h / 8 > 0 ? s_vel_h / 8 : 1));
        glUniform1f(s_fx_u.light_count, (float)s_light_count);
        glUniform3fv(s_fx_u.light_pos, 4, &s_light_pos[0][0]);
        glUniform3fv(s_fx_u.light_pos_view, 4, &pvpos[0][0]);
        glUniform3fv(s_fx_u.light_color, 4, &s_light_color[0][0]);
        glUniform1fv(s_fx_u.light_radius, 4, s_light_radius);
        glUniform1f(s_fx_u.light_intensity, (float)s_light_intensity);
        glUniform1f(s_fx_u.light_shadows, s_light_shadows ? 1.0f : 0.0f);
        glUniform1f(s_fx_u.light_shadow_steps, (float)s_light_shadow_steps);
        glUniform1f(s_fx_u.probe_vis, s_probe_vis ? 1.0f : 0.0f);
        glUniform3f(s_fx_u.probe_center, s_probe_center[0], s_probe_center[1],
                    s_probe_center[2]);
        glUniform1f(s_fx_u.probe_spacing, (float)s_probe_spacing);
        glUniform1f(s_fx_u.probe_dim, (float)s_probe_dim);
        glUniform1f(s_fx_u.probe_radius, (float)s_probe_radius);
        glUniform1f(s_fx_u.probe_res, (float)s_probe_res);
        {
            int cols = s_probe_cols < 1 ? 1 : (s_probe_cols > 4 ? 4 : s_probe_cols);
            int rows = s_probe_rows < 1 ? 1 : (s_probe_rows > 4 ? 4 : s_probe_rows);
            int depth = s_probe_depth < 1 ? 1 : (s_probe_depth > 4 ? 4 : s_probe_depth);
            int pn = 0;
            for (int iz = 0; iz < depth && pn < PTC_PROBE_MAX; iz++) {
                for (int iy = 0; iy < rows && pn < PTC_PROBE_MAX; iy++) {
                    for (int ix = 0; ix < cols && pn < PTC_PROBE_MAX; ix++) {
                        float px = s_probe_center[0]
                                 + ((float)ix - (float)(cols - 1) * 0.5f) * s_probe_spacing;
                        float py = s_probe_center[1]
                                 + ((float)iy - (float)(rows - 1) * 0.5f) * s_probe_spacing;
                        float pz = s_probe_center[2]
                                 + ((float)iz - (float)(depth - 1) * 0.5f) * s_probe_spacing;
                        float cr = 0.02f, cg = 0.02f, cb = 0.02f;
                        for (int li = 0; li < s_light_count; li++) {
                            float dx = s_light_pos[li][0] - px;
                            float dy = s_light_pos[li][1] - py;
                            float dz = s_light_pos[li][2] - pz;
                            float dd = sqrtf(dx * dx + dy * dy + dz * dz);
                            if (dd >= s_light_radius[li]) continue;
                            float aa = 1.0f - dd / s_light_radius[li];
                            aa *= aa;
                            cr += s_light_color[li][0] * aa;
                            cg += s_light_color[li][1] * aa;
                            cb += s_light_color[li][2] * aa;
                        }
                        if (pn < PTC_PROBE_MAX) {
                            s_probe_pos_u[pn][0] = px;
                            s_probe_pos_u[pn][1] = py;
                            s_probe_pos_u[pn][2] = pz;
                            s_probe_col_u[pn][0] = cr;
                            s_probe_col_u[pn][1] = cg;
                            s_probe_col_u[pn][2] = cb;
                            pn++;
                        }
                    }
                }
            }
            glUniform3fv(s_fx_u.probe_pos, PTC_PROBE_MAX, &s_probe_pos_u[0][0]);
            glUniform3fv(s_fx_u.probe_col, PTC_PROBE_MAX, &s_probe_col_u[0][0]);
            glUniform1f(s_fx_u.probe_count, (float)pn);
        }
        glUniform1f(s_fx_u.bokeh_on, s_fx_bokeh_on ? 1.0f : 0.0f);
        glUniform1f(s_fx_u.bokeh_aspect, (float)s_fx_bokeh_aspect);
        glUniform1f(s_fx_u.bokeh_blades, (float)s_fx_bokeh_blades);
        glUniform1f(s_fx_u.bokeh_rot, (float)s_fx_bokeh_rot);
        glUniform1f(s_fx_u.anamorphic, (float)s_fx_anamorphic);
        glUniform1f(s_fx_u.bokeh_highlight, (float)s_fx_bokeh_highlight);
        glUniform1f(s_fx_u.cat_eye, (float)s_fx_cat_eye);
        glUniform1f(s_fx_u.denoise_path, s_denoise_path ? 1.0f : 0.0f);
        /* When the reduced-res prepass ran, the denoised colour arrives as
         * uColor and this block must stay off. */
        glUniform1f(s_fx_u.denoise_on,
                    (s_denoise_on && dn_div == 0) ? 1.0f : 0.0f);
        glUniform1f(s_fx_u.denoise_intensity, (float)s_denoise_intensity);
        glUniform1f(s_fx_u.denoise_passes, (float)s_denoise_passes);
        glUniform1f(s_fx_u.denoise_radius, (float)s_denoise_radius);
        glUniform1f(s_fx_u.denoise_frames, (float)s_denoise_frames);
        glUniform1f(s_fx_u.denoise_bounces, (float)s_denoise_bounces);
        glUniform1f(s_fx_u.vec_gain, (float)s_fx_vec_gain);
    }
    glUniform1f(s_fx_u.ssrtx_on, s_fx_ssrtx_on ? 1.0f : 0.0f);
    glUniform1f(s_fx_u.ssrtx_intensity, (float)s_fx_ssrtx_intensity);
    glUniform1f(s_fx_u.ssrtx_dist, (float)s_fx_ssrtx_distance);
    glUniform1f(s_fx_u.ssrtx_steps, (float)s_fx_ssrtx_steps);
    glUniform1f(s_fx_u.ssrtx_rays, (float)s_fx_ssrtx_rays);
    glUniform1f(s_fx_u.ssrtx_thick, (float)s_fx_ssrtx_thickness);
    glUniform1f(s_fx_u.ssrtx_rough, (float)s_fx_ssrtx_roughness / 100.0f);
    glUniform1f(s_fx_u.ssrtx_fade, (float)s_fx_ssrtx_fade);

    if (s_fx_u.ray_tex >= 0) glUniform1i(s_fx_u.ray_tex, 5);
    {
        /* Shaft tint follows the selected dynamic light when one is active,
         * otherwise a warm sun. */
        float rc[3] = { 1.0f, 0.86f, 0.62f };
        int li = s_ray_light;
        if (li >= 0 && li < 4 && li < s_light_count) {
            memcpy(rc, s_light_color[li], sizeof(rc));
        }
        glUniform3f(s_fx_u.ray_color, rc[0], rc[1], rc[2]);
    }
    glUniform1f(s_fx_u.ray_intensity, (float)s_ray_intensity);
    glUniform1f(s_fx_u.ray_debug, (float)s_ray_debug);

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
    glUniform1f(s_fx_u.dof_on,
                (s_fx_dof_on && dof_div == 0) ? 1.0f : 0.0f);
    glUniform1f(s_fx_u.dof_pre, dof_div > 0 ? 1.0f : 0.0f);
    float dof_focus = s_fx_dof_auto ? gfx_dof_focus_auto : (float)s_fx_dof_focus;
    if (dof_focus < 1.0f) dof_focus = 1.0f;
    glUniform1f(s_fx_u.dof_focus, dof_focus);
    glUniform1f(s_fx_u.dof_aperture, (float)s_fx_dof_aperture);
    glUniform1f(s_fx_u.dof_maxblur, (float)s_fx_dof_maxblur);
    glUniform1f(s_fx_u.dof_samples, (float)s_fx_dof_samples);
    glUniform1f(s_fx_u.gi_on,
                (s_fx_gi_on && s_fx_gi_intensity > 0 && !gi_low) ? 1.0f : 0.0f);
    glUniform1f(s_fx_u.gi_intensity, (float)s_fx_gi_intensity);
    glUniform1f(s_fx_u.gi_radius, (float)s_fx_gi_radius);
    glUniform1f(s_fx_u.gi_steps, (float)s_fx_gi_steps);
    glUniform1f(s_fx_u.gi_rays, (float)s_fx_gi_rays);
    glUniform1f(s_fx_u.debug, (float)s_fx_debug);
    glUniform1f(s_fx_u.normal_flip, (float)s_fx_normal_flip);
    glUniform1f(s_fx_u.grade_on, s_grade_on ? 1.0f : 0.0f);
    glUniform1f(s_fx_u.grade_op, (float)s_grade_op);
    glUniform1f(s_fx_u.grade_exposure, (float)s_grade_exposure);
    glUniform1f(s_fx_u.grade_white, (float)s_grade_white);
    glUniform1f(s_fx_u.grade_look, (float)s_grade_look);
    glUniform1f(s_fx_u.grade_strength, (float)s_grade_strength);
    glUniform1f(s_fx_u.grade_sat, (float)s_grade_sat);
    glUniform1f(s_fx_u.grade_contrast, (float)s_grade_contrast);
    glUniform1f(s_fx_u.grade_tint, (float)s_grade_tint);
    glUniform1f(s_fx_u.grade_debug, (float)s_grade_debug);

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
        /* Reduced-res AO already folded AO into the colour; in that case the
         * in-shader gather must stay off.  If the chain did not produce this
         * eye's buffer, fall back to the full-res gather. */
        int ao_here = ao_chain && s_ao_valid[eye];
        glUniform1f(s_fx_u.ao_on, ao_here ? 0.0f : (ao_on_eff ? 1.0f : 0.0f));
        if (s_fx_u.ao_probe >= 0) {
            glUniform1f(s_fx_u.ao_probe, ao_here ? 0.0f : (float)s_fx_ao_probe);
        }
        glActiveTexture(GL_TEXTURE0);
        /* Reduced-res denoise: the prepass already wrote the mixed result;
         * hand it to the FX shader as the scene colour.  The DOF prepass runs
         * on top of that and its finished colour replaces both; the AO apply
         * runs on top of that in turn. */
        glBindTexture(GL_TEXTURE_2D,
                      ao_here ? s_ao_otex[eye]
                      : (s_dof_valid[eye] ? s_dof_result[eye]
                      : (s_dn_valid[eye] ? s_dn_result[eye] : s_eye_tex[eye])));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_eye_depth[eye]);
        glUniform1f(s_fx_u.shear, gfx_eye_shear[eye]);
        glUniform1f(s_fx_u.toe, gfx_eye_toe[eye]);
        glUniform1f(s_fx_u.flipy, gfx_eye_flip[eye] ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, s_ray_valid[eye] ? s_ray_result[eye] : 0);
        glUniform1f(s_fx_u.ray_on, s_ray_valid[eye] ? 1.0f : 0.0f);
        if (s_fx_u.ray_light_uv >= 0) {
            glUniform2f(s_fx_u.ray_light_uv, s_ray_lightuv[eye][0],
                        s_ray_lightuv[eye][1]);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* GI added back after the per-eye FX draws, so it never adds a sampler to
     * the big shader.  Same slot relative to bloom as the legacy in-shader GI. */
    if (gi_low) stereo_gi_composite();

    glDisableVertexAttribArray(apos);
    glDisableVertexAttribArray(auv);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, 0);
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
    // Emissive capture for this eye starts empty and ends in
    // gfx_opengl_emissive_flush(), which gfx_pc.c calls after the scene range.
    s_em_batch_n = 0;
    s_em_verts_n = 0;
    s_em_capture_eye = eye & 1;
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
    double fx_t0 = 0.0;
    if (s_fx_dof_stage) {
        glFinish();
        fx_t0 = dof_now_ms();
    }
    int fx_ready = gfx_stereo_zbuf ? 0 : stereo_fx_pass();
    if (s_fx_dof_stage) {
        glFinish();
        if ((frame_count % 30) == 0) {
            __android_log_print(ANDROID_LOG_INFO, "SM64FX",
                    "FXSTAGE fx=%.2f dof=%d half=%d div=%d taps=%d",
                    dof_now_ms() - fx_t0, s_fx_dof_on, s_fx_dof_half,
                    s_fx_dof_div,
                    s_fx_dof_taps > 0 ? s_fx_dof_taps : s_fx_dof_samples);
        }
    }
    // Bloom: additive upsample of the blurred emissive buffer into the FX
    // output. Skipped entirely when the chain is off (default).
    if (fx_ready) stereo_bloom_pass();
    // Optional 2D stripe LUT look: its own tiny pass, only when look 6 is
    // selected and a valid file was loaded. s_lut_out_valid then redirects the
    // composite to the graded texture.
    if (fx_ready) stereo_grade_lut_pass();

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
                                 : (fx_ready
                                    ? (s_lut_out_valid[src] ? s_lut_out_tex[src]
                                                            : s_pp_tex[src])
                                    : (s_ao_valid[src] ? s_ao_otex[src]
                                                       : s_eye_tex[src]));
        // GLOW debug view: show the emissive buffer (or the blurred bloom
        // buffer) on its own, so it is obvious the glow comes from world-space
        // geometry and not from a screen-space blur of the final image.
        if (!zview && s_glow_debug == 1 && s_em_tex[src] != 0) {
            scene_tex = s_em_tex[src];
        } else if (!zview && s_glow_debug == 2 && s_bl_a[src] != 0) {
            scene_tex = s_bl_a[src];
        }
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
    if (s_vel_rendering) {
        int d = s_vel_div > 0 ? s_vel_div : 1;
        glViewport(x / d, y / d, width / d, height / d);
        return;
    }
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
    /* Flat path occluder mapping: remember the scene viewport so NDC can be
     * turned into gl_FragCoord pixels without a per-frame GL query. */
    s_occl_vp_x = ox;
    s_occl_vp_y = y;
    s_occl_vp_w = width;
    s_occl_vp_h = height;
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
    // Emissive tag: keep a copy of this batch for the half-res emissive buffer.
    // Only during an eye's colour pass -- the shadow and velocity replays would
    // otherwise record the same geometry twice.
    if (s_em_capture_eye >= 0 && gfx_emissive_on && gfx_emissive_strength > 0
            && opengl_prg != NULL && !s_shadow_rendering && !s_vel_rendering
            && !gfx_splat_active) {
        emissive_capture(buf_vbo, buf_vbo_len, opengl_prg->num_floats,
                         buf_vbo_num_tris, gfx_emissive_rgb, gfx_emissive_strength);
    }
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
