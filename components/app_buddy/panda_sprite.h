/*
 * Panda sprite — public interface.
 *
 * The buddy app drives the panda entirely through this surface: it describes a
 * pose (which eyes, arms, and prop to show, plus a vertical offset and a
 * backdrop color) and asks the sprite module to composite that pose onto an
 * LVGL canvas. All the pixel-art data, the palette, and the scaled blit live in
 * panda_sprite.c; the app never touches a grid or a pixel.
 *
 * A gesture in the app is a sequence of these poses. Because a pose is small
 * plain data, the gesture tables in app_buddy.c stay readable and the art stays
 * isolated here: editing the panda's look never touches the state machine.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Grid and scaling geometry --- */
/* The panda is authored on a 32x32 logical grid. Each logical pixel is blitted
 * as a PANDA_SCALE square block, so the on-screen panda is
 * (32 * PANDA_SCALE) square. At scale 5 that is 160x160 px — a medium, centered
 * buddy on the 320x240 panel with room to bob. PANDA_TOP_MARGIN_PX is the
 * resting gap from the top of the canvas to the top of the sprite, leaving
 * headroom so an upward bob does not clip and a downward bob stays on-canvas. */
#define PANDA_GRID_W       32
#define PANDA_GRID_H       32
#define PANDA_SCALE        5
#define PANDA_TOP_MARGIN_PX 18

/* --- Palette indices --- */
/* The small integers the pixel grids contain. Index 0 is reserved transparent
 * (never drawn, so overlays and the body composite cleanly). PANDA_COL_ACCENT
 * is filled at runtime from the device theme accent in panda_sprite_init, so
 * props follow a re-theme; the rest are app-private panda colors. The COUNT
 * sizes the palette array. */
enum {
    PANDA_COL_TRANSPARENT = 0,
    PANDA_COL_BLACK,
    PANDA_COL_WHITE,
    PANDA_COL_SHADOW,
    PANDA_COL_BLUSH,
    PANDA_COL_ACCENT,
    PANDA_PALETTE_COUNT,
};

/* --- Part selectors --- */
/* The eyes overlay choices. PANDA_EYES_NONE draws no eyes (used when the paws
 * cover the face in peek-a-boo). The COUNT sizes the lookup table. */
typedef enum {
    PANDA_EYES_NONE = 0,
    PANDA_EYES_OPEN,
    PANDA_EYES_CLOSED,
    PANDA_EYES_HAPPY,
    PANDA_EYES_COUNT,
} panda_eyes_t;

/* The arms overlay choices. The arms are not part of the body grid, so every
 * value here draws a real arms grid: PANDA_ARMS_REST is the default down-paws,
 * and the others pose the arms for a wave, a celebration, or peek-a-boo.
 * PANDA_ARMS_WAVE_MID is the half-raised mid-point of the wave lift, sequenced
 * between rest and the fully-raised wave so the arm travels up smoothly rather
 * than snapping. */
typedef enum {
    PANDA_ARMS_REST = 0,
    PANDA_ARMS_WAVE_MID,
    PANDA_ARMS_WAVE,
    PANDA_ARMS_UP,
    PANDA_ARMS_PEEK,
    PANDA_ARMS_COUNT,
} panda_arms_t;

/* The prop overlay choices. PANDA_PROP_NONE draws nothing; the others add a
 * theme-accent heart or sparkle floating beside the head. */
typedef enum {
    PANDA_PROP_NONE = 0,
    PANDA_PROP_HEART,
    PANDA_PROP_SPARKLE,
    PANDA_PROP_COUNT,
} panda_prop_t;

/* --- Pose --- */
/* One fully-specified frame: the parts to show, a vertical pixel offset applied
 * to the whole panda (negative is up, for bobs and hops), and the backdrop
 * color the canvas is cleared to before compositing. The app fills this from
 * its gesture keyframes and hands it to panda_sprite_render. */
typedef struct {
    panda_eyes_t eyes;
    panda_arms_t arms;
    panda_prop_t prop;
    int32_t      y_offset;
    lv_color_t   bg;
} panda_pose_t;

/*
 * Initialize the sprite module. Builds the palette, including resolving the
 * accent slot from the device theme. Call once before the first render.
 */
void panda_sprite_init(void);

/*
 * Composite a pose onto the canvas. Clears the canvas to pose->bg, then blits
 * the constant body and the pose's selected overlays, scaled and centered, at
 * the pose's vertical offset. The caller invalidates the canvas (or its changed
 * region) afterward to present the frame.
 */
void panda_sprite_render(lv_obj_t *canvas, const panda_pose_t *pose);

#ifdef __cplusplus
}
#endif