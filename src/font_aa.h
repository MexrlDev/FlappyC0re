/* SPDX-License-Identifier: MIT */
#ifndef FONT_AA_H
#define FONT_AA_H
#include "core.h"

void font_aa_draw(u32 *fb, int x, int y, const char *s,
                  u32 color, int target_h);


int  font_aa_width(const char *s, int target_h);

#endif
