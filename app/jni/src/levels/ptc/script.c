#include <ultra64.h>
#include "sm64.h"
#include "behavior_data.h"
#include "model_ids.h"
#include "seq_ids.h"
#include "segment_symbols.h"
#include "level_commands.h"

#include "game/level_update.h"

#include "levels/scripts.h"
#include "levels/ptc/header.h"
#include "levels/ptc/ptc_level.h"

#include "make_const_nonconst.h"

const LevelScript level_ptc_entry[] = {
    INIT_LEVEL(),
    LOAD_MIO0(/*seg*/ 0x07, _ptc_segment_7SegmentRomStart, _ptc_segment_7SegmentRomEnd),
    LOAD_RAW(/*seg*/ 0x13, _behaviorSegmentRomStart, _behaviorSegmentRomEnd),
    ALLOC_LEVEL_POOL(),
    MARIO(/*model*/ MODEL_MARIO, /*behParam*/ 0x00000001, /*beh*/ bhvMario),

    AREA(/*index*/ 1, ptc_geo),
        OBJECT(/*model*/ MODEL_STAR, /*pos*/ PTC_STAR_X, PTC_STAR_Y, PTC_STAR_Z,
               /*angle*/ 0, 0, 0, /*behParam*/ 0x00000000, /*beh*/ bhvPtcDebugStar),
        OBJECT(/*model*/ MODEL_STAR, /*pos*/ PTC_STAR2_X, PTC_STAR2_Y, PTC_STAR2_Z,
               /*angle*/ 0, 0, 0, /*behParam*/ 0x01000000, /*beh*/ bhvPtcDebugStar),
        TERRAIN(/*terrainData*/ ptc_collision),
        SET_BACKGROUND_MUSIC(/*settingsPreset*/ 0x0000, /*seq*/ SEQ_LEVEL_INSIDE_CASTLE),
        TERRAIN_TYPE(/*terrainType*/ TERRAIN_STONE),
    END_AREA(),

    FREE_LEVEL_POOL(),
    MARIO_POS(/*area*/ 1, /*yaw*/ PTC_WALK_YAW, /*pos*/ PTC_MARIO_START_X, PTC_MARIO_START_Y,
              PTC_MARIO_START_Z),
    CALL(/*arg*/ 0, /*func*/ lvl_init_or_update),
    CALL_LOOP(/*arg*/ 1, /*func*/ lvl_init_or_update),
    CLEAR_LEVEL(),
    SLEEP_BEFORE_EXIT(/*frames*/ 1),
    EXIT(),
};
