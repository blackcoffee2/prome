/*
 * Theme — the single source of visual truth.
 *
 * Every color, gradient stop, metric, and font reference the launcher and app
 * screens use lives here, named by role. Editing this one file restyles the
 * entire device. The active values implement a light, faintly blue menu wash; a
 * status pane and softkey pane as light glass strips; and a main pane of bare
 * icons with labels beneath them and a blue selection highlight behind the
 * focused icon. There is no per-icon card or container -- the icon art sits
 * directly on the background.
 *
 * Naming rule: identifiers describe ROLE (surface, accent, cell, band), never
 * an external product whose look is being mirrored. Any design lineage is kept
 * entirely out of the code.
 *
 * Scope rule: this file holds values that express the DEVICE's visual identity
 * -- the palette and the type family the whole UI shares. Metrics and sizes
 * private to a single app (a widget's pixel dimensions, an app-only font
 * choice) live with that app, not here. An app reuses a theme color when it
 * wants to participate in the device look (the focus timer's ring reusing the
 * accent), and keeps its own geometry local.
 */
#pragma once

#include "lvgl.h"

/* --- Wallpaper --- */
/* Full-screen background image for the launcher (and any screen that opts in).
 * The launcher paints it as the screen's background image, layered over the
 * gradient surface below; the gradient remains the base beneath it.
 *
 * The image is a pre-generated LVGL descriptor, wallpaper_dsc, defined in
 * components/shell/wallpaper.c (a 320x240 image converted offline and compiled
 * in as const, so it lives in flash and costs no RAM or filesystem). It is
 * declared extern here and THEME_WALLPAPER_SRC points at it.
 *
 * This macro is always a valid descriptor address, never NULL: the wallpaper
 * ships with the device and is unconditionally present. The launcher therefore
 * sets it without a NULL check (a NULL check here would be an always-true
 * comparison that GCC 12's -Werror=address rejects). To make the wallpaper
 * genuinely optional again, the macro would need to become a real runtime
 * pointer rather than the address of a static object. */
extern const lv_image_dsc_t wallpaper_dsc;
#define THEME_WALLPAPER_SRC (&wallpaper_dsc)

/* --- Surfaces and gradients --- */
/* The backdrop is a light, faintly blue vertical wash from a near-white top to
 * a slightly deeper blue-grey bottom, a soft menu background. The wallpaper
 * sits over this; the gradient is the base. */
#define THEME_SURFACE_TOP    lv_color_hex(0xF4F6FA)
#define THEME_SURFACE_BOTTOM lv_color_hex(0xE2E8F1)
#define THEME_SURFACE_GRAD_DIR LV_GRAD_DIR_VER

/* --- Bands --- */
/* The status (title) pane and softkey (control) pane sit as light glass strips
 * a touch bluer than the surface, with a subtle vertical sheen. Text on them is
 * dark slate so it reads on the light fill. */
#define THEME_BAND_FILL      lv_color_hex(0xDCE4F0)
#define THEME_BAND_SHEEN     lv_color_hex(0xC9D6E8)
#define THEME_BAND_TEXT      lv_color_hex(0x2A3340)
#define THEME_BAND_HEIGHT_PX 28

/* --- Cells (icon + label, no container) --- */
/* A cell is an invisible, focusable hit area that holds one icon and its label.
 * It has no fill, border, or radius: the icon art is drawn directly on the
 * background and the name sits beneath it.
 *
 * Geometry derivation for a 4-column x 2-row grid on the 320x240 panel:
 *   - The main pane is 240 - 2*28 = 184 px tall and 320 px wide.
 *   - The grid container adds THEME_CELL_GAP_PX of pad_all (6 px each edge) and
 *     THEME_CELL_GAP_PX of inter-cell gap, so the usable inner width is
 *     320 - 2*6 = 308 px. For four cells with three 6 px gaps between them the
 *     cell budget is 308 - 3*6 = 290 px, i.e. 72.5 px per cell. THEME_CELL_W is
 *     set to 70 px (4*70 + 3*6 = 298 <= 308) so four cells always fit one row
 *     with margin to spare and never wrap to three.
 *   - The usable inner height is 184 - 2*6 = 172 px; with one 6 px inter-row
 *     gap for two rows that leaves 166 px for cells, i.e. 83 px per row.
 *     THEME_CELL_H is set to 82 px (2*82 + 6 + 2*6 = 182 <= 184) so two rows fit
 *     cleanly inside the pane.
 * A 70x82 cell is what lets the intended 4-column layout materialize rather
 * than wrapping to three columns. */
