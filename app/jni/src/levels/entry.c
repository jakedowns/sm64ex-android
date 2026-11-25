#include <ultra64.h>
#include "sm64.h"
#include "segment_symbols.h"
#include "level_commands.h"

#include "levels/intro/header.h"

#include "make_const_nonconst.h"

const LevelScript level_script_entry[] = {
    INIT_LEVEL(),
    SLEEP(/*frames*/ 2),
    BLACKOUT(/*active*/ FALSE),
    SET_REG(/*value*/ 0),
    EXECUTE(/*seg*/ 0x14, /*script*/ _introSegmentRomStart, /*scriptEnd*/ _introSegmentRomEnd, /*entry*/ level_intro_entry_1),
    JUMP(/*target*/ level_script_entry),
};

/* QUICK DEV boot (DEBUG -> SYS toggle, off by default).
 *
 * Starts at the title-screen entry with the debug level-select request already
 * armed, instead of playing the logo first. `intro_default` consumes the
 * request on its first frame (it does not wait for START), then the existing
 * level-select -> `level_main_scripts_entry` path loads the configured level.
 * The title-screen entry still plays briefly, but it advances without input,
 * so the logo, the attract demo and the press-start wait are skipped without a
 * parallel level-loading path. */
const LevelScript level_script_quickdev[] = {
    INIT_LEVEL(),
    SLEEP(/*frames*/ 2),
    BLACKOUT(/*active*/ FALSE),
    EXECUTE(/*seg*/ 0x14, /*script*/ _introSegmentRomStart, /*scriptEnd*/ _introSegmentRomEnd, /*entry*/ level_intro_entry_2),
    JUMP(/*target*/ level_script_quickdev),
};
