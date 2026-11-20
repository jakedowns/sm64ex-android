//Feel free to use it in your port too, but please keep authorship!
//Touch Controls made by: VDavid003
#ifdef TOUCH_CONTROLS
#include <ultra64.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>

#include "config.h"
#include "sm64.h"
#include "game/game_init.h"
#include "game/camera.h"
#include "game/level_update.h"
#include "game/memory.h"
#include "game/segment2.h"
#include "gfx_dimensions.h"
#include "pc/gfx/gfx_pc.h"

#include "controller_api.h"
#include "controller_touchscreen.h"

#define SCREEN_WIDTH_API 1280
#define SCREEN_HEIGHT_API 960

#define LEFT_EDGE ((int)floorf(SCREEN_WIDTH_API / 2 - SCREEN_HEIGHT_API / 2 * gfx_current_dimensions.aspect_ratio))
#define RIGHT_EDGE ((int)ceilf(SCREEN_WIDTH_API / 2 + SCREEN_HEIGHT_API / 2 * gfx_current_dimensions.aspect_ratio))

#define CORRECT_TOUCH_X(x) ((x * (RIGHT_EDGE - LEFT_EDGE)) + LEFT_EDGE)
#define CORRECT_TOUCH_Y(y) (y * SCREEN_HEIGHT_API)

#define JOYSTICK_SIZE 280

enum ControlElementType {
    Joystick,
    Button
};

struct Position {
    s32 x,y;
};

struct ControlElement {
    enum ControlElementType type;
    struct Position (*GetPos)();
    u8 touchID; //0 = not being touched, 1-255 = Finger being used
    //Joystick
    int joyX, joyY;
    //Button
    int buttonID;
    char character;
    int slideTouch;
};

int gfx_touch_float = 1;
// Input from the Java overlay (screen-space controls).
int gfx_ovl_active = 0;
int gfx_ovl_stick_x = 0;
int gfx_ovl_stick_y = 0;
int gfx_ovl_right_x = 0;
int gfx_ovl_right_active = 0;
int gfx_ovl_right_y = 0;
int gfx_ovl_analog = 0;
unsigned int gfx_ovl_buttons = 0;
int gfx_hide_touch_controls = 0;

// Overlay-only buttons (not N64 pad bits). Consumed by the camera code.
#define OVL_BTN_CAM      0x01000000u   // X: toggle free camera
#define OVL_BTN_RECENTER 0x02000000u   // Y: recentre / reset view
#define OVL_BTN_BACK     0x04000000u   // on-screen SELECT: exit FPV / hold level select
u8 camera_mode_button_pressed = 0;
u8 camera_recenter_button_pressed = 0;
static unsigned int s_prev_ovl_buttons = 0;
struct Position gStickPos = { .x = 160, .y = 720 };
int gStickFloating = 0;
float gfx_touch_parallax = 0.0f;

#include "controller_touchscreen_layouts.inc"

static struct ControlElement *ControlElements = ControlElementsDefault;
static int ControlElementsLength = sizeof(ControlElementsDefault)/sizeof(struct ControlElement);

#define TRIGGER_DETECT(size) (((pos.x + size / 2 > CORRECT_TOUCH_X(event->x)) && (pos.x - size / 2 < CORRECT_TOUCH_X(event->x))) &&\
                              ((pos.y + size / 2 > CORRECT_TOUCH_Y(event->y)) && (pos.y - size / 2 < CORRECT_TOUCH_Y(event->y))))

void touch_down(struct TouchEvent* event) {
	set_current_input(touchScreen);
	
    struct Position pos;
    for(int i = 0; i < ControlElementsLength; i++) {
        if (ControlElements[i].touchID == 0) {
            pos = ControlElements[i].GetPos();
            switch (ControlElements[i].type) {
                case Joystick:
                    if (gfx_touch_float
                        && CORRECT_TOUCH_X(event->x) < (LEFT_EDGE + (RIGHT_EDGE - LEFT_EDGE) / 2)) {
                        // Floating thumbstick: appears where the thumb lands.
                        gStickPos.x = CORRECT_TOUCH_X(event->x);
                        gStickPos.y = CORRECT_TOUCH_Y(event->y);
                        gStickFloating = 1;
                        ControlElements[i].touchID = event->touchID;
                        ControlElements[i].joyX = 0;
                        ControlElements[i].joyY = 0;
                    } else if (TRIGGER_DETECT(JOYSTICK_SIZE)) {
                        ControlElements[i].touchID = event->touchID;
                        ControlElements[i].joyX = CORRECT_TOUCH_X(event->x) - pos.x;
                        ControlElements[i].joyY = CORRECT_TOUCH_Y(event->y) - pos.y;
                    }
                    break;
                case Button:
                    if (TRIGGER_DETECT(120)) {
                        ControlElements[i].touchID = event->touchID;
                    }
                    break;
            }
        }
    }
}

