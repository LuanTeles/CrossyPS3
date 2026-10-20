/* Software renderer: flat-shaded triangles, z-buffer, planar ground shadows.
 *
 * Why software and not RSX shaders: the original game is a few thousand
 * flat-coloured triangles, which the Cell PPU handles fine at 640x360.
 * Rendering into a plain buffer keeps the PS3-specific code tiny (just a
 * framebuffer flip) and means the exact same code runs in the desktop test
 * harness, so what we test here is what runs on the console.
 *
 * See the "3D core" comment below for the Cell-specific speed choices.
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

static void init_shade_tab(void);

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
    init_shade_tab();
}

void r_xform_identity(Xform *x) {
    memset(x, 0, sizeof(*x));
    x->scale[0] = x->scale[1] = x->scale[2] = 1;
    x->pre_scale = 1;
}

static unsigned int clear_color;
static const int band_y0[R_CONTEXTS] = { 0, RH / 2 };
static const int band_y1[R_CONTEXTS] = { RH / 2, RH };

void r_begin(unsigned int clear_rgb, float view_half_height, float shake_x, float shake_y) {
    clear_color = clear_rgb;
    pix_per_unit = (RH * 0.5f) / view_half_height;
    scr_cx = RW * 0.5f + shake_x;
    scr_cy = RH * 0.5f + shake_y;
}

void r_clear_band(int ctx) {
    int i, a = band_y0[ctx] * RW, b = band_y1[ctx] * RW;
    for (i = a; i < b; i++) { rb_color[i] = clear_color; rb_depth[i] = INT_MAX; }
}

void r_shadow_pass_begin(int ctx) {
    memset(rb_stencil + band_y0[ctx] * RW, 0, (band_y1[ctx] - band_y0[ctx]) * RW);
}

/* ---------------------------------------------------------------- */
/* 3D core.
 *
 * Cell PPU notes that drive this design:
 *  - float<->int conversions go through memory (load-hit-store, ~40+ cycles),
 *    so vertices are converted to 16.16 fixed point once, and triangle edges
 *    and depth are stepped with 64-bit integers (the PPU is 64-bit, so int64
 *    math is as cheap as int32). The old code converted 3x per scanline.
 *  - no per-triangle sqrt/divide for normals: lighting and back-face tests
 *    use the light/view vectors transformed into model space once per object.
 *  - triangles fully off screen are rejected before any setup (the 25-unit
 *    ground strips are mostly off screen).
 */

typedef struct { float m[9]; } M3;

typedef struct {
    float fx, fy, fz;   /* screen-space float (for depth gradients) */
    int X, Y;           /* 16.16 fixed screen position */
} SV;

/* lighting table: [level][srgb channel] -> lit srgb channel */
#define SHADE_LEVELS 64
static unsigned char shade_tab[SHADE_LEVELS][256];

static void init_shade_tab(void) {
    int l, c;
    for (l = 0; l < SHADE_LEVELS; l++) {
        float d = (float)l / (SHADE_LEVELS - 1);           /* max(0, n.L) */
        float k = (1.8f + d) * 0.318309886f;                /* (ambient + direct) / pi */
        for (c = 0; c < 256; c++) {
            int v = (int)(lin_of_srgb[c] * k * 4095.0f);
            if (v > 4095) v = 4095;
            shade_tab[l][c] = srgb_of_lin[v];
        }
    }
}

static void build_rot(const float r[3], M3 *o) {
    /* R = Rx * Ry * Rz (three.js 'XYZ') */
    float cx = cosf(r[0]), sx = sinf(r[0]);
    float cy = cosf(r[1]), sy = sinf(r[1]);
    float cz = cosf(r[2]), sz = sinf(r[2]);
    o->m[0] = cy * cz;                  o->m[1] = -cy * sz;                 o->m[2] = sy;
    o->m[3] = cx * sz + sx * sy * cz;   o->m[4] = cx * cz - sx * sy * sz;   o->m[5] = -sx * cy;
    o->m[6] = sx * sz - cx * sy * cz;   o->m[7] = sx * cz + cx * sy * sz;   o->m[8] = cx * cy;
}

/* world = A*v + b for model-space v */
typedef struct { float A[9]; float b[3]; M3 R; } WorldXf;

static void world_xf(const Xform *x, WorldXf *w) {
    int i, j;
    float s[3];
    build_rot(x->rot, &w->R);
    for (j = 0; j < 3; j++) s[j] = x->scale[j];
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) w->A[i * 3 + j] = w->R.m[i * 3 + j] * s[j] * x->pre_scale;
        w->b[i] = x->pos[i];
        for (j = 0; j < 3; j++) w->b[i] += w->R.m[i * 3 + j] * s[j] * x->pre_off[j];
    }
}

