#include "ghostcon/render/gles.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghostcon/term/kitty_graphics.h"

static const char *VERT_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_color;\n"
    "attribute vec3 a_bg;\n"
    "attribute float a_is_glyph;\n"
    "varying highp vec2 v_uv;\n"
    "varying highp vec4 v_color;\n"
    "varying highp vec3 v_bg;\n"
    "varying highp float v_is_glyph;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "    v_bg = a_bg;\n"
    "    v_is_glyph = a_is_glyph;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

/* v_uv is explicitly highp, overriding the mediump default below --
   mediump only guarantees ~10 bits of relative precision, which quantizes
   UV lookups into the atlas texture (ATLAS_DIM x ATLAS_DIM, glyph cells
   only a handful of texels each) enough to jitter/snap glyph edges by a
   texel or more, independent of GL_LINEAR filtering being set correctly.
   Ghostty's own desktop-GL renderer (src/renderer/shaders/glsl/
   cell_text.f.glsl) sidesteps this the same way in spirit, just via a
   different mechanism: it samples through sampler2DRect (unnormalized,
   texel-space coordinates), which has no comparable precision loss to
   begin with. sampler2DRect isn't available under GLES2 (desktop-GL-only
   extension), so the portable GLES2 equivalent is this explicit highp
   qualifier instead. v_color stays mediump -- color isn't the affected
   value, and mediump keeps everything else (varying storage size, GPU
   register pressure) unchanged from before. */
/* Luminance-based alpha correction, ported from Ghostty's own
   cell_text.f.glsl (USE_LINEAR_CORRECTION path). Defaults OFF
   (config.c's gamma_correct default) -- keep reading before turning
   it on.

   Ghostty only ever pairs this with USE_LINEAR_BLENDING: an
   sRGB-format framebuffer where the GPU blends in true linear light
   and re-encodes to sRGB on write, which naturally renders light-on-
   dark text thinner than expected -- the correction exists purely to
   fatten that back up to look like traditional gamma-incorrect
   blending (confirmed straight from Ghostty's own config docs:
   Config.zig's alpha-blending default is "linear-corrected" on
   Linux, pairing both flags together; "linear" alone, without the
   correction, is documented there as making light text "much
   thicker" -- so the correction's whole job is undoing a thinning
   effect that only exists when blending is actually linear).

   ghostcon's GBM/EGL surface is a plain (non-sRGB) format with
   standard fixed-function blending -- structurally Ghostty's
   `native` mode already, the same "gamma-incorrect blending" the
   correction curve exists to reproduce. Applying the curve on top of
   that (found live: enabled by default for one deploy) over-corrects:
   partial-coverage pixels get pushed darker to fight a thinning
   effect that was never present, which reads as thin strokes (built
   mostly from partial-coverage AA pixels) rendering visibly dimmer
   than thick ones (built mostly from full-coverage pixels) within
   the same glyph. Only useful again if ghostcon-core ever gains a
   real linear-blending framebuffer -- which now exists (see
   u_linear_blending below), though it turned out to matter far less
   than expected: see this file's next doc comment for why. */
/* Glyph quads (v_is_glyph > 0.5) take a different path from every
   other quad type (background fills, cursor, selection overlay):
   `mix(v_bg, v_color.rgb, mask)`, computed entirely in-shader from
   the mask sampled out of the (now RGB, 3 bytes/pixel) atlas, with NO
   reliance on GL's fixed-function blend. This is what makes real
   per-subpixel-channel blending possible at all under GLES2: standard
   alpha blending only has one alpha for every channel, but true
   subpixel rendering needs a different mix weight per R/G/B channel
   (that's the entire point of it -- see atlas.c's cleartype packing
   path). Every AA mode other than cleartype stores a replicated mask
   (R=G=B), which degenerates this exact formula to the plain scalar
   alpha blend every glyph used before this existed -- PROVIDED v_bg
   genuinely matches whatever's already under this glyph in the
   framebuffer, which holds here: machine.c always pushes a cell's
   background quad (with this same resolved color) before that cell's
   glyph quad, every frame. That equivalence is also why the old
   gamma-correction curve (still used below, for non-glyph quads only)
   has nothing left to do for glyphs -- it existed to compensate for
   coverage-alpha GL blending, and glyphs no longer go through GL
   blending at all.

   u_linear_blending (set when egl.c got an sRGB-colorspace surface
   AND the driver has GL_EXT_sRGB_write_control, both checked live --
   see core/main.c) matters for BOTH branches even though only
   non-glyph quads still use GL_BLEND: GL_FRAMEBUFFER_SRGB affects
   every fragment WRITE to an sRGB-encoded framebuffer, blended or
   not, auto-converting linear shader output to sRGB on write. This
   shader's colors (v_color/v_bg, and everything computed from them)
   are already in sRGB-encoded form -- writing them straight through
   with that hardware auto-conversion active would gamma-encode an
   already-gamma-encoded value a second time, visibly darkening/
   distorting EVERYTHING, glyphs included, not just the GL_BLEND
   quads. So both branches compute their result exactly as before,
   then linearize() the final RGB once, right before output, only
   when u_linear_blending is active -- undoing that pre-encoding so
   the hardware's forced re-encode on write lands back on the
   intended sRGB bytes. */