void touch_motion(struct TouchEvent* event) {
    struct Position pos;
    for(int i = 0; i < ControlElementsLength; i++) {
        pos = ControlElements[i].GetPos();
        if (ControlElements[i].touchID == event->touchID) {
            pos = ControlElements[i].GetPos();
                switch (ControlElements[i].type) {
                    case Joystick:
                        ; //workaround
                        s32 x,y;
                        x = CORRECT_TOUCH_X(event->x) - pos.x;
                        y = CORRECT_TOUCH_Y(event->y) - pos.y;
                        if (pos.x + JOYSTICK_SIZE/2 < CORRECT_TOUCH_X(event->x))
                            x = JOYSTICK_SIZE/2;
                        if (pos.x - JOYSTICK_SIZE/2 > CORRECT_TOUCH_X(event->x))
                            x = -JOYSTICK_SIZE/2;
                        if (pos.y + JOYSTICK_SIZE/2 < CORRECT_TOUCH_Y(event->y))
                            y = JOYSTICK_SIZE/2;
                        if (pos.y - JOYSTICK_SIZE/2 > CORRECT_TOUCH_Y(event->y))
                            y = -JOYSTICK_SIZE/2;

                        ControlElements[i].joyX = x;
                        ControlElements[i].joyY = y;
                        break;
                    case Button:
                        if (ControlElements[i].slideTouch && !TRIGGER_DETECT(120)) {
                            ControlElements[i].slideTouch = 0;
                            ControlElements[i].touchID = 0;
                        }
                        break;
                }
        }
        else {
            switch (ControlElements[i].type) {
                case Joystick:
                    break;
                case Button:
                    if (TRIGGER_DETECT(120)) {
                        ControlElements[i].slideTouch = 1;
                        ControlElements[i].touchID = event->touchID;
                    }
                    break;
            }
        }
    }
}

static void handle_touch_up(int i) {//seperated for when the layout changes
    ControlElements[i].touchID = 0;
    switch (ControlElements[i].type) {
        case Joystick:
            gStickFloating = 0;
            ControlElements[i].joyX = 0;
            ControlElements[i].joyY = 0;
            break;
        case Button:
            break;
    }
}

void touch_up(struct TouchEvent* event) {
    struct Position pos;
    for(int i = 0; i < ControlElementsLength; i++) {
        if (ControlElements[i].touchID == event->touchID) {
            handle_touch_up(i);
        }
    }
}

ALIGNED8 static const u8 texture_button[] = {
#include "textures/touchcontrols/touch_button.rgba16.inc.c"
};

ALIGNED8 static const u8 texture_button_dark[] = {
#include "textures/touchcontrols/touch_button_dark.rgba16.inc.c"
};

//Sprite drawing code stolen from src/game/print.c

static void select_button_texture(int dark) {
    const u8 *const *glyphs = segmented_to_virtual(main_hud_lut);

    gDPPipeSync(gDisplayListHead++);
    if (!dark) {
        gDPSetTextureImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, texture_button);
    }
    else {
        gDPSetTextureImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, texture_button_dark);
    }
    gSPDisplayList(gDisplayListHead++, dl_hud_img_load_tex_block);
}

static void select_char_texture(char num) {
    const u8 *const *glyphs = segmented_to_virtual(main_hud_lut);

    gDPPipeSync(gDisplayListHead++);
    gDPSetTextureImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, glyphs[num - 87]);
    gSPDisplayList(gDisplayListHead++, dl_hud_img_load_tex_block);
}

// Live knobs for on-screen control placement (percent of screen, 100 = normal).
float gfx_touch_scale = 1.0f;
float gfx_touch_dx = 0.0f;
float gfx_touch_dy = 0.0f;

