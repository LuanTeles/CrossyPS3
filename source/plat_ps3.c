/* PS3 platform layer (PSL1GHT).
 *
 * Common first-port problems this file deals with:
 *  - PS button -> "Quit Game" hangs or crashes the console unless the game
 *    registers a sysutil callback, polls sysUtilCheckCallback() every frame
 *    and exits cleanly (RSX finished, audio port closed) on SYSUTIL_EXIT_GAME.
 *  - ioPadGetData() returns len == 0 when nothing changed; treating that as
 *    "all buttons released" makes inputs stutter/drop. Keep the last state.
 *  - Controllers can be plugged into any port; read them all.
 *  - Sticks rest a little off-centre; use a deadzone with hysteresis.
 *  - Drawing into the buffer that is still on screen tears/flickers; wait
 *    for the previous flip before reusing a buffer.
 *  - The audio port runs at a fixed 48 kHz float stereo; an open port left
 *    behind on exit leaves the next app silent, so always close it.
 *  - Save files must go under the game's own USRDIR (derived from argv[0]).
 */
#include <ppu-lv2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <io/pad.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <lv2/mutex.h>
#include <lv2/systime.h>

#include "platform.h"
#include "render.h"
#include "audio.h"

#define CB_SIZE   0x100000
#define HOST_SIZE (32 * 1024 * 1024)
#define GCM_LABEL_INDEX 255

static gcmContextData *context;
static void *host_addr;
static u32 *fb[2];
static u32 fb_offset[2];
static int cur_fb;
static u16 scr_w, scr_h;
static volatile int quit_requested;
static s64 time_base;

static char save_dir[256] = "/dev_hdd0/game/BBCN00001/USRDIR";

/* ------------------------------------------------------------ sysutil */
static void sysutil_cb(u64 status, u64 param, void *usr) {
    (void)param; (void)usr;
    if (status == SYSUTIL_EXIT_GAME) quit_requested = 1;
}

int plat_should_quit(void) {
    sysUtilCheckCallback();
    return quit_requested;
}

/* ------------------------------------------------------------ video */
static u32 label_val = 1;
static void wait_rsx_idle(void) {
    rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, label_val);
    rsxSetWaitLabel(context, GCM_LABEL_INDEX, label_val);
    label_val++;
    rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, label_val);
    rsxFlushBuffer(context);
    while (*(vu32 *)gcmGetLabelAddress(GCM_LABEL_INDEX) != label_val) usleep(30);
    label_val++;
}

static void wait_flip(void) {
    while (gcmGetFlipStatus() != 0) usleep(200);
    gcmResetFlipStatus();
}

static void do_flip(int id) {
    if (gcmSetFlip(context, (u8)id) == 0) {
        rsxFlushBuffer(context);
        gcmSetWaitFlip(context);
    }
}

static int video_init(void) {
    videoState state;
    videoConfiguration vconfig;
    videoResolution res;
    int i;

    host_addr = memalign(1024 * 1024, HOST_SIZE);
    if (!host_addr) return -1;
    rsxInit(&context, CB_SIZE, HOST_SIZE, host_addr);
    if (!context) return -1;

    if (videoGetState(0, 0, &state) != 0 || state.state != 0) return -1;
    if (videoGetResolution(state.displayMode.resolution, &res) != 0) return -1;

    memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution = state.displayMode.resolution;
    vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch = res.width * sizeof(u32);
    vconfig.aspect = state.displayMode.aspect;

    wait_rsx_idle();
    if (videoConfigure(0, &vconfig, NULL, 0) != 0) return -1;
    if (videoGetState(0, 0, &state) != 0) return -1;

    gcmSetFlipMode(GCM_FLIP_VSYNC);
    scr_w = res.width;
    scr_h = res.height;

    for (i = 0; i < 2; i++) {
        fb[i] = (u32 *)rsxMemalign(64, scr_w * scr_h * 4);
        if (!fb[i]) return -1;
        if (rsxAddressToOffset(fb[i], &fb_offset[i]) != 0) return -1;
        if (gcmSetDisplayBuffer(i, fb_offset[i], scr_w * 4, scr_w, scr_h) != 0) return -1;
        memset(fb[i], 0, scr_w * scr_h * 4);
    }
    gcmResetFlipStatus();
    do_flip(1);
    cur_fb = 0;
    return 0;
}