static const char *FRAG_SRC =
    "precision mediump float;\n"
    "varying highp vec2 v_uv;\n"
    "varying highp vec4 v_color;\n"
    "varying highp vec3 v_bg;\n"
    "varying highp float v_is_glyph;\n"
    "uniform sampler2D u_atlas;\n"
    "uniform bool u_gamma_correct;\n"
    "uniform bool u_linear_blending;\n"
    "highp float unlin1(highp float v) {\n"
    "    return v <= 0.0031308 ? v * 12.92 : pow(v, 1.0 / 2.4) * 1.055 - 0.055;\n"
    "}\n"
    "highp float lin1(highp float v) {\n"
    "    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);\n"
    "}\n"
    "highp float luminance(highp vec3 c) {\n"
    "    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;\n"
    "}\n"
    "highp vec3 linearize3(highp vec3 c) {\n"
    "    return vec3(lin1(c.r), lin1(c.g), lin1(c.b));\n"
    "}\n"
    "void main() {\n"
    "    highp vec3 mask = texture2D(u_atlas, v_uv).rgb;\n"
    "    highp vec3 out_rgb;\n"
    "    highp float out_a;\n"
    "    if (v_is_glyph > 0.5) {\n"
    "        out_rgb = mix(v_bg, v_color.rgb, mask);\n"
    "        out_a = 1.0;\n"
    "    } else {\n"
    "        highp float a = mask.r;\n"
    "        if (u_gamma_correct) {\n"
    "            highp float fg_l = luminance(v_color.rgb);\n"
    "            highp float bg_l = luminance(v_bg);\n"
    "            if (abs(fg_l - bg_l) > 0.001) {\n"
    "                highp float blend_l = lin1(unlin1(fg_l) * a + unlin1(bg_l) * (1.0 - a));\n"
    "                a = clamp((blend_l - bg_l) / (fg_l - bg_l), 0.0, 1.0);\n"
    "            }\n"
    "        }\n"
    "        out_rgb = v_color.rgb;\n"
    "        out_a = v_color.a * a;\n"
    "    }\n"
    "    if (u_linear_blending)\n"
    "        out_rgb = linearize3(out_rgb);\n"
    "    gl_FragColor = vec4(out_rgb, out_a);\n"
    "}\n";

/* Blit pass: copies the offscreen sRGB FBO's color texture to the
   real default framebuffer, one full-viewport triangle. GLES2 has no
   glBlitFramebuffer (GLES3+/desktop only), so this is the portable
   equivalent. GL_TEXTURE_SRGB_DECODE_EXT/GL_SKIP_DECODE_EXT on the
   source texture (set once at creation, see ghostcon_gles_create())
   makes texture2D() here return the RAW stored bytes instead of
   auto-decoding them to linear -- since those bytes are already
   correctly sRGB-encoded (the main FRAG_SRC's own linearize3() +
   the hardware's auto-reencode-on-write into the sRGB attachment
   already produced them), this blit is then a genuine byte-for-byte
   passthrough into the plain (non-sRGB) default framebuffer, no
   further conversion needed or wanted. */
static const char *BLIT_VERT_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying highp vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";
static const char *BLIT_FRAG_SRC =
    "precision mediump float;\n"
    "varying highp vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

struct ghostcon_gles {
    uint32_t viewport_w, viewport_h;

    GLuint program;
    GLint attr_pos, attr_uv, attr_color, attr_bg, attr_is_glyph;
    GLint uniform_atlas, uniform_gamma_correct, uniform_linear_blending;

