/* SPDX-License-Identifier: MIT */
#ifndef RENDER_H
#define RENDER_H
#include "core.h"

enum asset_id;

void render_init(u32 *fb_a, u32 *fb_b);
void render_begin_frame(void);
void render_end_frame(void);
void render_swap(void);

u32 *render_fb(void);

void render_clear(u32 argb);
void render_fill_rect(int x, int y, int w, int h, u32 argb);
void render_blit_scaled(int asset, float x, float y, float scale, u8 alpha);
void render_blit_scaled_bg(int asset, float x, float scale);
void render_blit_tiled(int asset, float scroll_x, float scale);
void render_text(int x, int y, const char *s, u32 argb, int scale);
void render_text_center(int y, const char *s, u32 argb, int scale);
int  render_text_width(const char *s, int scale);

#endif
