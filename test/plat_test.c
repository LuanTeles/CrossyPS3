/* Headless desktop platform used to test the port without a PS3.
 *
 *   bouncy_test <out_dir> <frames> "<script>" "<shot frames>"
 *
 * script: space-separated "frame:+BTN" / "frame:-BTN" events, BTN one of
 *         U D L R X O START SELECT. Time advances exactly 1/60 s per frame.
 * shots:  comma-separated frame numbers to dump as PPM.
 * Also writes audio.wav (the mixer output) and prints timing stats.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "platform.h"
#include "render.h"
#include "audio.h"

static const char *out_dir = ".";
static int frame, max_frames = 600;
static unsigned int held;
static char script[8192], shots[1024];
static FILE *wav;
static long wav_samples;
static double render_time;
static unsigned int fb[1280 * 720];

static unsigned int btn(const char *s) {
    if (!strcmp(s, "U")) return PAD_UP;
    if (!strcmp(s, "D")) return PAD_DOWN;
    if (!strcmp(s, "L")) return PAD_LEFT;
    if (!strcmp(s, "R")) return PAD_RIGHT;
    if (!strcmp(s, "X")) return PAD_CROSS;
    if (!strcmp(s, "O")) return PAD_CIRCLE;
    if (!strcmp(s, "START")) return PAD_START;
    if (!strcmp(s, "SELECT")) return PAD_SELECT;
    return 0;
}

int plat_init(int argc, char **argv) {
    char path[512];
    if (argc > 1) out_dir = argv[1];
    if (argc > 2) max_frames = atoi(argv[2]);
    if (argc > 3) snprintf(script, sizeof(script), "%s", argv[3]);
    if (argc > 4) snprintf(shots, sizeof(shots), "%s", argv[4]);
    snprintf(path, sizeof(path), "%s/audio.wav", out_dir);
    wav = fopen(path, "wb");
    if (wav) fseek(wav, 44, SEEK_SET);
    return 0;
}

int plat_should_quit(void) { return frame >= max_frames; }

unsigned int plat_read_pad(void) {
    char buf[8192], *tok, *save = 0;
    snprintf(buf, sizeof(buf), "%s", script);
    for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(0, " ", &save)) {
        int f = atoi(tok);
        char *c = strchr(tok, ':');
        if (!c || f != frame) continue;
        if (c[1] == '+') held |= btn(c + 2);
        else if (c[1] == '-') held &= ~btn(c + 2);
    }
    return held;
}

static int is_shot(int f) {
    char buf[1024], *tok, *save = 0;
    snprintf(buf, sizeof(buf), "%s", shots);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(0, ",", &save))
        if (atoi(tok) == f) return 1;
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void plat_present(void) {
    float mixbuf[800 * 2];
    short pcm[800 * 2];
    int i;
    double t0 = now_s();
    /* exercise the same 720p upscale path the PS3 uses */
    r_present_to(fb, 1280, 720, 1280);
    render_time += now_s() - t0;
    if (is_shot(frame)) {
        char path[512];
        FILE *f;
        snprintf(path, sizeof(path), "%s/frame_%05d.ppm", out_dir, frame);
        f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n1280 720\n255\n");
            for (i = 0; i < 1280 * 720; i++) {
                unsigned char px[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] };
                fwrite(px, 1, 3, f);
            }
            fclose(f);
        }
    }
    audio_mix(mixbuf, 800); /* 48000 / 60 */
    for (i = 0; i < 1600; i++) pcm[i] = (short)(mixbuf[i] * 32767);
    if (wav) { fwrite(pcm, 2, 1600, wav); wav_samples += 800; }
    frame++;
}

double plat_seconds(void) { return frame / 60.0; }
void plat_rumble(int s) { (void)s; }
void plat_parallel(void (*fn)(int)) { fn(0); fn(1); }
void plat_lock(void) {}
void plat_unlock(void) {}

int plat_load_text(const char *name, char *buf, int cap) {
    char path[512]; FILE *f; int n;
    snprintf(path, sizeof(path), "%s/%s", out_dir, name);
    f = fopen(path, "rb");
    if (!f) return -1;
    n = (int)fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

int plat_save_text(const char *name, const char *buf) {
    char path[512]; FILE *f;
    snprintf(path, sizeof(path), "%s/%s", out_dir, name);
    f = fopen(path, "wb");
    if (!f) return -1;
    fputs(buf, f);
    fclose(f);
    return 0;
}

static void put32(FILE *f, unsigned int v) { unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void put16(FILE *f, unsigned int v) { unsigned char b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

void plat_shutdown(void) {
    if (wav) {
        unsigned int data = (unsigned int)(wav_samples * 4);
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); put32(wav, 36 + data); fwrite("WAVEfmt ", 1, 8, wav);
        put32(wav, 16); put16(wav, 1); put16(wav, 2); put32(wav, AUDIO_RATE); put32(wav, AUDIO_RATE * 4);
        put16(wav, 4); put16(wav, 16); fwrite("data", 1, 4, wav); put32(wav, data);
        fclose(wav);
    }
    printf("frames=%d upscale_ms_avg=%.3f\n", frame, frame ? render_time * 1000 / frame : 0);
}
