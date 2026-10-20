#ifndef AUDIO_H
#define AUDIO_H

#define AUDIO_RATE 48000

void audio_mix_init(void);
void audio_play(int snd, float volume);
void audio_set_muted(int muted);
int  audio_muted(void);
/* Fill `frames` interleaved stereo float samples (called from the audio thread). */
void audio_mix(float *out, int frames);

#endif
