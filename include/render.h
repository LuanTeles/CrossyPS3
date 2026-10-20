#ifndef RENDER_H
#define RENDER_H

#include "mesh.h"

/* Internal render resolution. The PS3 layer scales this up to the
   TV mode (720p = x2, 1080p = x3, 480/576 = stretched). */
#define RW 640
#define RH 360

extern unsigned int rb_color[RW * RH];

typedef struct {
    float pos[3];       /* world translation */
    float rot[3];       /* Euler XYZ (three.js order: R = Rx*Ry*Rz) */
    float scale[3];
    float pre_off[3];   /* applied before scale: v' = pre_off + pre_scale*v */
    float pre_scale;
} Xform;

void r_init(void);
void r_xform_identity(Xform *x);
void r_begin(unsigned int clear_rgb, float view_half_height, float shake_x, float shake_y);

/* 3D */
void r_draw_mesh(const Mesh *m, const Xform *x, unsigned int color_override);
void r_draw_shadow(const Mesh *m, const Xform *x, float plane_y);
void r_draw_box(const float c[3], const float size[3], const float rot[3], unsigned int rgb, int cast_shadow, float plane_y);
void r_draw_quad_flat(const float c[3], float size, float rot_y, unsigned int rgb);
int  r_mesh_on_screen(const Mesh *m, const Xform *x);
void r_shadow_pass_begin(void);   /* call after ground is drawn */

/* 2D (internal-resolution pixels) */
void r_fill_rect(int x, int y, int w, int h, unsigned int rgb);
void r_shade_rect(int x, int y, int w, int h, int alpha256, unsigned int rgb);
int  r_text_width(const char *s, int scale);
void r_text(int x, int y, const char *s, int scale, unsigned int rgb, int outline);
void r_text_centered(int cy, const char *s, int scale, unsigned int rgb, int outline);
void r_text_3d(int x, int y, const char *s, int scale, unsigned int face, unsigned int side, int depth);

/* Scale the internal buffer to a destination framebuffer (XRGB8888). */
void r_present_to(unsigned int *dst, int dw, int dh, int pitch_px);

#endif