    GLuint atlas_tex;
    uint32_t atlas_dim;

    bool gamma_correct;

    /* Real linear-space rendering: an offscreen sRGB-format FBO
       everything gets drawn into instead of the default framebuffer,
       blitted (via a tiny passthrough shader -- GLES2 has no
       glBlitFramebuffer) to the real one at the end of every frame.
       See ghostcon_gles_create()'s doc comment for why this exists
       instead of just requesting an sRGB EGL window surface. Only
       ever decided at creation time -- not something a live config
       reload rebuilds, same "startup-only" precedent already used
       for e.g. scrollback_lines/repeat_delay_ms. */
    bool linear_blending;
    GLuint srgb_fbo, srgb_color_tex;
    GLuint blit_program;
    GLint blit_attr_pos, blit_attr_uv, blit_uniform_tex;

    ghostcon_vertex_t *verts;
    size_t vert_count, vert_cap;

    /* Kitty graphics image quads -- separate pipeline, see gles.h's
       doc comment on this section. */
    GLuint img_program;
    GLint  img_attr_pos, img_attr_uv, img_uniform_tex, img_uniform_alpha, img_uniform_linear_blending;

    struct ghostcon_gles_queued_image *queued_images;
    size_t queued_count, queued_cap;

    struct ghostcon_gles_kitty_tex_entry *kitty_tex;
    size_t kitty_tex_count, kitty_tex_cap;
};

struct ghostcon_gles_kitty_tex_entry {
    uint32_t image_id;
    uint32_t generation;
    ghostcon_gles_image_t *tex;
};

struct ghostcon_gles_image {
    GLuint tex;
    int    width, height;
};

struct ghostcon_gles_queued_image {
    ghostcon_gles_image_t *img;
    float x, y, w, h;
    float src_x, src_y, src_w, src_h;
    float alpha;
};

/* Kitty graphics image quads -- deliberately its own tiny shader
   rather than reusing VERT_SRC/FRAG_SRC above: those sample the
   shared glyph atlas and carry gamma-correction/is_glyph machinery
   this doesn't need. Plain textured quad, uniform alpha multiplies
   whatever the texture's own alpha is (1.0 for RGB-format textures,
   giving a uniform placement opacity either way). */
static const char *IMG_VERT_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying highp vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *IMG_FRAG_SRC =
    "precision mediump float;\n"
    "varying highp vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_alpha;\n"
    "uniform bool u_linear_blending;\n"
    /* Same fix, same reason as FRAG_SRC's own lin1()/u_linear_blending
       doc comment above: when linear_blending is active, the target is
       an sRGB-format FBO that auto-encodes every fragment write. The
       uploaded texture holds ordinary sRGB-encoded 0-255 bytes (e.g. a
       Kitty image's raw RGB payload) sampled back as-is; writing those
       straight through under that auto-encode gets them gamma-encoded
       a SECOND time, washing the image out (found live: a checkerboard
       transmitted as (220,60,60)/(30,30,140) rendered as
       (239,133,133)/(96,96,196) -- exactly the shape of a double sRGB
       encode, which compresses hard near black and barely moves near
       white). Pre-linearizing here cancels the hardware's forced
       re-encode, same as FRAG_SRC already does for text/background. */
    "highp float lin1(highp float v) {\n"
    "    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_tex, v_uv);\n"
    "    vec3 rgb = c.rgb;\n"
    "    if (u_linear_blending)\n"
    "        rgb = vec3(lin1(rgb.r), lin1(rgb.g), lin1(rgb.b));\n"
    "    gl_FragColor = vec4(rgb, c.a * u_alpha);\n"
    "}\n";

static GLuint
compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "gles: shader compile failed: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

