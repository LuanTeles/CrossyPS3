/* Software renderer: flat-shaded triangles, z-buffer, planar ground shadows.
 *
 * Why software and not RSX shaders: the original game is a few thousand
 * flat-coloured triangles, which the Cell PPU handles fine at 640x360.
 * Rendering into a plain buffer keeps the PS3-specific code tiny (just a
 * framebuffer flip) and means the exact same code runs in the desktop test
 * harness, so what we test here is what runs on the console.
 *
 * Hot loops use fixed-point integers: on the PPU every float->int
 * conversion is a load-hit-store stall, so those happen per scanline, not
 * per pixel.
 */
#include <math.h>
#include <string.h>
#include <limits.h>
#include "render.h"

unsigned int rb_color[RW * RH];
static int rb_depth[RW * RH];
static unsigned char rb_stencil[RW * RH];

static float lin_of_srgb[256];
static unsigned char srgb_of_lin[4096];
static unsigned char shadow_lut[256];

/* camera basis (three.js lookAt from (-1, 2.8, -2.9) to the origin) */
static float cam_pos[3] = { -1.0f, 2.8f, -2.9f };
static float cam_r[3], cam_u[3], cam_f[3];
static float pix_per_unit;
static float scr_cx, scr_cy;

/* light: DirectionalLight(1.0) at (20,30,0.05) + AmbientLight(1.8), Lambert */
static float light_dir[3];
#define ZSCALE 65536.0f

static void v_norm(float *v) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

