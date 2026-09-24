#ifndef FONT_AA_H
#define FONT_AA_H
#include "core.h"

/* Draw anti-aliased text with the baked TTF atlas.
   target_h = desired glyph height in pixels (cap height).
   The current render_text() maps scale * 8 to target_h. */
void font_aa_draw(u32 *fb, int x, int y, const char *s,
                  u32 color, int target_h);

int  font_aa_width(const char *s, int target_h);

#endif