ghostcon_gles_t *
ghostcon_gles_create(uint32_t viewport_w, uint32_t viewport_h, bool want_linear_blending)
{
    ghostcon_gles_t *gles = calloc(1, sizeof(*gles));
    if (!gles)
        return NULL;
    gles->viewport_w = viewport_w;
    gles->viewport_h = viewport_h;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERT_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs)
        goto fail;

    gles->program = glCreateProgram();
    glAttachShader(gles->program, vs);
    glAttachShader(gles->program, fs);
    glLinkProgram(gles->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(gles->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(gles->program, sizeof(log), NULL, log);
        fprintf(stderr, "gles: program link failed: %s\n", log);
        goto fail;
    }

    gles->attr_pos = glGetAttribLocation(gles->program, "a_pos");
    gles->attr_uv = glGetAttribLocation(gles->program, "a_uv");
    gles->attr_color = glGetAttribLocation(gles->program, "a_color");
    gles->attr_bg = glGetAttribLocation(gles->program, "a_bg");
    gles->attr_is_glyph = glGetAttribLocation(gles->program, "a_is_glyph");
    gles->uniform_atlas = glGetUniformLocation(gles->program, "u_atlas");
    gles->uniform_gamma_correct = glGetUniformLocation(gles->program, "u_gamma_correct");
    gles->uniform_linear_blending = glGetUniformLocation(gles->program, "u_linear_blending");
    gles->gamma_correct = true;

    GLuint ivs = compile_shader(GL_VERTEX_SHADER, IMG_VERT_SRC);
    GLuint ifs = compile_shader(GL_FRAGMENT_SHADER, IMG_FRAG_SRC);
    if (ivs && ifs) {
        gles->img_program = glCreateProgram();
        glAttachShader(gles->img_program, ivs);
        glAttachShader(gles->img_program, ifs);
        glLinkProgram(gles->img_program);
        GLint img_linked = 0;
        glGetProgramiv(gles->img_program, GL_LINK_STATUS, &img_linked);
        if (img_linked) {
            gles->img_attr_pos = glGetAttribLocation(gles->img_program, "a_pos");
            gles->img_attr_uv = glGetAttribLocation(gles->img_program, "a_uv");
            gles->img_uniform_tex = glGetUniformLocation(gles->img_program, "u_tex");
            gles->img_uniform_alpha = glGetUniformLocation(gles->img_program, "u_alpha");
            gles->img_uniform_linear_blending =
                glGetUniformLocation(gles->img_program, "u_linear_blending");
        } else {
            char log[512];
            glGetProgramInfoLog(gles->img_program, sizeof(log), NULL, log);
            fprintf(stderr, "gles: image program link failed: %s\n", log);
            gles->img_program = 0;
        }
    }
    if (ivs) glDeleteShader(ivs);
    if (ifs) glDeleteShader(ifs);

    /* Real linear-space rendering via an offscreen sRGB FBO -- see
       this struct's own doc comment on why (an sRGB EGL window
       surface was tried and confirmed non-functional on this
       project's actual dev hardware/driver). Needs GL_EXT_sRGB
       (sRGB-format renderable textures) and
       GL_EXT_texture_sRGB_decode (GL_SKIP_DECODE_EXT for the blit
       pass) -- checked live, not assumed; falls back to plain
       rendering (this project's behavior before any of this existed)
       if either is missing or the FBO doesn't complete, not an
       error. */
    gles->linear_blending = false;
    if (want_linear_blending) {
        const char *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
        bool has_srgb = gl_exts && strstr(gl_exts, "GL_EXT_sRGB");
        bool has_decode_ctrl = gl_exts && strstr(gl_exts, "GL_EXT_texture_sRGB_decode");
        if (has_srgb && has_decode_ctrl) {
            glGenTextures(1, &gles->srgb_color_tex);
            glBindTexture(GL_TEXTURE_2D, gles->srgb_color_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB_ALPHA_EXT, (GLsizei)viewport_w, (GLsizei)viewport_h,
                         0, GL_SRGB_ALPHA_EXT, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);

            glGenFramebuffers(1, &gles->srgb_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, gles->srgb_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, gles->srgb_color_tex, 0);

            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                GLuint bvs = compile_shader(GL_VERTEX_SHADER, BLIT_VERT_SRC);
                GLuint bfs = compile_shader(GL_FRAGMENT_SHADER, BLIT_FRAG_SRC);
                if (bvs && bfs) {
                    gles->blit_program = glCreateProgram();
                    glAttachShader(gles->blit_program, bvs);
                    glAttachShader(gles->blit_program, bfs);
                    glLinkProgram(gles->blit_program);
                    GLint blit_linked = 0;
                    glGetProgramiv(gles->blit_program, GL_LINK_STATUS, &blit_linked);
                    if (blit_linked) {
                        gles->blit_attr_pos = glGetAttribLocation(gles->blit_program, "a_pos");
                        gles->blit_attr_uv = glGetAttribLocation(gles->blit_program, "a_uv");
                        gles->blit_uniform_tex = glGetUniformLocation(gles->blit_program, "u_tex");
                        gles->linear_blending = true;
                    }
                }
                if (bvs) glDeleteShader(bvs);
                if (bfs) glDeleteShader(bfs);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            if (!gles->linear_blending) {
                glDeleteTextures(1, &gles->srgb_color_tex);
                glDeleteFramebuffers(1, &gles->srgb_fbo);
                if (gles->blit_program) glDeleteProgram(gles->blit_program);
                gles->srgb_color_tex = 0;
                gles->srgb_fbo = 0;
                gles->blit_program = 0;
            }
        }
    }

    glGenTextures(1, &gles->atlas_tex);
    glBindTexture(GL_TEXTURE_2D, gles->atlas_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glViewport(0, 0, (GLsizei)viewport_w, (GLsizei)viewport_h);

    gles->vert_cap = 1024;
    gles->verts = malloc(gles->vert_cap * sizeof(ghostcon_vertex_t));

    return gles;

fail:
    if (gles->program)
        glDeleteProgram(gles->program);
    free(gles);
    return NULL;
}

void
ghostcon_gles_destroy(ghostcon_gles_t *gles)
{
    if (!gles)
        return;
    glDeleteTextures(1, &gles->atlas_tex);
    glDeleteProgram(gles->program);
    if (gles->linear_blending) {
        glDeleteTextures(1, &gles->srgb_color_tex);
        glDeleteFramebuffers(1, &gles->srgb_fbo);
        glDeleteProgram(gles->blit_program);
    }
    free(gles->verts);
    free(gles->queued_images);
    for (size_t i = 0; i < gles->kitty_tex_count; i++)
        ghostcon_gles_image_destroy(gles->kitty_tex[i].tex);
    free(gles->kitty_tex);
    if (gles->img_program) glDeleteProgram(gles->img_program);
    free(gles);
}

void
ghostcon_gles_resize(ghostcon_gles_t *gles, uint32_t w, uint32_t h)
{
    gles->viewport_w = w;
    gles->viewport_h = h;
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
}

void
ghostcon_gles_sync_atlas(ghostcon_gles_t *gles, ghostcon_atlas_t *atlas, bool force)
{
    if (!force && !ghostcon_atlas_dirty(atlas))
        return;

    uint32_t dim = ghostcon_atlas_dim(atlas);
    glBindTexture(GL_TEXTURE_2D, gles->atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)dim, (GLsizei)dim, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, ghostcon_atlas_bitmap(atlas));
    gles->atlas_dim = dim;

    ghostcon_atlas_clear_dirty(atlas);
}