void plat_present(void) {
    wait_flip();                                   /* previous flip done -> fb[cur_fb] is free */
    r_present_to(fb[cur_fb], scr_w, scr_h, scr_w);
    do_flip(cur_fb);
    cur_fb ^= 1;
}

/* ------------------------------------------------------------ pads */
static padData last_pad[MAX_PADS];
static int stick_x_state[MAX_PADS], stick_y_state[MAX_PADS];
static int rumble_port_state[MAX_PADS];

static int stick_axis(int v, int *state) {
    /* 0..255, centre 128. Engage beyond 80 from centre, release under 40. */
    int d = v - 128;
    if (*state == 0) {
        if (d < -80) *state = -1;
        else if (d > 80) *state = 1;
    } else if (d > -40 && d < 40) {
        *state = 0;
    }
    return *state;
}

unsigned int plat_read_pad(void) {
    padInfo info;
    unsigned int m = 0;
    int i;
    if (ioPadGetInfo(&info) != 0) return 0;
    for (i = 0; i < MAX_PADS; i++) {
        padData pd;
        padData *p = &last_pad[i];
        if (!info.status[i]) { memset(p, 0, sizeof(*p)); continue; }
        memset(&pd, 0, sizeof(pd));
        if (ioPadGetData(i, &pd) == 0 && pd.len > 0) *p = pd;   /* len 0 = no change */
        if (p->len <= 0) continue;
        if (p->BTN_UP) m |= PAD_UP;
        if (p->BTN_DOWN) m |= PAD_DOWN;
        if (p->BTN_LEFT) m |= PAD_LEFT;
        if (p->BTN_RIGHT) m |= PAD_RIGHT;
        if (p->BTN_CROSS) m |= PAD_CROSS;
        if (p->BTN_CIRCLE) m |= PAD_CIRCLE;
        if (p->BTN_SQUARE) m |= PAD_SQUARE;
        if (p->BTN_TRIANGLE) m |= PAD_TRIANGLE;
        if (p->BTN_START) m |= PAD_START;
        if (p->BTN_SELECT) m |= PAD_SELECT;
        if (p->BTN_L1) m |= PAD_L1;
        if (p->BTN_R1) m |= PAD_R1;
        {
            int sx = stick_axis(p->ANA_L_H, &stick_x_state[i]);
            int sy = stick_axis(p->ANA_L_V, &stick_y_state[i]);
            if (sx < 0) m |= PAD_LEFT;
            if (sx > 0) m |= PAD_RIGHT;
            if (sy < 0) m |= PAD_UP;
            if (sy > 0) m |= PAD_DOWN;
        }
    }
    return m;
}

void plat_rumble(int strength) {
    padInfo info;
    int i;
    if (ioPadGetInfo(&info) != 0) return;
    for (i = 0; i < MAX_PADS; i++) {
        padActParam act;
        if (!info.status[i]) { rumble_port_state[i] = 0; continue; }
        if (rumble_port_state[i] == strength) continue;  /* only send changes */
        memset(&act, 0, sizeof(act));
        act.small_motor = strength > 0 ? 1 : 0;
        act.large_motor = (u8)(strength > 255 ? 255 : strength);
        ioPadSetActDirect(i, &act);
        rumble_port_state[i] = strength;
    }
}

/* ------------------------------------------------------------ audio */
static u32 audio_port;
static audioPortConfig audio_cfg;
static sys_event_queue_t audio_queue;
static sys_ipc_key_t audio_key;
static sys_ppu_thread_t audio_thread_id;
static volatile int audio_running;
static int audio_ok;
static sys_lwmutex_t mix_mutex;

