#include <ultra64.h>

#include "sm64.h"
#include "gfx_dimensions.h"
#include "game_init.h"
#include "mario.h"
#include "memory.h"
#include "save_file.h"
#include "main.h"
#include "engine/surface_collision.h"
#include "geo_misc.h"
#include "segment2.h"
#include "print.h"

/**
 * This file handles printing and formatting the colorful text that
 * appears when printing things such as "PRESS START".
 */

struct TextLabel {
    u32 x;
    u32 y;
    s16 length;
    char buffer[50];
};

/**
 * Stores the text to be rendered on screen
 * and how they are to be rendered.
 */
struct TextLabel *sTextLabels[256];
s16 sTextLabelsCount = 0;

/* One small caption line (used by the PTC trailer feature labels), drawn at
 * half the HUD glyph size so it does not dominate the frame. */
static char sCaptionText[64];
static s32 sCaptionX = 160, sCaptionY = 160;
static s32 sCaptionLen = 0;
static f32 sCaptionScale = 0.5f;

void print_caption(s32 x, s32 y, const char *str) {
    s32 n = 0;
    while (str[n] != 0 && n < 63) {
        sCaptionText[n] = str[n];
        n++;
    }
    sCaptionText[n] = 0;
    sCaptionLen = n;
    sCaptionX = x;
    sCaptionY = y;
    sCaptionScale = 0.5f;
}

/* Quarter-size HUD text for dense test annotations. */
/* A second, independent caption line so a running feature label (e.g. the
 * light loop) and a test-settings line can be on screen at once. */
static char sCaptionText2[64];
static s32 sCaptionX2 = 160, sCaptionY2 = 160;
static s32 sCaptionLen2 = 0;
static f32 sCaptionScale2 = 0.25f;

void print_caption2_small(s32 x, s32 y, const char *str) {
    s32 n = 0;
    while (str[n] != 0 && n < 63) {
        sCaptionText2[n] = str[n];
        n++;
    }
    sCaptionText2[n] = 0;
    sCaptionLen2 = n;
    sCaptionX2 = x;
    sCaptionY2 = y;
    sCaptionScale2 = 0.25f;
}

void print_caption_small(s32 x, s32 y, const char *str) {
    s32 n = 0;
    while (str[n] != 0 && n < 63) {
        sCaptionText[n] = str[n];
        n++;
    }
    sCaptionText[n] = 0;
    sCaptionLen = n;
    sCaptionX = x;
    sCaptionY = y;
    sCaptionScale = 0.25f;
}

/* Small corner marker for QUICK DEV screenshots. Kept in its own slot so the
 * PTC feature labels cannot overwrite it. Rebuilt every frame by
 * ptc_chain_hud_text(); an empty string clears it. */
static char sQuickDevText[24];
static s32 sQuickDevLen = 0;

void print_quick_dev_marker(const char *str) {
    s32 n = 0;
    if (str == NULL) {
        sQuickDevLen = 0;
        return;
    }
    while (str[n] != 0 && n < 23) {
        sQuickDevText[n] = str[n];
        n++;
    }
    sQuickDevText[n] = 0;
    sQuickDevLen = n;
}

/**
 * Returns n to the exponent power, only for non-negative powers.
 */
s32 int_pow(s32 n, s32 exponent) {
    s32 result = 1;
    s32 i;

    for (i = 0; i < exponent; i++) {
        result = n * result;
    }

    return result;
}

/**
 * Formats an integer n for print by fitting it to width, prefixing with a negative,
 * and converting the base.
 */
