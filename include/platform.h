#ifndef PLATFORM_H
#define PLATFORM_H

/* Buttons (held mask). The stick is folded into the d-pad bits. */
enum {
    PAD_UP = 1 << 0, PAD_DOWN = 1 << 1, PAD_LEFT = 1 << 2, PAD_RIGHT = 1 << 3,
    PAD_CROSS = 1 << 4, PAD_CIRCLE = 1 << 5, PAD_SQUARE = 1 << 6, PAD_TRIANGLE = 1 << 7,
    PAD_START = 1 << 8, PAD_SELECT = 1 << 9, PAD_L1 = 1 << 10, PAD_R1 = 1 << 11
};

int  plat_init(int argc, char **argv);
void plat_shutdown(void);
int  plat_should_quit(void);              /* PS button -> Quit Game */
unsigned int plat_read_pad(void);
void plat_present(void);                  /* scale rb_color to the TV, flip, vsync */
double plat_seconds(void);
void plat_rumble(int strength);           /* 0 = off, 1..255 */
/* Run fn(0) and fn(1) concurrently (second PPU hardware thread on PS3),
   returning when both are done. */
void plat_parallel(void (*fn)(int));
void plat_lock(void);                     /* guards the audio mixer */
void plat_unlock(void);
int  plat_load_text(const char *name, char *buf, int cap);
int  plat_save_text(const char *name, const char *buf);

#endif
