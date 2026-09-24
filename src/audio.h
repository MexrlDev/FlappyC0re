/* SPDX-License-Identifier: MIT */
#ifndef AUDIO_H
#define AUDIO_H
#include "core.h"
#include "assets.h"

int  audio_init(s32 handle, void *aud_out_fn, void *G);
void audio_shutdown(void);
void audio_play(enum asset_id id, float vol);
void audio_mix_tick(void);

#endif
