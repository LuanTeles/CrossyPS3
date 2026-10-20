/* Tiny software mixer: 16 voices of 48 kHz mono PCM -> stereo float. */
#include <string.h>
#include "audio.h"
#include "assets_data.h"
#include "platform.h"

#define VOICES 16

typedef struct {
    const short *data;
    int len, pos;
    float vol;
} Voice;

static Voice voices[VOICES];
static volatile int muted;

void audio_mix_init(void) { memset(voices, 0, sizeof(voices)); }
void audio_set_muted(int m) { muted = m; }
int audio_muted(void) { return muted; }

void audio_play(int snd, float volume) {
    int i, best = 0, best_left = 0x7fffffff;
    if (snd < 0 || snd >= SND_COUNT || muted) return;
    plat_lock();
    for (i = 0; i < VOICES; i++) {
        int left = voices[i].data ? voices[i].len - voices[i].pos : 0;
        if (left < best_left) { best_left = left; best = i; }
    }
    voices[best].data = sound_table[snd].data;
    voices[best].len = sound_table[snd].len;
    voices[best].pos = 0;
    voices[best].vol = volume;
    plat_unlock();
}

void audio_mix(float *out, int frames) {
    int i, f;
    memset(out, 0, sizeof(float) * 2 * frames);
    if (muted) return;
    plat_lock();
    for (i = 0; i < VOICES; i++) {
        Voice *v = &voices[i];
        float g;
        int n;
        if (!v->data) continue;
        g = v->vol * (0.8f / 32768.0f);
        n = v->len - v->pos;
        if (n > frames) n = frames;
        for (f = 0; f < n; f++) {
            float s = v->data[v->pos + f] * g;
            out[f * 2] += s;
            out[f * 2 + 1] += s;
        }
        v->pos += n;
        if (v->pos >= v->len) v->data = 0;
    }
    plat_unlock();
    for (f = 0; f < frames * 2; f++) {
        if (out[f] > 1.0f) out[f] = 1.0f;
        else if (out[f] < -1.0f) out[f] = -1.0f;
    }
}
