#include "ghostcon/render/gles.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *VERT_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_color;\n"
    "varying highp vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
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
static const char *FRAG_SRC =
    "precision mediump float;\n"
    "varying highp vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "uniform sampler2D u_atlas;\n"
    "void main() {\n"
    "    highp float a = texture2D(u_atlas, v_uv).a;\n"
    "    gl_FragColor = vec4(v_color.rgb, v_color.a * a);\n"
    "}\n";

struct ghostcon_gles {
    uint32_t viewport_w, viewport_h;

    GLuint program;
    GLint attr_pos, attr_uv, attr_color;
    GLint uniform_atlas;

    GLuint atlas_tex;
    uint32_t atlas_dim;

    ghostcon_vertex_t *verts;
    size_t vert_count, vert_cap;
};

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
ghostcon_gles_create(uint32_t viewport_w, uint32_t viewport_h)
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
    gles->uniform_atlas = glGetUniformLocation(gles->program, "u_atlas");

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
    free(gles->verts);
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, (GLsizei)dim, (GLsizei)dim, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, ghostcon_atlas_bitmap(atlas));
    gles->atlas_dim = dim;

    ghostcon_atlas_clear_dirty(atlas);
}

void
ghostcon_gles_begin(ghostcon_gles_t *gles, bool clear, float bg_r, float bg_g, float bg_b)
{
    gles->vert_count = 0;
    if (clear) {
        glClearColor(bg_r, bg_g, bg_b, 1.0f);
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
          float r, float g, float b, float a)
{
    ghostcon_vertex_t tl = { x,     y,     u0, v0, r, g, b, a };
    ghostcon_vertex_t tr = { x + w, y,     u1, v0, r, g, b, a };
    ghostcon_vertex_t bl = { x,     y + h, u0, v1, r, g, b, a };
    ghostcon_vertex_t br = { x + w, y + h, u1, v1, r, g, b, a };

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
       exactly this — sampling (0,0) always yields alpha=1.0. */
    push_quad(gles, x, y, w, h, 0.0f, 0.0f, 0.0f, 0.0f, r, g, b, a);
}

void
ghostcon_gles_push_glyph(ghostcon_gles_t *gles,
                          float x, float y, float w, float h,
                          const ghostcon_glyph_t *glyph,
                          float r, float g, float b, float a)
{
    push_quad(gles, x, y, w, h, glyph->u0, glyph->v0, glyph->u1, glyph->v1,
              r, g, b, a);
}

void
ghostcon_gles_end(ghostcon_gles_t *gles)
{
    glUseProgram(gles->program);
    glBindTexture(GL_TEXTURE_2D, gles->atlas_tex);
    glUniform1i(gles->uniform_atlas, 0);

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
    glEnableVertexAttribArray(gles->attr_pos);
    glEnableVertexAttribArray(gles->attr_uv);
    glEnableVertexAttribArray(gles->attr_color);

    if (gles->vert_count > 0)
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)gles->vert_count);

    glDisableVertexAttribArray(gles->attr_pos);
    glDisableVertexAttribArray(gles->attr_uv);
    glDisableVertexAttribArray(gles->attr_color);
}