void format_integer(s32 n, s32 base, char *dest, s32 *totalLength, u8 width, s8 zeroPad) {
    u32 powBase;
    s32 numDigits = 0;
    s32 i;
    s32 len = 0;
    s8 digit;
    s8 negative = FALSE;
    char pad;

    if (zeroPad == TRUE) {
        pad = '0';
    } else {
        pad = -1;
    }

    if (n != 0) {
        // Formats a negative number for negative prefix.
        if (n < 0) {
            n = -n;
            negative = TRUE;
        }

        // Increments the number of digits until length is long enough.
        while (1) {
            powBase = int_pow(base, numDigits);

            if (powBase > (u32) n) {
                break;
            }

            numDigits++;
        }

        // Add leading pad to fit width.
        if (width > numDigits) {
            for (len = 0; len < width - numDigits; len++) dest[len] = pad;

            // Needs 1 length to print negative prefix.
            if (negative == TRUE) {
                len--;
            }
        }

        // Use 'M' prefix to indicate negative numbers.
        if (negative == TRUE) {
            dest[len] = 'M';
            len++;
        }

        // Transfer the digits into the proper base.
        for (i = numDigits - 1; i >= 0; i--) {
            powBase = int_pow(base, i);
            digit = n / powBase;

            // FIXME: Why doesn't [] match?
            if (digit < 10) {
                *(dest + len + numDigits - 1 - i) = digit + '0';
            } else {
                *(dest + len + numDigits - 1 - i) = digit + '7';
            }

            n -= digit * powBase;
        }
    } else // n is zero.
    {
        numDigits = 1;
        if (width > numDigits) {
            for (len = 0; len < width - numDigits; len++) dest[len] = pad;
        }
        dest[len] = '0';
    }

    *totalLength += numDigits + len;
}

/**
 * Determines the width of the number for printing, writing to 'width'.
 * Additionally, this determines if a number should be zero-padded,
 * writing to 'zeroPad'.
 */
void parse_width_field(const char *str, s32 *srcIndex, u8 *width, s8 *zeroPad) {
    s8 digits[12]; // unknown length
    s8 digitsLen = 0;
    s16 i;

    // If first character is 0, then the string should be zero padded.
    if (str[*srcIndex] == '0') {
        *zeroPad = TRUE;
    }

    // Read width digits up until the 'd' or 'x' format specifier.
    while (str[*srcIndex] != 'd' && str[*srcIndex] != 'x') {
        digits[digitsLen] = str[*srcIndex] - '0';

        if (digits[digitsLen] < 0 || digits[digitsLen] >= 10) // not a valid digit
        {
            *width = 0;
            return;
        }

        digitsLen++;
        (*srcIndex)++;
    }

    // No digits
    if (digitsLen == 0) {
        return;
    }

    // Sum the digits to calculate the total width.
    for (i = 0; i < digitsLen - 1; i++) {
        *width = *width + digits[i] * ((digitsLen - i - 1) * 10);
    }

    *width = *width + digits[digitsLen - 1];
}

/**
 * Takes a number, finds the intended base, formats the number, and prints it
 * at the given X & Y coordinates.
 *
 * Warning: this fails on too large numbers, because format_integer has bugs
 * related to overflow. For romhacks, prefer sprintf + print_text.
 */
void print_text_fmt_int(s32 x, s32 y, const char *str, s32 n) {
    char c = 0;
    s8 zeroPad = FALSE;
    u8 width = 0;
    s32 base = 0;
    s32 len = 0;
    s32 srcIndex = 0;

    // Don't continue if there is no memory to do so.
    if (sTextLabelsCount >= 256) {
        return;
    }
    if ((sTextLabels[sTextLabelsCount] = mem_pool_alloc(gEffectsMemoryPool,
                                                        sizeof(struct TextLabel))) == NULL) {
        return;
    }

    sTextLabels[sTextLabelsCount]->x = x;
    sTextLabels[sTextLabelsCount]->y = y;

    c = str[srcIndex];

    while (c != 0) {
        if (c == '%') {
            srcIndex++;

            parse_width_field(str, &srcIndex, &width, &zeroPad);

            if (str[srcIndex] != 'd' && str[srcIndex] != 'x') {
                break;
            }
            if (str[srcIndex] == 'd') {
                base = 10;
            }
            if (str[srcIndex] == 'x') {
                base = 16;
            }

            srcIndex++;

            format_integer(n, base, sTextLabels[sTextLabelsCount]->buffer + len, &len, width, zeroPad);
        } else // straight copy
        {
            sTextLabels[sTextLabelsCount]->buffer[len] = c;
            len++;
            srcIndex++;
        }
        c = str[srcIndex];
    }

    sTextLabels[sTextLabelsCount]->length = len;
    sTextLabelsCount++;
}

/**
 * Prints text in the colorful lettering at given X, Y coordinates.
 */
