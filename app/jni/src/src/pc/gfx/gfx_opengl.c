#ifdef RAPI_GL

#include <stdint.h>
#include <stdbool.h>

// Stereo state lives in gfx_pc.c (set by the Leia bridge).
extern int gfx_stereo_enabled;
extern int gfx_stereo_eye;
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

struct ShaderProgram {
    uint32_t shader_id;
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[7];
    GLint uniform_locations[5];
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
    append_line(vs_buf, &vs_len, "gl_Position = aVtxPos;");
    append_line(vs_buf, &vs_len, "}");

    // Fragment shader
#ifdef USE_GLES
    append_line(fs_buf, &fs_len, "#version 100");
    append_line(fs_buf, &fs_len, "precision mediump float;");
#else
    append_line(fs_buf, &fs_len, "#version 120");
#endif

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
        append_line(fs_buf, &fs_len, "gl_FragColor = texel;");
    } else {
        append_line(fs_buf, &fs_len, "gl_FragColor = vec4(texel, 1.0);");
    }
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
int gfx_stereo_in_scale_pct = 100;   // pre-scale of each eye inside the SBS buffer
int gfx_stereo_debug_tint = 0;   // 1 = 50% red/blue overlay per half

static const char *s_comp_vs =
    "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
    "uniform float uScale;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos * uScale, 0.0, 1.0); }\n";
static const char *s_comp_fs =
    "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;\n"
    "uniform float uMode; uniform vec4 uSolid;\n"
    "void main(){\n"
    "  vec4 t = texture2D(uTex, vUV);\n"
    "  if (uMode > 1.5) gl_FragColor = vec4(t.rrr, 1.0);\n"
    "  else if (uMode > 0.5) gl_FragColor = uSolid;\n"
    "  else gl_FragColor = t;\n"
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
static int s_fx_dof_focus = 800;      // world units
static int s_fx_dof_aperture = 100;   // percent
static int s_fx_dof_maxblur = 12;     // pixels
static int s_fx_dof_samples = 16;

static int s_fx_gi_on = 0;
static int s_fx_gi_intensity = 100;   // percent
static int s_fx_gi_radius = 150;      // world units
static int s_fx_gi_steps = 6;
static int s_fx_gi_rays = 4;

static int s_fx_debug = 0;            // 0 off, 1 AO, 2 CoC, 3 GI
static int s_fx_normal_flip = 0;

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
    "float linZ(float d) {\n"
    "  float z = d * 2.0 - 1.0;\n"
    "  return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));\n"
    "}\n"
    "vec3 viewPos(vec2 cuv, float z) {\n"
    "  vec2 ndc = cuv * 2.0 - 1.0;\n"
    "  return vec3(ndc.x * z * uTanHalf * uAspect,\n"
    "              ndc.y * z * uTanHalf, -z);\n"
    "}\n"
    "vec2 canvasUV(vec2 eyeUV) {\n"
    "  return (eyeUV * uRes - uCanvas.xy) / uCanvas.zw;\n"
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
    "  if (uDebug > 2.5) gl_FragColor = vec4(gi * 3.0 * (uGIIntensity / 100.0), 1.0);\n"
    "  else if (uDebug > 1.5) gl_FragColor = vec4(vec3(clamp(cocPx / max(uMaxBlur, 1.0), 0.0, 1.0)), 1.0);\n"
    "  else if (uDebug > 0.5) gl_FragColor = vec4(vec3(ao), 1.0);\n"
    "  else {\n"
    "    vec3 outc = dofColor * ao + gi * (uGIIntensity / 100.0) * 0.6;\n"
    "    gl_FragColor = vec4(outc, 1.0);\n"
    "  }\n"
    "}\n";

static struct {
    GLint color, depth, res, canvas, nearv, farv, tanhalf, aspect, frame;
    GLint ao_on, ao_intensity, ao_radius, ao_samples, ao_bias, ao_power;
    GLint dof_on, dof_focus, dof_aperture, dof_maxblur, dof_samples;
    GLint gi_on, gi_intensity, gi_radius, gi_steps, gi_rays;
    GLint debug, normal_flip;
} s_fx_u;

static GLuint s_fx_prog = 0;
static int    s_fx_prog_state = 0;   // 0 = untried, 1 = ok, -1 = failed
static GLuint s_pp_fbo[2] = {0, 0};
static GLuint s_pp_tex[2] = {0, 0};
static int    s_pp_w = 0, s_pp_h = 0;

static int stereo_fx_any(void) {
    return s_fx_ao_on || s_fx_dof_on || s_fx_gi_on || s_fx_debug != 0;
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
        case 21: s_fx_gi_on = value ? 1 : 0; break;
        case 22: s_fx_gi_intensity = value < 0 ? 0 : value; break;
        case 23: s_fx_gi_radius = value < 1 ? 1 : value; break;
        case 24: s_fx_gi_steps = value < 1 ? 1 : (value > 12 ? 12 : value); break;
        case 25: s_fx_gi_rays = value < 1 ? 1 : (value > 8 ? 8 : value); break;
        case 31: s_fx_debug = value < 0 ? 0 : (value > 3 ? 3 : value); break;
        case 32: s_fx_normal_flip = value ? 1 : 0; break;
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
    s_fx_prog_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "SM64FX", "FX program ready (%u)", s_fx_prog);
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
    glUniform1f(s_fx_u.dof_focus, (float)s_fx_dof_focus);
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
        glBindFramebuffer(GL_FRAMEBUFFER, s_pp_fbo[eye]);
        glViewport(0, 0, s_eye_w, s_eye_h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_eye_tex[eye]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_eye_depth[eye]);
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
        glDeleteShader(vs);
        glDeleteShader(fs);
        __android_log_print(ANDROID_LOG_INFO, "SM64FBO", "composite prog=%u pos=%d uv=%d",
                            s_comp_prog, s_comp_pos, s_comp_uv);
    }
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

/* Blit both eye textures into the 2x1 side-by-side default framebuffer. */
void gfx_opengl_stereo_composite(int w, int h, int full_w, int full_h) {
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
        glViewport(eye * (full_w / 2), 0, full_w / 2, full_h);
        glBindTexture(GL_TEXTURE_2D, zview ? s_eye_depth[eye]
                                           : (fx_ready ? s_pp_tex[eye] : s_eye_tex[eye]));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

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
    if (gfx_stereo_wire) {
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