#define THEME_CELL_W       70
#define THEME_CELL_H       82
#define THEME_CELL_GAP_PX  6
#define THEME_CELL_PAD_PX  4

/* Icon art size within a cell. The launcher constrains the icon image object to
 * this box and scales the source art to fit, so an icon renders at this size
 * regardless of the resolution its PNG was converted at. Set to 52 px: within
 * an 82 px cell, after 4 px top/bottom padding (74 px usable), a 52 px icon plus
 * the 2 px flex gap plus the ~14 px line of the smaller cell font still leaves
 * the label fully inside the cell, while giving the icon clear visual weight. */
#define THEME_ICON_SIZE_PX 52

/* The label color for the name beneath each icon. White, so the name reads
 * against the wallpaper rather than the light band fill -- the icons sit on the
 * darker wallpaper region, where dark slate would disappear. Distinct from the
 * band text (which stays dark on the light glass strips) only in role. */
#define THEME_CELL_TEXT    lv_color_hex(0xFFFFFF)

/* --- App text --- */
/* The default text color for content an app renders directly on its root. The
 * shell sets this on every app root, so an app that creates a label without
 * specifying a color gets legible text without each app restating it. App roots
 * are transparent over the screen's black base, so the default is white; an app
 * that paints its own light surface (or a widget like the list that carries its
 * own text color) overrides this locally. Lives here, in the theme, so the
 * whole device's app text restyles from one place. */
#define THEME_APP_TEXT     lv_color_hex(0xFFFFFF)

/* --- Selection accent (the signature) --- */
/* A single saturated blue is the one bold element. In the icon grid it is a
 * soft rounded highlight painted BEHIND the focused icon (not a ring around a
 * card, since there is no card). The launcher applies it to the cell's focused
 * state as a filled rounded background with a soft glow. */
#define THEME_ACCENT          lv_color_hex(0x2E8BFF)
#define THEME_ACCENT_GLOW     lv_color_hex(0x9CC6FF)
#define THEME_ACCENT_RADIUS_PX 12

/* --- Focus timer ring colors (clock app) --- */
/* The clock app renders an elapsed-time "focus timer" whose centerpiece is a
 * sweeping ring. Only the ring's two COLORS live here, because they express the
 * device look: the filled portion reuses the signature accent so the timer
 * shares the launcher's selection blue, and re-theming the accent should carry
 * the timer with it. To give the timer its own distinct color, change
 * THEME_TIMER_RING alone -- the app references no color literal directly.
 *
 * Everything else about the ring -- its diameter, stroke width, and the readout
 * fonts -- is geometry private to the clock app and is defined there, not here,
 * since no other screen shares those metrics.
 *
 * THEME_TIMER_TRACK is the unfilled remainder of the ring: a dim slate that
 * reads as a recessed groove on the black app base without competing with the
 * filled arc. */
#define THEME_TIMER_RING   THEME_ACCENT
#define THEME_TIMER_TRACK  lv_color_hex(0x2A3340)

/* --- Typography --- */
/* One family at sizes that carry the whole UI: band captions (title and clock)
 * at 14, cell labels at the smaller 12 so the name sits compactly beneath a
 * larger icon, and an 18 weight for screen titles and the placeholder icon
 * initial. Sizes map to LVGL's built-in fonts, so no custom font asset ships;
 * swapping to a custom face is a change to these macros alone.
 *
 * These are the device-wide type roles. An app that needs a size used nowhere
 * else (the focus timer's large readout face) declares that font locally rather
 * than adding a one-consumer role here.
 *
 * THEME_FONT_CELL uses montserrat_12, which must be enabled in
 * sdkconfig.defaults (CONFIG_LV_FONT_MONTSERRAT_12) or the build cannot resolve
 * the symbol. */
#define THEME_FONT_BAND  (&lv_font_montserrat_14)
#define THEME_FONT_CELL  (&lv_font_montserrat_12)
#define THEME_FONT_TITLE (&lv_font_montserrat_18)