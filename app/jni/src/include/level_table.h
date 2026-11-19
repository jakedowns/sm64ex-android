#ifndef _LEVEL_TABLE_H
#define _LEVEL_TABLE_H

// For LEVEL_NAME defines, see level_defines.h.
// Please include this file if you want to use them.

#define STUB_LEVEL(_0, levelenum, _2, _3, _4, _5, _6, _7, _8) levelenum,
#define DEFINE_LEVEL(_0, levelenum, _2, _3, _4, _5, _6, _7, _8, _9, _10) levelenum,

enum LevelNum
{
    LEVEL_NONE,
#include "levels/level_defines.h"
    LEVEL_COUNT,
    LEVEL_MAX = LEVEL_COUNT - 1,
    LEVEL_MIN = LEVEL_NONE + 1
};

#undef STUB_LEVEL
#undef DEFINE_LEVEL

// The graybox Portal Test Chamber reuses the last unused level slot so the
// level count (and every table sized by LEVEL_COUNT) stays unchanged.
#define LEVEL_PTC LEVEL_UNKNOWN_38

#endif // _LEVEL_TABLE_H