/* screen = P*A*v + t, P = rows (ppu*camR, -ppu*camU, camF) */
typedef struct { float M[9]; float t[3]; } ScreenXf;

static void screen_xf(const WorldXf *w, ScreenXf *o) {
    float P[9], d[3];
    int i, j, k;
    for (j = 0; j < 3; j++) {
        P[j] = cam_r[j] * pix_per_unit;
        P[3 + j] = -cam_u[j] * pix_per_unit;
        P[6 + j] = cam_f[j];
    }
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            float a = 0;
            for (k = 0; k < 3; k++) a += P[i * 3 + k] * w->A[k * 3 + j];
            o->M[i * 3 + j] = a;
        }
    for (k = 0; k < 3; k++) d[k] = w->b[k] - cam_pos[k];
    for (i = 0; i < 3; i++) o->t[i] = P[i * 3] * d[0] + P[i * 3 + 1] * d[1] + P[i * 3 + 2] * d[2];
    o->t[0] += scr_cx;
    o->t[1] += scr_cy;
}

static inline void sv_set(SV *s, float x, float y, float z) {
    if (x > 16000.f) x = 16000.f; else if (x < -16000.f) x = -16000.f;
    if (y > 16000.f) y = 16000.f; else if (y < -16000.f) y = -16000.f;
    s->fx = x; s->fy = y; s->fz = z;
    s->X = (int)(x * 65536.0f);
    s->Y = (int)(y * 65536.0f);
}

/* mode 0: opaque (depth test+write). mode 1: shadow (depth test with bias,
   stencil so overlapping shadows don't double-darken, no depth write). */
static void raster(int ctx, const SV *a, const SV *b, const SV *c, unsigned int col, int mode) {
    const SV *v0 = a, *v1 = b, *v2 = c, *t;
    float area, dzdx, dzdy;
    long long dzx, dzy, zrow, s02, s01, s12, xl, xr;
    int ys, ym, ye, y, minx, maxx;

    if (v1->Y < v0->Y) { t = v0; v0 = v1; v1 = t; }
    if (v2->Y < v0->Y) { t = v0; v0 = v2; v2 = t; }
    if (v2->Y < v1->Y) { t = v1; v1 = v2; v2 = t; }
    if (v2->Y <= v0->Y) return;

    /* off-screen reject */
    minx = v0->X; maxx = v0->X;
    if (v1->X < minx) { minx = v1->X; }
    if (v1->X > maxx) { maxx = v1->X; }
    if (v2->X < minx) { minx = v2->X; }
    if (v2->X > maxx) { maxx = v2->X; }
    if (maxx < 0 || minx >= (RW << 16) || v2->Y < (band_y0[ctx] << 16) || v0->Y >= (band_y1[ctx] << 16)) return;

    /* scanline y covers centre y+0.5: first = ceil(Y - 0.5) */
    ys = (v0->Y + 0x7FFF) >> 16;
    ym = (v1->Y + 0x7FFF) >> 16;
    ye = (v2->Y + 0x7FFF) >> 16;
    if (ys < band_y0[ctx]) ys = band_y0[ctx];
    if (ym < ys) ym = ys;
    if (ye > band_y1[ctx]) ye = band_y1[ctx];
    if (ym > ye) ym = ye;
    if (ys >= ye) return;

    area = (v1->fx - v0->fx) * (v2->fy - v0->fy) - (v2->fx - v0->fx) * (v1->fy - v0->fy);
    if (area > -1e-6f && area < 1e-6f) return;
    dzdx = ((v1->fz - v0->fz) * (v2->fy - v0->fy) - (v2->fz - v0->fz) * (v1->fy - v0->fy)) / area;
    dzdy = ((v2->fz - v0->fz) * (v1->fx - v0->fx) - (v1->fz - v0->fz) * (v2->fx - v0->fx)) / area;
    if (dzdx > 30000.f) dzdx = 30000.f; else if (dzdx < -30000.f) dzdx = -30000.f;
    if (dzdy > 30000.f) dzdy = 30000.f; else if (dzdy < -30000.f) dzdy = -30000.f;
    dzx = (long long)(dzdx * ZSCALE);
    dzy = (long long)(dzdy * ZSCALE);
    /* depth at pixel centre (0.5, ys+0.5), in ZSCALE units */
    zrow = (long long)(v0->fz * ZSCALE)
         + (((long long)(0x8000 - v0->X) * dzx) >> 16)
         + (((((long long)ys << 16) + 0x8000 - v0->Y) * dzy) >> 16);

    /* long edge 0->2 */
    s02 = ((long long)(v2->X - v0->X) * 65536) / (v2->Y - v0->Y);
    xl = v0->X + (((((long long)ys << 16) + 0x8000 - v0->Y) * s02) >> 16);

    {
        int bias = (mode == 1) ? (int)(0.03f * ZSCALE) : 0;
        int seg;
        for (seg = 0; seg < 2; seg++) {
            int y0 = seg == 0 ? ys : ym, y1 = seg == 0 ? ym : ye;
            const SV *ea = seg == 0 ? v0 : v1, *eb = seg == 0 ? v1 : v2;
            long long s;
            if (y0 >= y1) continue;
            if (eb->Y - ea->Y <= 0) continue;
            s = ((long long)(eb->X - ea->X) * 65536) / (eb->Y - ea->Y);
            xr = ea->X + (((((long long)y0 << 16) + 0x8000 - ea->Y) * s) >> 16);
            if (seg == 0) s01 = s; else s12 = s;
            for (y = y0; y < y1; y++) {
                long long a0 = xl, a1 = xr;
                int xs, xe, x, z, row;
                if (a0 > a1) { long long tmp = a0; a0 = a1; a1 = tmp; }
                xs = (int)((a0 + 0x7FFF) >> 16);
                xe = (int)((a1 + 0x7FFF) >> 16);
                if (xs < 0) xs = 0;
                if (xe > RW) xe = RW;
                if (xs < xe) {
                    long long zz = zrow + (long long)(y - ys) * dzy + dzx * xs;
                    int dz = (int)dzx;
                    if (zz > 0x7FFF0000LL) zz = 0x7FFF0000LL; else if (zz < -0x7FFF0000LL) zz = -0x7FFF0000LL;
                    z = (int)zz;
                    row = y * RW;
                    if (mode == 0) {
                        int *zb = rb_depth + row;
                        unsigned int *cb = rb_color + row;
                        for (x = xs; x < xe; x++, z += dz)
                            if (z < zb[x]) { zb[x] = z; cb[x] = col; }
                    } else {
                        int *zb = rb_depth + row;
                        unsigned int *cb = rb_color + row;
                        unsigned char *sb = rb_stencil + row;
                        for (x = xs; x < xe; x++, z += dz) {
                            if (!sb[x] && zb[x] != INT_MAX && z - bias <= zb[x]) {
                                unsigned int p = cb[x];
                                sb[x] = 1;
                                cb[x] = ((unsigned int)shadow_lut[(p >> 16) & 255] << 16) |
                                        ((unsigned int)shadow_lut[(p >> 8) & 255] << 8) | shadow_lut[p & 255];
                            }
                        }
                    }
                }
                xl += s02;
                xr += s;
            }
        }
        (void)s01; (void)s12;
    }
}

