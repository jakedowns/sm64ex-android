#ifndef _LEVEL_SELECT_MENU_H
#define _LEVEL_SELECT_MENU_H

#include "types.h"

extern s32 lvl_intro_update(s16 arg1, s32 arg2);

/** Debug: make the level select menu jump straight to this level (-1 = off). */
void level_select_set_auto_level(s16 level);

#endif /* _LEVEL_SELECT_MENU_H */
