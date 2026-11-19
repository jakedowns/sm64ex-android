#include <ultra64.h>
#include "sm64.h"
#include "geo_commands.h"

#include "game/level_geo.h"
#include "game/geo_misc.h"
#include "game/camera.h"
#include "game/moving_texture.h"
#include "game/screen_transition.h"
#include "game/paintings.h"

#include "make_const_nonconst.h"
#include "levels/ptc/header.h"

extern Gfx *ptc_geo_obj(s32 callContext, struct GraphNode *node, UNUSED void *context);

const GeoLayout ptc_geo[] = {
   GEO_NODE_SCREEN_AREA(10, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2),
   GEO_OPEN_NODE(),
      GEO_ZBUFFER(0),
      GEO_OPEN_NODE(),
         GEO_NODE_ORTHO(100),
         GEO_OPEN_NODE(),
            GEO_BACKGROUND_COLOR(0x0001),
         GEO_CLOSE_NODE(),
      GEO_CLOSE_NODE(),
      GEO_ZBUFFER(1),
      GEO_OPEN_NODE(),
         GEO_CAMERA_FRUSTUM_WITH_FUNC(45, 100, 20000, geo_camera_fov),
         GEO_OPEN_NODE(),
            GEO_CAMERA(1, 0, 2000, 6000, 0, 0, 0, geo_camera_main),
            GEO_OPEN_NODE(),
               GEO_DISPLAY_LIST(LAYER_OPAQUE, ptc_dl_room),
               GEO_ASM(0, ptc_geo_obj),
               GEO_ASM(1, ptc_geo_obj),
               GEO_ASM(2, ptc_geo_obj),
               GEO_ASM(3, ptc_geo_obj),
               GEO_ASM(4, ptc_geo_obj),
               GEO_ASM(5, ptc_geo_obj),
               GEO_ASM(6, ptc_geo_obj),
               GEO_ASM(7, ptc_geo_obj),
               GEO_ASM(8, ptc_geo_obj),
               GEO_ASM(9, ptc_geo_obj),
               GEO_ASM(10, ptc_geo_obj),
               GEO_ASM(11, ptc_geo_obj),
               GEO_ASM(12, ptc_geo_obj),
               GEO_DISPLAY_LIST(LAYER_OPAQUE, ptc_dl_panels),
               GEO_DISPLAY_LIST(LAYER_OPAQUE, ptc_dl_labels),
               GEO_RENDER_OBJ(),
            GEO_CLOSE_NODE(),
         GEO_CLOSE_NODE(),
      GEO_CLOSE_NODE(),
   GEO_CLOSE_NODE(),
   GEO_END(),
};