static void DrawSprite(s32 x, s32 y, int scaling) {
    // Scale around the screen centre, then offset, so the controls can be
    // pushed out towards the panel edges.
    float cx0 = SCREEN_WIDTH * 0.5f;
    float cy0 = SCREEN_HEIGHT * 0.5f;
    int cx = (int)(cx0 + (x - cx0) * gfx_touch_scale + gfx_touch_dx);
    int cy = (int)(cy0 + (y - cy0) * gfx_touch_scale + gfx_touch_dy);
    int half = (int)((15 << scaling) * gfx_touch_scale);
    gSPTextureRectangle(gDisplayListHead++, cx - half, cy - half, cx + half,
                        cy + half, G_TX_RENDERTILE, 0, 0, 4 << (11 - scaling), 1 << (11 - scaling));
}

void render_touch_controls(void) {
    if (gfx_hide_touch_controls) {
        return;
    }
	
	if(get_current_input()!=touchScreen)
        return;
	
    Mtx *mtx;

    mtx = alloc_display_list(sizeof(*mtx));

    if (mtx == NULL) {
        return;
    }

    guOrtho(mtx, 0.0f, SCREEN_WIDTH, 0.0f, SCREEN_HEIGHT, -10.0f, 10.0f, 1.0f);
    gSPPerspNormalize((Gfx *) (gDisplayListHead++), 0xFFFF);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPDisplayList(gDisplayListHead++, dl_hud_img_begin);

    struct Position pos;
    for (int i = 0; i < ControlElementsLength; i++) {
        select_button_texture(0);
        switch (ControlElements[i].type) {
            case Joystick:
                pos = ControlElements[i].GetPos();
                DrawSprite(pos.x, pos.y, 3);
                DrawSprite(pos.x + 4 + ControlElements[i].joyX, pos.y + 4 + ControlElements[i].joyY, 2);
                break;
            case Button:
                if (ControlElements[i].touchID)
                    select_button_texture(1);
                pos = ControlElements[i].GetPos();
                DrawSprite(pos.x - 8, pos.y, 2);
                select_char_texture(ControlElements[i].character);
                DrawSprite(pos.x, pos.y, 1);
                break;
        }
    }

    gSPDisplayList(gDisplayListHead++, dl_hud_img_end);
}

static void touchscreen_init(void) {
}

static void touchscreen_read(OSContPad *pad) {
    // Overlay buttons are always merged on top of the hardware controller's
    // pad; only the overlay stick takes the axes over, and only while it is
    // actually being held. (Taking the stick unconditionally killed the left
    // stick of a paired gamepad after any tap on the overlay.)
    unsigned int pressed = gfx_ovl_buttons & ~s_prev_ovl_buttons;
    s_prev_ovl_buttons = gfx_ovl_buttons;
    if (pressed & OVL_BTN_CAM) {
        camera_mode_button_pressed = 1;
    }
    if (pressed & OVL_BTN_RECENTER) {
        camera_recenter_button_pressed = 1;
    }
    {
        /* On-screen SELECT/BACK mirrors the physical Back button: short press
         * exits first person, ~0.7s hold opens level select. */
        static int backFrames, backFired;
        if (gfx_ovl_buttons & OVL_BTN_BACK) {
            if (backFrames == 0) backFired = 0;
            backFrames++;
            if (!backFired && backFrames > 21) {
                backFired = 1;
                level_select_request_open(-1);
            }
        } else {
            if (backFrames > 0 && !backFired) {
                camera_exit_first_person();
            }
            backFrames = 0;
            backFired = 0;
        }
    }
    pad->button |= (u16)(gfx_ovl_buttons & 0xFFFFu);
    if (gfx_ovl_active) {
        pad->stick_x = (s8)gfx_ovl_stick_x;
        pad->stick_y = (s8)gfx_ovl_stick_y;
        return;
    }
    for(int i = 0; i < ControlElementsLength; i++) {
        switch (ControlElements[i].type) {
            case Joystick:
                if (ControlElements[i].joyX || ControlElements[i].joyY) {
                    pad->stick_x = (ControlElements[i].joyX + JOYSTICK_SIZE/2) * 255 / JOYSTICK_SIZE - 128;
                    pad->stick_y = (-ControlElements[i].joyY + JOYSTICK_SIZE/2) * 255 / JOYSTICK_SIZE - 128; //inverted for some reason
                }
                break;
            case Button:
                if (ControlElements[i].touchID) {
                    pad->button |= ControlElements[i].buttonID;
                }
                break;
        }
    }
}

static u32 touchscreen_rawkey(void) { //dunno what this does but I'll skip it for now
    return VK_INVALID;
}

struct ControllerAPI controller_touchscreen = {
    0,
    touchscreen_init,
    touchscreen_read,
    touchscreen_rawkey,
    NULL,
    NULL,
    NULL,
    NULL
};
#endif