void print_text(s32 x, s32 y, const char *str) {
    char c = 0;
    s32 length = 0;
    s32 srcIndex = 0;

    // Don't continue if there is no memory to do so.
    if (sTextLabelsCount >= 256) {
        return;
    }
    if ((sTextLabels[sTextLabelsCount] = mem_pool_alloc(gEffectsMemoryPool,
                                                        sizeof(struct TextLabel))) == NULL) {
        return;
    }

    sTextLabels[sTextLabelsCount]->x = x;
    sTextLabels[sTextLabelsCount]->y = y;

    c = str[srcIndex];

    // Set the array with the text to print while finding length.
    // Truncate instead of overflowing TextLabel.buffer[50]; debug labels can
    // be longer than the font row and a stack/heap overflow would corrupt the
    // whole display list.
    while (c != 0 && length < 49) {
        sTextLabels[sTextLabelsCount]->buffer[length] = c;
        length++;
        srcIndex++;
        c = str[srcIndex];
    }

    sTextLabels[sTextLabelsCount]->length = length;
    sTextLabelsCount++;
}

/**
 * Prints text in the colorful lettering centered
 * at given X, Y coordinates.
 */
void print_text_centered(s32 x, s32 y, const char *str) {
    char c = 0;
    UNUSED s8 unused1 = 0;
    UNUSED s32 unused2 = 0;
    s32 length = 0;
    s32 srcIndex = 0;

    // Don't continue if there is no memory to do so.
    if (sTextLabelsCount >= 256) {
        return;
    }
    if ((sTextLabels[sTextLabelsCount] = mem_pool_alloc(gEffectsMemoryPool,
                                                        sizeof(struct TextLabel))) == NULL) {
        return;
    }

    c = str[srcIndex];

    // Set the array with the text to print while finding length.
    while (c != 0) {
        sTextLabels[sTextLabelsCount]->buffer[length] = c;
        length++;
        srcIndex++;
        c = str[srcIndex];
    }

    sTextLabels[sTextLabelsCount]->length = length;
    sTextLabels[sTextLabelsCount]->x = x - length * 12 / 2;
    sTextLabels[sTextLabelsCount]->y = y;
    sTextLabelsCount++;
}

/**
 * Converts a char into the proper colorful glyph for the char.
 */
s8 char_to_glyph_index(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 55;
    }

    if (c >= 'a' && c <= 'z') {
        return c - 87;
    }

    if (c >= '0' && c <= '9') {
        return c - 48;
    }

    if (c == ' ') {
        return GLYPH_SPACE;
    }

    if (c == '!') {
        return GLYPH_EXCLAMATION_PNT; // !, JP only
    }

    if (c == '#') {
        return GLYPH_TWO_EXCLAMATION; // !!, JP only
    }

    if (c == '?') {
        return GLYPH_QUESTION_MARK; // ?, JP only
    }

    if (c == '&') {
        return GLYPH_AMPERSAND; // &, JP only
    }

    if (c == '%') {
        return GLYPH_PERCENT; // %, JP only
    }

    if (c == '*') {
        return GLYPH_MULTIPLY; // x
    }

    if (c == '+') {
        return GLYPH_COIN; // coin
    }

    if (c == ',') {
        return GLYPH_MARIO_HEAD; // Imagine I drew Mario's head
    }

    if (c == '-') {
        return GLYPH_STAR; // star
    }

    if (c == '.') {
        return GLYPH_PERIOD; // large shaded dot, JP only
    }

    if (c == '/') {
        return GLYPH_BETA_KEY; // beta key, JP only. Reused for Ü in EU.
    }

    return GLYPH_SPACE;
}

/**
 * Adds an individual glyph to be rendered.
 */
void add_glyph_texture(s8 glyphIndex) {
    const u8 *const *glyphs = segmented_to_virtual(main_hud_lut);

    gDPPipeSync(gDisplayListHead++);
    gDPSetTextureImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, glyphs[glyphIndex]);
    gSPDisplayList(gDisplayListHead++, dl_hud_img_load_tex_block);
}

/**
 * Renders the glyph that's set at the given position.
 */