void plat_lock(void) { sysLwMutexLock(&mix_mutex, 0); }
void plat_unlock(void) { sysLwMutexUnlock(&mix_mutex); }

static void audio_thread(void *arg) {
    (void)arg;
    while (audio_running) {
        sys_event_t ev;
        u64 block;
        f32 *start, *buf;
        if (sysEventQueueReceive(audio_queue, &ev, 20 * 1000) != 0) continue;
        block = *(u64 *)(u64)audio_cfg.readIndex;
        start = (f32 *)(u64)audio_cfg.audioDataStart;
        buf = start + audio_cfg.channelCount * AUDIO_BLOCK_SAMPLES * ((block + 1) % audio_cfg.numBlocks);
        audio_mix(buf, AUDIO_BLOCK_SAMPLES);
    }
    sysThreadExit(0);
}

static void audio_init(void) {
    audioPortParam params;
    if (audioInit() != 0) return;
    memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_8;
    params.attrib = 0;
    params.level = 1.0f;
    if (audioPortOpen(&params, &audio_port) != 0) { audioQuit(); return; }
    audioGetPortConfig(audio_port, &audio_cfg);
    if (audioCreateNotifyEventQueue(&audio_queue, &audio_key) != 0) { audioPortClose(audio_port); audioQuit(); return; }
    audioSetNotifyEventQueue(audio_key);
    sysEventQueueDrain(audio_queue);
    audioPortStart(audio_port);
    audio_running = 1;
    sysThreadCreate(&audio_thread_id, audio_thread, NULL, 900, 0x8000, THREAD_JOINABLE, "audio");
    audio_ok = 1;
}

static void audio_shutdown(void) {
    u64 ret;
    if (!audio_ok) return;
    audio_running = 0;
    sysThreadJoin(audio_thread_id, &ret);
    audioPortStop(audio_port);
    audioRemoveNotifyEventQueue(audio_key);
    audioPortClose(audio_port);
    sysEventQueueDestroy(audio_queue, 0);
    audioQuit();
    audio_ok = 0;
}

/* ------------------------------------------------------------ files */
int plat_load_text(const char *name, char *buf, int cap) {
    char path[320];
    FILE *f;
    int n;
    snprintf(path, sizeof(path), "%s/%s", save_dir, name);
    f = fopen(path, "rb");
    if (!f) return -1;
    n = (int)fread(buf, 1, cap - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

int plat_save_text(const char *name, const char *buf) {
    char path[320];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", save_dir, name);
    f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(buf, 1, strlen(buf), f);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------ init */
double plat_seconds(void) { return (double)(sysGetSystemTime() - time_base) / 1000000.0; }

int plat_init(int argc, char **argv) {
    sys_lwmutex_attr_t mattr;
    time_base = sysGetSystemTime();

    /* argv[0] = /dev_hdd0/game/<TITLEID>/USRDIR/EBOOT.BIN */
    if (argc > 0 && argv && argv[0] && strncmp(argv[0], "/dev_", 5) == 0) {
        char *slash;
        snprintf(save_dir, sizeof(save_dir), "%s", argv[0]);
        slash = strrchr(save_dir, '/');
        if (slash) *slash = 0;
    }

    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);

    memset(&mattr, 0, sizeof(mattr));
    mattr.attr_protocol = SYS_LWMUTEX_PROTOCOL_PRIO;
    mattr.attr_recursive = SYS_LWMUTEX_ATTR_NOT_RECURSIVE;
    strcpy(mattr.name, "mix");
    sysLwMutexCreate(&mix_mutex, &mattr);

    if (video_init() != 0) return -1;
    ioPadInit(7);
    audio_init();
    return 0;
}

void plat_shutdown(void) {
    int i;
    plat_rumble(0);
    audio_shutdown();
    gcmSetWaitFlip(context);
    for (i = 0; i < 2; i++) if (fb[i]) rsxFree(fb[i]);
    rsxFinish(context, 1);
    if (host_addr) free(host_addr);
    ioPadEnd();
    sysLwMutexDestroy(&mix_mutex);
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
}
