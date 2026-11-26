#ifndef _PRINT_H
#define _PRINT_H

#include "types.h"

#define TEXRECT_MIN_X 10
#define TEXRECT_MAX_X 300
#define TEXRECT_MIN_Y 5
#define TEXRECT_MAX_Y 220

#define GLYPH_SPACE           -1
#define GLYPH_U               30
#define GLYPH_EXCLAMATION_PNT 36
#define GLYPH_TWO_EXCLAMATION 37
#define GLYPH_QUESTION_MARK   38
#define GLYPH_AMPERSAND       39
#define GLYPH_PERCENT         40
#define GLYPH_MULTIPLY        50
#define GLYPH_COIN            51
#define GLYPH_MARIO_HEAD      52
#define GLYPH_STAR            53
#define GLYPH_PERIOD          54
#define GLYPH_BETA_KEY        55
#define GLYPH_APOSTROPHE      56
#define GLYPH_DOUBLE_QUOTE    57
#define GLYPH_UMLAUT          58

extern void print_text_fmt_int(s32 x, s32 y, const char *str, s32 n);
extern void print_text(s32 x, s32 y, const char *str);
extern void print_text_centered(s32 x, s32 y, const char *str);
extern void print_caption(s32 x, s32 y, const char *str);
extern void print_quick_dev_marker(const char *str);
extern void render_text_labels(void);

/* Java caption overlay: 1 = Java draws the caption lines, 0 = the game font.
 * The strings themselves are still produced by print_caption*; these getters
 * only publish them for the overlay (see print.c). which: 0 = main line,
 * 1 = the small second line. */
extern void print_set_caption_java_mode(int javaOwns);
extern int print_caption_java_mode(void);
extern int print_caption_copy(int which, char *dst, int cap);
extern int print_caption_pos(int which, int field);

#endif /* _PRINT_H */
