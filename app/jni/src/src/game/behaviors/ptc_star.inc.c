// ptc_star.inc.c
// Debug-only collectible star for the Portal Test Chamber.
//
// It deliberately uses INTERACT_COIN instead of INTERACT_STAR_OR_KEY: the coin
// handler only marks the object as interacted with, so collecting this star
// never calls save_file_collect_star_or_key() and never changes the player's
// real star count.  The visible star model is still MODEL_STAR.

static struct ObjectHitbox sPtcDebugStarHitbox = {
    /* interactType:      */ INTERACT_COIN,
    /* downOffset:        */ 0,
    /* damageOrCoinValue: */ 0,
    /* health:            */ 0,
    /* numLootCoins:      */ 0,
    /* radius:            */ 80,
    /* height:            */ 80,
    /* hurtboxRadius:     */ 0,
    /* hurtboxHeight:     */ 0,
};

void bhv_ptc_debug_star_init(void) {
    obj_set_hitbox(o, &sPtcDebugStarHitbox);
    o->oDamageOrCoinValue = 0;
    if (ptc_star_collected_index((o->oBehParams >> 24) & 0xFF)) {
        o->oPosY = -10000.0f;
    }
}

void bhv_ptc_debug_star_loop(void) {
    if (ptc_star_collected_index((o->oBehParams >> 24) & 0xFF)) {
        o->oPosY = -10000.0f;
        o->oInteractStatus = 0;
        return;
    }

    o->oFaceAngleYaw += 0x800;

    if (o->oInteractStatus & INT_STATUS_INTERACTED) {
        o->oInteractStatus = 0;
        ptc_star_mark_collected_index((o->oBehParams >> 24) & 0xFF);
        o->oPosY = -10000.0f;
        cur_obj_play_sound_2(SOUND_MENU_STAR_SOUND);
        spawn_object(o, MODEL_SPARKLES, bhvStarKeyCollectionPuffSpawner);
    }
}