void render_textrect(s32 x, s32 y, s32 pos) {
    s32 rectBaseX = x + pos * 12;
    s32 rectBaseY = 224 - y;
    s32 rectX;
    s32 rectY;

    rectX = rectBaseX;
    rectY = rectBaseY;
    gSPTextureRectangle(gDisplayListHead++, rectX << 2, rectY << 2, (rectX + 15) << 2,
                        (rectY + 15) << 2, G_TX_RENDERTILE, 0, 0, 4 << 10, 1 << 10);
}

/**
 * Renders the text in sTextLabels on screen at the proper locations by iterating
 * a for loop.
 */
static void draw_caption_text(const char *text, s32 len, s32 x, s32 y, f32 sc) {
    s32 j;
    s8 glyphIndex;
    if (len <= 0) {
        return;
    }
    s32 stepS = (s32) (4.0f / sc) << 10;
    s32 stepT = (s32) (1.0f / sc) << 10;
    s32 startX = x - (s32) (len * 12 * sc / 2.0f);
    s32 cy = 224 - y;
    for (j = 0; j < len; j++) {
        glyphIndex = char_to_glyph_index(text[j]);
        if (glyphIndex != GLYPH_SPACE) {
            s32 rx = startX + (s32) (j * 12 * sc);
            add_glyph_texture(glyphIndex);
            gSPTextureRectangle(gDisplayListHead++, rx << 2, cy << 2,
                                (rx + (s32) (16 * sc)) << 2,
                                (cy + (s32) (16 * sc)) << 2,
                                G_TX_RENDERTILE, 0, 0, stepS, stepT);
        }
    }
}

void render_text_labels(void) {
    s32 i;
    s32 j;
    s8 glyphIndex;
    Mtx *mtx;

    if (sTextLabelsCount == 0 && sQuickDevLen == 0) {
        return;
    }

    mtx = alloc_display_list(sizeof(*mtx));

    if (mtx == NULL) {
        sTextLabelsCount = 0;
        return;
    }

    guOrtho(mtx, 0.0f, SCREEN_WIDTH, 0.0f, SCREEN_HEIGHT, -10.0f, 10.0f, 1.0f);
    gSPPerspNormalize((Gfx *) (gDisplayListHead++), 0xFFFF);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPDisplayList(gDisplayListHead++, dl_hud_img_begin);

    draw_caption_text(sCaptionText, sCaptionLen, sCaptionX, sCaptionY, sCaptionScale);
    sCaptionLen = 0;
    draw_caption_text(sCaptionText2, sCaptionLen2, sCaptionX2, sCaptionY2, sCaptionScale2);
    sCaptionLen2 = 0;
    /* Quick-dev marker: bottom-left, quarter size, self-documenting shots.
     * Hidden with the rest of the HUD so trailer captures stay clean. */
    {
        extern int gHudForceHidden;
        if (!gHudForceHidden) {
            /* y=52 clears both the collapsed Java panel and the PTC caption
             * line (y=34), so the marker is readable in every screenshot. */
            draw_caption_text(sQuickDevText, sQuickDevLen, 84, 52, 0.25f);
        }
    }

    for (i = 0; i < sTextLabelsCount; i++) {
        for (j = 0; j < sTextLabels[i]->length; j++) {
            glyphIndex = char_to_glyph_index(sTextLabels[i]->buffer[j]);

            if (glyphIndex != GLYPH_SPACE) {
#ifdef VERSION_EU
                // Beta Key was removed by EU, so glyph slot reused.
                // This produces a colorful Ü.
                if (glyphIndex == GLYPH_BETA_KEY) {
                    add_glyph_texture(GLYPH_U);
                    render_textrect(sTextLabels[i]->x, sTextLabels[i]->y, j);

                    add_glyph_texture(GLYPH_UMLAUT);
                    render_textrect(sTextLabels[i]->x, sTextLabels[i]->y + 3, j);
                } else {
                    add_glyph_texture(glyphIndex);
                    render_textrect(sTextLabels[i]->x, sTextLabels[i]->y, j);
                }
#else
                add_glyph_texture(glyphIndex);
                render_textrect(sTextLabels[i]->x, sTextLabels[i]->y, j);
#endif
            }
        }

        mem_pool_free(gEffectsMemoryPool, sTextLabels[i]);
    }

    gSPDisplayList(gDisplayListHead++, dl_hud_img_end);

    sTextLabelsCount = 0;
}