/* Matches FRAG_SRC's lin1() exactly -- clearing an sRGB-attached FBO
   goes through the same hardware auto-reencode-on-write as any other
   fragment write to it, so the clear color needs the same
   pre-linearization or the cleared background would come out
   double-gamma-encoded, same class of bug as an un-linearized shader
   output. */
static float
linearize_channel(float v)
{
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

void
ghostcon_gles_begin(ghostcon_gles_t *gles, bool clear, float bg_r, float bg_g, float bg_b)
{
    gles->vert_count = 0;
    if (gles->linear_blending)
        glBindFramebuffer(GL_FRAMEBUFFER, gles->srgb_fbo);
    if (clear) {
        if (gles->linear_blending) {
            glClearColor(linearize_channel(bg_r), linearize_channel(bg_g),
                         linearize_channel(bg_b), 1.0f);
        } else {
            glClearColor(bg_r, bg_g, bg_b, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

static void
push_vertex(ghostcon_gles_t *gles, ghostcon_vertex_t v)
{
    if (gles->vert_count >= gles->vert_cap) {
        gles->vert_cap *= 2;
        gles->verts = realloc(gles->verts, gles->vert_cap * sizeof(ghostcon_vertex_t));
    }
    /* Pixel space (top-left origin, y-down) -> NDC. */
    v.x = (v.x / (float)gles->viewport_w) * 2.0f - 1.0f;
    v.y = 1.0f - (v.y / (float)gles->viewport_h) * 2.0f;
    gles->verts[gles->vert_count++] = v;
}

static void
push_quad(ghostcon_gles_t *gles,
          float x, float y, float w, float h,
          float u0, float v0, float u1, float v1,
          float r, float g, float b, float a,
          float bg_r, float bg_g, float bg_b,
          float is_glyph)
{
    ghostcon_vertex_t tl = { x,     y,     u0, v0, r, g, b, a, bg_r, bg_g, bg_b, is_glyph };
    ghostcon_vertex_t tr = { x + w, y,     u1, v0, r, g, b, a, bg_r, bg_g, bg_b, is_glyph };
    ghostcon_vertex_t bl = { x,     y + h, u0, v1, r, g, b, a, bg_r, bg_g, bg_b, is_glyph };
    ghostcon_vertex_t br = { x + w, y + h, u1, v1, r, g, b, a, bg_r, bg_g, bg_b, is_glyph };

    push_vertex(gles, tl);
    push_vertex(gles, bl);
    push_vertex(gles, tr);
    push_vertex(gles, tr);
    push_vertex(gles, bl);
    push_vertex(gles, br);
}

void
ghostcon_gles_push_rect(ghostcon_gles_t *gles,
                         float x, float y, float w, float h,
                         float r, float g, float b, float a)
{
    /* atlas.c reserves an opaque 2x2 block at the atlas origin for
       exactly this — sampling (0,0) always yields mask=(1,1,1). bg == fg
       here so the gamma correction curve is a guaranteed no-op, and
       is_glyph=0 keeps this on the plain alpha-blended path (needed
       for e.g. the selection overlay's partial-alpha tint). */
    push_quad(gles, x, y, w, h, 0.0f, 0.0f, 0.0f, 0.0f, r, g, b, a, r, g, b, 0.0f);
}

void
ghostcon_gles_push_glyph(ghostcon_gles_t *gles,
                          float x, float y, float w, float h,
                          const ghostcon_glyph_t *glyph,
                          float r, float g, float b, float a,
                          float bg_r, float bg_g, float bg_b)
{
    push_quad(gles, x, y, w, h, glyph->u0, glyph->v0, glyph->u1, glyph->v1,
              r, g, b, a, bg_r, bg_g, bg_b, 1.0f);
}

void
ghostcon_gles_set_gamma_correct(ghostcon_gles_t *gles, bool enabled)
{
    gles->gamma_correct = enabled;
}

bool
ghostcon_gles_linear_blending_active(const ghostcon_gles_t *gles)
{
    return gles->linear_blending;
}

ghostcon_gles_image_t *
ghostcon_gles_image_create(const uint8_t *pixels, int width, int height, int bpp)
{
    if (width <= 0 || height <= 0 || (bpp != 3 && bpp != 4))
        return NULL;
    ghostcon_gles_image_t *img = calloc(1, sizeof(*img));
    if (!img)
        return NULL;
    img->width = width;
    img->height = height;

    GLenum fmt = (bpp == 4) ? GL_RGBA : GL_RGB;
    glGenTextures(1, &img->tex);
    glBindTexture(GL_TEXTURE_2D, img->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, width, height, 0,
                 fmt, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return img;
}

void
ghostcon_gles_image_destroy(ghostcon_gles_image_t *img)
{
    if (!img)
        return;
    glDeleteTextures(1, &img->tex);
    free(img);
}

static void
draw_image_internal(ghostcon_gles_t *gles, ghostcon_gles_image_t *img,
                    float x, float y, float w, float h,
                    float src_x, float src_y, float src_w, float src_h,
                    float alpha)
{
    if (!gles->img_program || !img)
        return;

    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (src_w > 0.0f && src_h > 0.0f) {
        u0 = src_x / (float)img->width;
        v0 = src_y / (float)img->height;
        u1 = (src_x + src_w) / (float)img->width;
        v1 = (src_y + src_h) / (float)img->height;
    }

    /* Pixel space (top-left origin, y-down) -> NDC -- same formula as
       push_vertex() above, duplicated rather than shared since this
       writes into a small stack array, not the batch's verts[]. */
    float x0 = (x / (float)gles->viewport_w) * 2.0f - 1.0f;
    float x1 = ((x + w) / (float)gles->viewport_w) * 2.0f - 1.0f;
    float y0 = 1.0f - (y / (float)gles->viewport_h) * 2.0f;
    float y1 = 1.0f - ((y + h) / (float)gles->viewport_h) * 2.0f;

    float quad[24] = {
        x0, y0, u0, v0,
        x0, y1, u0, v1,
        x1, y0, u1, v0,
        x1, y0, u1, v0,
        x0, y1, u0, v1,
        x1, y1, u1, v1,
    };

    glUseProgram(gles->img_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, img->tex);
    glUniform1i(gles->img_uniform_tex, 0);
    glUniform1f(gles->img_uniform_alpha, alpha);
    glUniform1i(gles->img_uniform_linear_blending, gles->linear_blending ? 1 : 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glVertexAttribPointer(gles->img_attr_pos, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), &quad[0]);
    glVertexAttribPointer(gles->img_attr_uv, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), &quad[2]);
    glEnableVertexAttribArray(gles->img_attr_pos);
    glEnableVertexAttribArray(gles->img_attr_uv);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(gles->img_attr_pos);
    glDisableVertexAttribArray(gles->img_attr_uv);
}

void
ghostcon_gles_draw_image_now(ghostcon_gles_t *gles, ghostcon_gles_image_t *img,
                             float x, float y, float w, float h,
                             float src_x, float src_y, float src_w, float src_h,
                             float alpha)
{
    draw_image_internal(gles, img, x, y, w, h, src_x, src_y, src_w, src_h, alpha);
}

ghostcon_gles_image_t *
ghostcon_gles_kitty_tex_get(ghostcon_gles_t *gles,
                           const struct ghostcon_kitty_graphics *kg,
                           uint32_t image_id, uint32_t generation,
                           const uint8_t *pixels, int width, int height, int bpp)
{
    for (size_t i = 0; i < gles->kitty_tex_count; i++) {
        struct ghostcon_gles_kitty_tex_entry *e = &gles->kitty_tex[i];
        if (e->image_id == image_id) {
            if (e->generation == generation)
                return e->tex;
            /* Re-transmitted under the same id -- swap the texture. */
            ghostcon_gles_image_destroy(e->tex);
            e->tex = ghostcon_gles_image_create(pixels, width, height, bpp);
            e->generation = generation;
            return e->tex;
        }
    }

    if (gles->kitty_tex_count >= GHOSTCON_KITTY_MAX_IMAGES) {
        /* Under pressure: evict whichever cached id no longer resolves
           in the current image store (genuinely deleted), else the
           oldest entry. */
        size_t victim = 0;
        for (size_t i = 0; i < gles->kitty_tex_count; i++) {
            if (!kg || !ghostcon_kitty_graphics_find_image(kg, gles->kitty_tex[i].image_id)) {
                victim = i;
                break;
            }
        }
        ghostcon_gles_image_destroy(gles->kitty_tex[victim].tex);
        memmove(&gles->kitty_tex[victim], &gles->kitty_tex[victim + 1],
               (gles->kitty_tex_count - victim - 1) * sizeof(*gles->kitty_tex));
        gles->kitty_tex_count--;
    }

    if (gles->kitty_tex_count >= gles->kitty_tex_cap) {
        gles->kitty_tex_cap = gles->kitty_tex_cap ? gles->kitty_tex_cap * 2 : 8;
        gles->kitty_tex = realloc(gles->kitty_tex, gles->kitty_tex_cap * sizeof(*gles->kitty_tex));
    }

    ghostcon_gles_image_t *tex = ghostcon_gles_image_create(pixels, width, height, bpp);
    if (!tex)
        return NULL;
    gles->kitty_tex[gles->kitty_tex_count++] = (struct ghostcon_gles_kitty_tex_entry){
        image_id, generation, tex,
    };
    return tex;
}

void
ghostcon_gles_queue_image(ghostcon_gles_t *gles, ghostcon_gles_image_t *img,
                          float x, float y, float w, float h,
                          float src_x, float src_y, float src_w, float src_h,
                          float alpha)
{
    if (gles->queued_count >= gles->queued_cap) {
        gles->queued_cap = gles->queued_cap ? gles->queued_cap * 2 : 8;
        gles->queued_images = realloc(gles->queued_images,
                                      gles->queued_cap * sizeof(*gles->queued_images));
    }
    gles->queued_images[gles->queued_count++] = (struct ghostcon_gles_queued_image){
        img, x, y, w, h, src_x, src_y, src_w, src_h, alpha,
    };
}

void
ghostcon_gles_end(ghostcon_gles_t *gles)
{
    glUseProgram(gles->program);
    glBindTexture(GL_TEXTURE_2D, gles->atlas_tex);
    glUniform1i(gles->uniform_atlas, 0);
    glUniform1i(gles->uniform_gamma_correct, gles->gamma_correct ? 1 : 0);
    glUniform1i(gles->uniform_linear_blending, gles->linear_blending ? 1 : 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glVertexAttribPointer(gles->attr_pos, 2, GL_FLOAT, GL_FALSE,
                           sizeof(ghostcon_vertex_t),
                           &gles->verts[0].x);
    glVertexAttribPointer(gles->attr_uv, 2, GL_FLOAT, GL_FALSE,
                           sizeof(ghostcon_vertex_t),
                           &gles->verts[0].u);
    glVertexAttribPointer(gles->attr_color, 4, GL_FLOAT, GL_FALSE,
                           sizeof(ghostcon_vertex_t),
                           &gles->verts[0].r);
    glVertexAttribPointer(gles->attr_bg, 3, GL_FLOAT, GL_FALSE,
                           sizeof(ghostcon_vertex_t),
                           &gles->verts[0].bg_r);
    glVertexAttribPointer(gles->attr_is_glyph, 1, GL_FLOAT, GL_FALSE,
                           sizeof(ghostcon_vertex_t),
                           &gles->verts[0].is_glyph);
    glEnableVertexAttribArray(gles->attr_pos);
    glEnableVertexAttribArray(gles->attr_uv);
    glEnableVertexAttribArray(gles->attr_color);
    glEnableVertexAttribArray(gles->attr_bg);
    glEnableVertexAttribArray(gles->attr_is_glyph);

    if (gles->vert_count > 0)
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)gles->vert_count);

    glDisableVertexAttribArray(gles->attr_pos);
    glDisableVertexAttribArray(gles->attr_uv);
    glDisableVertexAttribArray(gles->attr_color);
    glDisableVertexAttribArray(gles->attr_bg);
    glDisableVertexAttribArray(gles->attr_is_glyph);

    /* z>=0 image placements: drawn now, after the text/background
       batch above but before the sRGB blit/swap below -- see gles.h's
       doc comment on why draw call TIMING is what controls paint
       order for these, not vertex order. */
    for (size_t i = 0; i < gles->queued_count; i++) {
        struct ghostcon_gles_queued_image *q = &gles->queued_images[i];
        draw_image_internal(gles, q->img, q->x, q->y, q->w, q->h,
                            q->src_x, q->src_y, q->src_w, q->src_h, q->alpha);
    }
    gles->queued_count = 0;

    if (gles->linear_blending) {
        /* Everything above was drawn into the offscreen sRGB FBO --
           blit it (a full-viewport textured quad; GLES2 has no
           glBlitFramebuffer) to the real default framebuffer, which
           is what the caller's eglSwapBuffers() actually presents. */
        static const float quad[] = {
            /*  x,     y,    u,   v */
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 1.0f,
        };

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_BLEND);

        glUseProgram(gles->blit_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gles->srgb_color_tex);
        glUniform1i(gles->blit_uniform_tex, 0);

        glVertexAttribPointer(gles->blit_attr_pos, 2, GL_FLOAT, GL_FALSE,
                               4 * sizeof(float), &quad[0]);
        glVertexAttribPointer(gles->blit_attr_uv, 2, GL_FLOAT, GL_FALSE,
                               4 * sizeof(float), &quad[2]);
        glEnableVertexAttribArray(gles->blit_attr_pos);
        glEnableVertexAttribArray(gles->blit_attr_uv);

        glDrawArrays(GL_TRIANGLES, 0, 6);

        glDisableVertexAttribArray(gles->blit_attr_pos);
        glDisableVertexAttribArray(gles->blit_attr_uv);
    }
}

bool
ghostcon_gles_screenshot_ppm(ghostcon_gles_t *gles, const char *path)
{
    uint32_t w = gles->viewport_w, h = gles->viewport_h;
    uint8_t *pixels = malloc((size_t)w * h * 4);
    if (!pixels)
        return false;
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(pixels);
        return false;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);

    /* GL's origin is bottom-left; PPM rows are written top-down. */
    uint8_t *row_rgb = malloc((size_t)w * 3);
    bool ok = row_rgb != NULL;
    for (uint32_t y = 0; ok && y < h; y++) {
        const uint8_t *src = pixels + (size_t)(h - 1 - y) * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            row_rgb[x * 3 + 0] = src[x * 4 + 0];
            row_rgb[x * 3 + 1] = src[x * 4 + 1];
            row_rgb[x * 3 + 2] = src[x * 4 + 2];
        }
        if (fwrite(row_rgb, 1, (size_t)w * 3, f) != (size_t)w * 3)
            ok = false;
    }

    free(row_rgb);
    fclose(f);
    free(pixels);
    return ok;
}