/* ---------------------------------------------------------------- */

#define MAXV 1024
static SV sv_buf[R_CONTEXTS][MAXV];

static int bounds_on_screen(const Mesh *m, const ScreenXf *s) {
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    int i;
    for (i = 0; i < 8; i++) {
        float v0 = (i & 1) ? m->max[0] : m->min[0];
        float v1 = (i & 2) ? m->max[1] : m->min[1];
        float v2 = (i & 4) ? m->max[2] : m->min[2];
        float x = s->M[0] * v0 + s->M[1] * v1 + s->M[2] * v2 + s->t[0];
        float y = s->M[3] * v0 + s->M[4] * v1 + s->M[5] * v2 + s->t[1];
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    return !(maxx < 0 || minx > RW || maxy < 0 || miny > RH);
}

int r_mesh_on_screen(const Mesh *m, const Xform *x) {
    WorldXf w;
    ScreenXf s;
    world_xf(x, &w);
    screen_xf(&w, &s);
    return bounds_on_screen(m, &s);
}

void r_draw_mesh(int ctx, const Mesh *m, const Xform *x, unsigned int color_override) {
    SV *sv = sv_buf[ctx];
    WorldXf w;
    ScreenXf s;
    float fm[3], lm[3], inv[3];
    int i, j;
    if (m->nverts > MAXV) return;
    world_xf(x, &w);
    screen_xf(&w, &s);
    if (!bounds_on_screen(m, &s)) return;

    /* view and light vectors in model space (R^T v). Back faces: the world
       normal is R*S^-1*n, so its dot with F equals n . S^-1 R^T F. */
    for (j = 0; j < 3; j++) {
        float f = 0, l = 0;
        for (i = 0; i < 3; i++) { f += w.R.m[i * 3 + j] * cam_f[i]; l += w.R.m[i * 3 + j] * light_dir[i]; }
        inv[j] = 1.0f / (x->scale[j] != 0 ? x->scale[j] : 1e-6f);
        fm[j] = f * inv[j];
        lm[j] = l;
    }
    {
        /* light with the inverse-scaled normal, renormalised once per object
           for the common uniform case; good enough for squash tweens */
        float ll;
        lm[0] *= inv[0]; lm[1] *= inv[1]; lm[2] *= inv[2];
        ll = sqrtf(lm[0] * lm[0] + lm[1] * lm[1] + lm[2] * lm[2]);
        if (ll > 0) { ll = 1.0f / ll; lm[0] *= ll; lm[1] *= ll; lm[2] *= ll; }
    }

    for (i = 0; i < m->nverts; i++) {
        const float *v = m->verts + i * 3;
        sv_set(&sv[i],
               s.M[0] * v[0] + s.M[1] * v[1] + s.M[2] * v[2] + s.t[0],
               s.M[3] * v[0] + s.M[4] * v[1] + s.M[5] * v[2] + s.t[1],
               s.M[6] * v[0] + s.M[7] * v[1] + s.M[8] * v[2] + s.t[2]);
    }
    for (i = 0; i < m->ntris; i++) {
        const MeshTri *t = &m->tris[i];
        const float *n = t->n;
        float d;
        int lvl;
        unsigned int c, col;
        if (n[0] * fm[0] + n[1] * fm[1] + n[2] * fm[2] >= 0) continue;   /* back face */
        d = n[0] * lm[0] + n[1] * lm[1] + n[2] * lm[2];
        if (d < 0) d = 0; else if (d > 1) d = 1;
        lvl = (int)(d * (SHADE_LEVELS - 1) + 0.5f);
        c = color_override ? color_override : t->color;
        col = ((unsigned int)shade_tab[lvl][(c >> 16) & 255] << 16) |
              ((unsigned int)shade_tab[lvl][(c >> 8) & 255] << 8) | shade_tab[lvl][c & 255];
        raster(ctx, &sv[t->idx[0]], &sv[t->idx[1]], &sv[t->idx[2]], col, 0);
    }
}

void r_draw_shadow(int ctx, const Mesh *m, const Xform *x, float plane_y) {
    SV *sv = sv_buf[ctx];
    WorldXf w;
    float lm[3];
    float P[9], cx, cy;
    int i, j;
    if (m->nverts > MAXV) return;
    world_xf(x, &w);
    for (j = 0; j < 3; j++) {
        float l = 0;
        for (i = 0; i < 3; i++) l += w.R.m[i * 3 + j] * light_dir[i];
        lm[j] = l;
    }
    for (j = 0; j < 3; j++) {
        P[j] = cam_r[j] * pix_per_unit;
        P[3 + j] = -cam_u[j] * pix_per_unit;
        P[6 + j] = cam_f[j];
    }
    cx = scr_cx; cy = scr_cy;
    for (i = 0; i < m->nverts; i++) {
        const float *v = m->verts + i * 3;
        float wx = w.A[0] * v[0] + w.A[1] * v[1] + w.A[2] * v[2] + w.b[0];
        float wy = w.A[3] * v[0] + w.A[4] * v[1] + w.A[5] * v[2] + w.b[1];
        float wz = w.A[6] * v[0] + w.A[7] * v[1] + w.A[8] * v[2] + w.b[2];
        /* project along the light onto y = plane_y */
        float k = (wy - plane_y) / light_dir[1];
        float d0, d1, d2;
        if (k < 0) k = 0;
        wx -= light_dir[0] * k;
        wz -= light_dir[2] * k;
        wy = plane_y + 0.002f;
        d0 = wx - cam_pos[0]; d1 = wy - cam_pos[1]; d2 = wz - cam_pos[2];
        sv_set(&sv[i],
               cx + P[0] * d0 + P[1] * d1 + P[2] * d2,
               cy + P[3] * d0 + P[4] * d1 + P[5] * d2,
               P[6] * d0 + P[7] * d1 + P[8] * d2);
    }
    for (i = 0; i < m->ntris; i++) {
        const MeshTri *t = &m->tris[i];
        const float *n = t->n;
        /* only light-facing triangles: their projection covers the silhouette */
        if (n[0] * lm[0] + n[1] * lm[1] + n[2] * lm[2] <= 0) continue;
        raster(ctx, &sv[t->idx[0]], &sv[t->idx[1]], &sv[t->idx[2]], 0, 1);
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

void r_draw_box(int ctx, const float c[3], const float size[3], const float rot[3], unsigned int rgb, int cast_shadow, float plane_y) {
    Xform x;
    r_xform_identity(&x);
    memcpy(x.pos, c, sizeof(x.pos));
    memcpy(x.rot, rot, sizeof(x.rot));
    memcpy(x.scale, size, sizeof(x.scale));
    if (cast_shadow) r_draw_shadow(ctx, &cube_mesh, &x, plane_y);
    r_draw_mesh(ctx, &cube_mesh, &x, rgb);
}

void r_draw_quad_flat(int ctx, const float c[3], float size, float rot_y, unsigned int rgb) {
    float s[3] = { size, 0.01f, size };
    float r[3] = { 0, rot_y, 0 };
    r_draw_box(ctx, c, s, r, rgb, 0, 0);
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
