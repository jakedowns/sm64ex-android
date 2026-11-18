#include "cheats.h"

struct CheatList Cheats = {
    // The Android build has no command line; keep the cheat menu visible by
    // default. The pause-menu cheat toggle can still hide it, and the vanilla
    // triple-L unlock remains available.
    .EnableCheats = true,
};