void r_init(void) {
    int i;
    for (i = 0; i < 256; i++) {
        float c = i / 255.0f;
        lin_of_srgb[i] = (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    for (i = 0; i < 4096; i++) {
        float l = i / 4095.0f, s;
        s = (l <= 0.0031308f) ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
        srgb_of_lin[i] = (unsigned char)(s * 255.0f + 0.5f);
    }
    /* shadowed ground only receives ambient: (1.8)/(1.8+0.832) of lit top faces */
    for (i = 0; i < 256; i++) {
        float l = lin_of_srgb[i] * (1.8f / 2.632f);
        int k = (int)(l * 4095.0f + 0.5f);
        shadow_lut[i] = srgb_of_lin[k > 4095 ? 4095 : k];
    }
    {
        float zc[3] = { cam_pos[0], cam_pos[1], cam_pos[2] };
        v_norm(zc);
        cam_f[0] = -zc[0]; cam_f[1] = -zc[1]; cam_f[2] = -zc[2];
        /* x = up cross z */
        cam_r[0] = zc[2]; cam_r[1] = 0; cam_r[2] = -zc[0];
        v_norm(cam_r);
        /* y = z cross x */
        cam_u[0] = zc[1] * cam_r[2] - zc[2] * cam_r[1];
        cam_u[1] = zc[2] * cam_r[0] - zc[0] * cam_r[2];
        cam_u[2] = zc[0] * cam_r[1] - zc[1] * cam_r[0];
    }
    /* Engine.init() sets camera.position.z = 1 after the constructor's
       lookAt, so the view keeps its angle but is shifted. Match that. */
    cam_pos[2] = 1.0f;
    light_dir[0] = 20; light_dir[1] = 30; light_dir[2] = 0.05f;
    v_norm(light_dir);
}

void r_xform_identity(Xform *x) {
    memset(x, 0, sizeof(*x));
    x->scale[0] = x->scale[1] = x->scale[2] = 1;
    x->pre_scale = 1;
}

void r_begin(unsigned int clear_rgb, float view_half_height, float shake_x, float shake_y) {
    int i;
    for (i = 0; i < RW * RH; i++) { rb_color[i] = clear_rgb; rb_depth[i] = INT_MAX; }
    pix_per_unit = (RH * 0.5f) / view_half_height;
    scr_cx = RW * 0.5f + shake_x;
    scr_cy = RH * 0.5f + shake_y;
}

void r_shadow_pass_begin(void) { memset(rb_stencil, 0, sizeof(rb_stencil)); }

/* ---------------------------------------------------------------- */

typedef struct { float m[9]; } M3;

static void build_rot(const float r[3], M3 *o) {
    /* R = Rx * Ry * Rz */
    float cx = cosf(r[0]), sx = sinf(r[0]);
    float cy = cosf(r[1]), sy = sinf(r[1]);
    float cz = cosf(r[2]), sz = sinf(r[2]);
    o->m[0] = cy * cz;                  o->m[1] = -cy * sz;                 o->m[2] = sy;
    o->m[3] = cx * sz + sx * sy * cz;   o->m[4] = cx * cz - sx * sy * sz;   o->m[5] = -sx * cy;
    o->m[6] = sx * sz - cx * sy * cz;   o->m[7] = sx * cz + cx * sy * sz;   o->m[8] = cx * cy;
}

static inline void xf_point(const Xform *x, const M3 *R, const float *v, float *o) {
    float a = (x->pre_off[0] + x->pre_scale * v[0]) * x->scale[0];
    float b = (x->pre_off[1] + x->pre_scale * v[1]) * x->scale[1];
    float c = (x->pre_off[2] + x->pre_scale * v[2]) * x->scale[2];
    o[0] = x->pos[0] + R->m[0] * a + R->m[1] * b + R->m[2] * c;
    o[1] = x->pos[1] + R->m[3] * a + R->m[4] * b + R->m[5] * c;
    o[2] = x->pos[2] + R->m[6] * a + R->m[7] * b + R->m[8] * c;
}

static inline void project(const float *p, float *s) {
    float d0 = p[0] - cam_pos[0], d1 = p[1] - cam_pos[1], d2 = p[2] - cam_pos[2];
    s[0] = scr_cx + (d0 * cam_r[0] + d1 * cam_r[1] + d2 * cam_r[2]) * pix_per_unit;
    s[1] = scr_cy - (d0 * cam_u[0] + d1 * cam_u[1] + d2 * cam_u[2]) * pix_per_unit;
    s[2] = d0 * cam_f[0] + d1 * cam_f[1] + d2 * cam_f[2];
}

static unsigned int shade(unsigned int rgb, const float *n) {
    float d = n[0] * light_dir[0] + n[1] * light_dir[1] + n[2] * light_dir[2];
    float k = (1.8f + (d > 0 ? d : 0)) * 0.318309886f;
    int r = (int)(lin_of_srgb[(rgb >> 16) & 255] * k * 4095.0f);
    int g = (int)(lin_of_srgb[(rgb >> 8) & 255] * k * 4095.0f);
    int b = (int)(lin_of_srgb[rgb & 255] * k * 4095.0f);
    if (r > 4095) r = 4095;
    if (g > 4095) g = 4095;
    if (b > 4095) b = 4095;
    return ((unsigned int)srgb_of_lin[r] << 16) | ((unsigned int)srgb_of_lin[g] << 8) | srgb_of_lin[b];
}

/* mode 0: opaque (depth test+write). mode 1: shadow (depth test with bias,
   stencil so overlapping shadows don't double-darken, no depth write). */
static void raster(const float *a, const float *b, const float *c, unsigned int col, int mode) {
    const float *v0 = a, *v1 = b, *v2 = c, *t;
    float area, dzdx, dzdy;
    int y, ys, ye;
    if (v1[1] < v0[1]) { t = v0; v0 = v1; v1 = t; }
    if (v2[1] < v0[1]) { t = v0; v0 = v2; v2 = t; }
    if (v2[1] < v1[1]) { t = v1; v1 = v2; v2 = t; }
    if (v2[1] - v0[1] < 1e-6f) return;
    area = (v1[0] - v0[0]) * (v2[1] - v0[1]) - (v2[0] - v0[0]) * (v1[1] - v0[1]);
    if (area > -1e-6f && area < 1e-6f) return;
    dzdx = ((v1[2] - v0[2]) * (v2[1] - v0[1]) - (v2[2] - v0[2]) * (v1[1] - v0[1])) / area;
    dzdy = ((v2[2] - v0[2]) * (v1[0] - v0[0]) - (v1[2] - v0[2]) * (v2[0] - v0[0])) / area;

    ys = (int)ceilf(v0[1] - 0.5f);
    ye = (int)ceilf(v2[1] - 0.5f);
    if (ys < 0) ys = 0;
    if (ye > RH) ye = RH;
    {
        int dzx = (int)(dzdx * ZSCALE);
        float inv02 = 1.0f / (v2[1] - v0[1]);
        int bias = (mode == 1) ? (int)(0.03f * ZSCALE) : 0;
        for (y = ys; y < ye; y++) {
            float py = y + 0.5f;
            float xa = v0[0] + (v2[0] - v0[0]) * (py - v0[1]) * inv02;
            float xb;
            int xs, xe, x, z, row;
            if (py < v1[1]) {
                float dy = v1[1] - v0[1];
                xb = (dy > 1e-6f) ? v0[0] + (v1[0] - v0[0]) * (py - v0[1]) / dy : v1[0];
            } else {
                float dy = v2[1] - v1[1];
                xb = (dy > 1e-6f) ? v1[0] + (v2[0] - v1[0]) * (py - v1[1]) / dy : v1[0];
            }
            if (xa > xb) { float s = xa; xa = xb; xb = s; }
            xs = (int)ceilf(xa - 0.5f);
            xe = (int)ceilf(xb - 0.5f);
            if (xs < 0) xs = 0;
            if (xe > RW) xe = RW;
            if (xs >= xe) continue;
            z = (int)((v0[2] + dzdx * (xs + 0.5f - v0[0]) + dzdy * (py - v0[1])) * ZSCALE);
            row = y * RW;
            if (mode == 0) {
                int *zb = rb_depth + row;
                unsigned int *cb = rb_color + row;
                for (x = xs; x < xe; x++, z += dzx) {
                    if (z < zb[x]) { zb[x] = z; cb[x] = col; }
                }
            } else {
                int *zb = rb_depth + row;
                unsigned int *cb = rb_color + row;
                unsigned char *sb = rb_stencil + row;
                for (x = xs; x < xe; x++, z += dzx) {
                    if (!sb[x] && zb[x] != INT_MAX && z - bias <= zb[x]) {
                        unsigned int p = cb[x];
                        sb[x] = 1;
                        cb[x] = ((unsigned int)shadow_lut[(p >> 16) & 255] << 16) |
                                ((unsigned int)shadow_lut[(p >> 8) & 255] << 8) | shadow_lut[p & 255];
                    }
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- */

#define MAXV 1024
static float tv[MAXV][3];   /* world */
static float sv[MAXV][3];   /* screen */

int r_mesh_on_screen(const Mesh *m, const Xform *x) {
    M3 R;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    int i;
    build_rot(x->rot, &R);
    for (i = 0; i < 8; i++) {
        float v[3], w[3], s[3];
        v[0] = (i & 1) ? m->max[0] : m->min[0];
        v[1] = (i & 2) ? m->max[1] : m->min[1];
        v[2] = (i & 4) ? m->max[2] : m->min[2];
        xf_point(x, &R, v, w);
        project(w, s);
        if (s[0] < minx) minx = s[0];
        if (s[0] > maxx) maxx = s[0];
        if (s[1] < miny) miny = s[1];
        if (s[1] > maxy) maxy = s[1];
    }
    return !(maxx < 0 || minx > RW || maxy < 0 || miny > RH);
}

void r_draw_mesh(const Mesh *m, const Xform *x, unsigned int color_override) {
    M3 R;
    int i;
    if (m->nverts > MAXV || !r_mesh_on_screen(m, x)) return;
    build_rot(x->rot, &R);
    for (i = 0; i < m->nverts; i++) {
        xf_point(x, &R, m->verts + i * 3, tv[i]);
        project(tv[i], sv[i]);
    }
    for (i = 0; i < m->ntris; i++) {
        const MeshTri *t = &m->tris[i];
        float n[3];
        const float *ln = t->n;
        /* normal: inverse-scale then rotate (handles squash/stretch tweens) */
        float a = ln[0] / x->scale[0], b = ln[1] / x->scale[1], c = ln[2] / x->scale[2];
        n[0] = R.m[0] * a + R.m[1] * b + R.m[2] * c;
        n[1] = R.m[3] * a + R.m[4] * b + R.m[5] * c;
        n[2] = R.m[6] * a + R.m[7] * b + R.m[8] * c;
        if (n[0] * cam_f[0] + n[1] * cam_f[1] + n[2] * cam_f[2] >= 0) continue; /* back face */
        v_norm(n);
        raster(sv[t->idx[0]], sv[t->idx[1]], sv[t->idx[2]],
               shade(color_override ? color_override : t->color, n), 0);
    }
}

void r_draw_shadow(const Mesh *m, const Xform *x, float plane_y) {
    M3 R;
    int i;
    if (m->nverts > MAXV) return;
    build_rot(x->rot, &R);
    for (i = 0; i < m->nverts; i++) {
        float w[3], k;
        xf_point(x, &R, m->verts + i * 3, w);
        /* project along the light direction onto y = plane_y */
        k = (w[1] - plane_y) / light_dir[1];
        if (k < 0) k = 0;
        w[0] -= light_dir[0] * k;
        w[2] -= light_dir[2] * k;
        w[1] = plane_y + 0.002f;
        project(w, sv[i]);
    }
    for (i = 0; i < m->ntris; i++) {
        const MeshTri *t = &m->tris[i];
        const float *ln = t->n;
        float ny = R.m[3] * ln[0] + R.m[4] * ln[1] + R.m[5] * ln[2];
        float nx = R.m[0] * ln[0] + R.m[1] * ln[1] + R.m[2] * ln[2];
        float nz = R.m[6] * ln[0] + R.m[7] * ln[1] + R.m[8] * ln[2];
        /* only light-facing triangles: their projection covers the silhouette */
        if (nx * light_dir[0] + ny * light_dir[1] + nz * light_dir[2] <= 0) continue;
        raster(sv[t->idx[0]], sv[t->idx[1]], sv[t->idx[2]], 0, 1);
    }
}

/* unit cube for particles */
static const float cube_v[24] = {
    -0.5f,-0.5f,-0.5f, 0.5f,-0.5f,-0.5f, 0.5f,0.5f,-0.5f, -0.5f,0.5f,-0.5f,
    -0.5f,-0.5f,0.5f,  0.5f,-0.5f,0.5f,  0.5f,0.5f,0.5f,  -0.5f,0.5f,0.5f };
static const MeshTri cube_t[12] = {
    {{0,2,1},0,{0,0,-1}}, {{0,3,2},0,{0,0,-1}},
    {{4,5,6},0,{0,0,1}},  {{4,6,7},0,{0,0,1}},
    {{0,4,7},0,{-1,0,0}}, {{0,7,3},0,{-1,0,0}},
    {{1,2,6},0,{1,0,0}},  {{1,6,5},0,{1,0,0}},
    {{3,7,6},0,{0,1,0}},  {{3,6,2},0,{0,1,0}},
    {{0,1,5},0,{0,-1,0}}, {{0,5,4},0,{0,-1,0}} };
static const Mesh cube_mesh = { cube_v, cube_t, 8, 12, {-0.5f,-0.5f,-0.5f}, {0.5f,0.5f,0.5f} };

void r_draw_box(const float c[3], const float size[3], const float rot[3], unsigned int rgb, int cast_shadow, float plane_y) {
    Xform x;
    r_xform_identity(&x);
    memcpy(x.pos, c, sizeof(x.pos));
    memcpy(x.rot, rot, sizeof(x.rot));
    memcpy(x.scale, size, sizeof(x.scale));
    if (cast_shadow) r_draw_shadow(&cube_mesh, &x, plane_y);
    r_draw_mesh(&cube_mesh, &x, rgb);
}

void r_draw_quad_flat(const float c[3], float size, float rot_y, unsigned int rgb) {
    float s[3] = { size, 0.01f, size };
    float r[3] = { 0, rot_y, 0 };
    r_draw_box(c, s, r, rgb, 0, 0);
}

/* ---------------------------------------------------------------- 2D */

void r_fill_rect(int x, int y, int w, int h, unsigned int rgb) {
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > RW) w = RW - x;
    if (y + h > RH) h = RH - y;
    for (j = 0; j < h; j++) {
        unsigned int *p = rb_color + (y + j) * RW + x;
        for (i = 0; i < w; i++) p[i] = rgb;
    }
}

void r_shade_rect(int x, int y, int w, int h, int a, unsigned int rgb) {
    int i, j;
    int cr = (rgb >> 16) & 255, cg = (rgb >> 8) & 255, cb = rgb & 255;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > RW) w = RW - x;
    if (y + h > RH) h = RH - y;
    for (j = 0; j < h; j++) {
        unsigned int *p = rb_color + (y + j) * RW + x;
        for (i = 0; i < w; i++) {
            unsigned int v = p[i];
            int r = (((v >> 16) & 255) * (256 - a) + cr * a) >> 8;
            int g = (((v >> 8) & 255) * (256 - a) + cg * a) >> 8;
            int b = ((v & 255) * (256 - a) + cb * a) >> 8;
            p[i] = (r << 16) | (g << 8) | b;
        }
    }
}

/* Original 5x7 pixel font for this port. */
static const char *font_chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ !?:.-/'+,()";
static const char font_rows[][7][6] = {
    {" ### ","#   #","#   #","#   #","#   #","#   #"," ### "}, /* 0 */
    {"  #  "," ##  ","  #  ","  #  ","  #  ","  #  "," ### "}, /* 1 */
    {" ### ","#   #","    #","  ## "," #   ","#    ","#####"}, /* 2 */
    {"#####","    #","   # ","  ## ","    #","#   #"," ### "}, /* 3 */
    {"   # ","  ## "," # # ","#  # ","#####","   # ","   # "}, /* 4 */
    {"#####","#    ","#### ","    #","    #","#   #"," ### "}, /* 5 */
    {"  ## "," #   ","#    ","#### ","#   #","#   #"," ### "}, /* 6 */
    {"#####","    #","   # ","  #  "," #   "," #   "," #   "}, /* 7 */
    {" ### ","#   #","#   #"," ### ","#   #","#   #"," ### "}, /* 8 */
    {" ### ","#   #","#   #"," ####","    #","   # "," ##  "}, /* 9 */
    {" ### ","#   #","#   #","#####","#   #","#   #","#   #"}, /* A */
    {"#### ","#   #","#   #","#### ","#   #","#   #","#### "}, /* B */
    {" ### ","#   #","#    ","#    ","#    ","#   #"," ### "}, /* C */
    {"#### ","#   #","#   #","#   #","#   #","#   #","#### "}, /* D */
    {"#####","#    ","#    ","#### ","#    ","#    ","#####"}, /* E */
    {"#####","#    ","#    ","#### ","#    ","#    ","#    "}, /* F */
    {" ### ","#   #","#    ","# ###","#   #","#   #"," ####"}, /* G */
    {"#   #","#   #","#   #","#####","#   #","#   #","#   #"}, /* H */
    {" ### ","  #  ","  #  ","  #  ","  #  ","  #  "," ### "}, /* I */
    {"  ###","   # ","   # ","   # ","   # ","#  # "," ##  "}, /* J */
    {"#   #","#  # ","# #  ","##   ","# #  ","#  # ","#   #"}, /* K */
    {"#    ","#    ","#    ","#    ","#    ","#    ","#####"}, /* L */
    {"#   #","## ##","# # #","# # #","#   #","#   #","#   #"}, /* M */
    {"#   #","#   #","##  #","# # #","#  ##","#   #","#   #"}, /* N */
    {" ### ","#   #","#   #","#   #","#   #","#   #"," ### "}, /* O */
    {"#### ","#   #","#   #","#### ","#    ","#    ","#    "}, /* P */
    {" ### ","#   #","#   #","#   #","# # #","#  # "," ## #"}, /* Q */
    {"#### ","#   #","#   #","#### ","# #  ","#  # ","#   #"}, /* R */
    {" ####","#    ","#    "," ### ","    #","    #","#### "}, /* S */
    {"#####","  #  ","  #  ","  #  ","  #  ","  #  ","  #  "}, /* T */
    {"#   #","#   #","#   #","#   #","#   #","#   #"," ### "}, /* U */
    {"#   #","#   #","#   #","#   #","#   #"," # # ","  #  "}, /* V */
    {"#   #","#   #","#   #","# # #","# # #","# # #"," # # "}, /* W */
    {"#   #","#   #"," # # ","  #  "," # # ","#   #","#   #"}, /* X */
    {"#   #","#   #"," # # ","  #  ","  #  ","  #  ","  #  "}, /* Y */
    {"#####","    #","   # ","  #  "," #   ","#    ","#####"}, /* Z */
    {"     ","     ","     ","     ","     ","     ","     "}, /* space */
    {"  #  ","  #  ","  #  ","  #  ","  #  ","     ","  #  "}, /* ! */
    {" ### ","#   #","    #","   # ","  #  ","     ","  #  "}, /* ? */
    {"     ","  #  ","  #  ","     ","  #  ","  #  ","     "}, /* : */
    {"     ","     ","     ","     ","     "," ##  "," ##  "}, /* . */
    {"     ","     ","     ","#####","     ","     ","     "}, /* - */
    {"    #","    #","   # ","  #  "," #   ","#    ","#    "}, /* / */
    {"  #  ","  #  "," #   ","     ","     ","     ","     "}, /* ' */
    {"     ","  #  ","  #  ","#####","  #  ","  #  ","     "}, /* + */
    {"     ","     ","     ","     "," ##  ","  #  "," #   "}, /* , */
    {"   # ","  #  "," #   "," #   "," #   ","  #  ","   # "}, /* ( */
    {" #   ","  #  ","   # ","   # ","   # ","  #  "," #   "}, /* ) */
};

static int glyph_index(char ch) {
    const char *p;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    for (p = font_chars; *p; p++) if (*p == ch) return (int)(p - font_chars);
    return 36; /* space */
}

int r_text_width(const char *s, int scale) {
    int n = (int)strlen(s);
    return n ? (n * 6 - 1) * scale : 0;
}

static void draw_glyphs(int x, int y, const char *s, int scale, unsigned int rgb) {
    for (; *s; s++, x += 6 * scale) {
        int g = glyph_index(*s), r, c;
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++)
                if (font_rows[g][r][c] == '#') r_fill_rect(x + c * scale, y + r * scale, scale, scale, rgb);
    }
}

void r_text(int x, int y, const char *s, int scale, unsigned int rgb, int outline) {
    if (outline) {
        int o = outline;
        draw_glyphs(x - o, y, s, scale, 0); draw_glyphs(x + o, y, s, scale, 0);
        draw_glyphs(x, y - o, s, scale, 0); draw_glyphs(x, y + o, s, scale, 0);
        draw_glyphs(x - o, y - o, s, scale, 0); draw_glyphs(x + o, y + o, s, scale, 0);
        draw_glyphs(x + o, y - o, s, scale, 0); draw_glyphs(x - o, y + o, s, scale, 0);
    }
    draw_glyphs(x, y, s, scale, rgb);
}

void r_text_centered(int y, const char *s, int scale, unsigned int rgb, int outline) {
    r_text((RW - r_text_width(s, scale)) / 2, y, s, scale, rgb, outline);
}

/* chunky extruded title lettering */
void r_text_3d(int x, int y, const char *s, int scale, unsigned int face, unsigned int side, int depth) {
    int d;
    for (d = depth; d > 0; d--) draw_glyphs(x + d, y + d, s, scale, side);
    r_text(x, y, s, scale, face, 0);
}

/* ---------------------------------------------------------------- present */

/* PS3 note: dst is RSX video memory. The PPU writes it fine but reads from
   it are extremely slow, so never copy row-to-row inside dst: expand each
   source row once into a main-memory line and write that out. */
void r_present_to(unsigned int *dst, int dw, int dh, int pitch_px) {
    static int xmap[1920];
    static unsigned int line[1920] __attribute__((aligned(128)));
    int x, y, last_sy = -1;
    if (dw > 1920) dw = 1920;
    for (x = 0; x < dw; x++) xmap[x] = (x * RW) / dw;
    for (y = 0; y < dh; y++) {
        int sy = (y * RH) / dh;
        if (sy != last_sy) {
            const unsigned int *s = rb_color + sy * RW;
            for (x = 0; x < dw; x++) line[x] = s[xmap[x]];
            last_sy = sy;
        }
        memcpy(dst + y * pitch_px, line, dw * 4);
    }
}
