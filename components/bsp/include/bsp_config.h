/*
 * Board support configuration.
 *
 * This is the ONLY file that encodes board specifics: GPIO assignments, bus
 * parameters, and panel geometry. Porting the firmware to a different board is
 * a single-file change here. Nothing above the BSP layer references a raw pin
 * or bus number; everything reaches the hardware through these names.
 *
 * Default target board: KMRTM32032-SPI, a 3.2" 240x320 ILI9341 panel on a
 * 4-wire SPI interface, with an XPT2046 (HR2046) resistive touch controller
 * that shares the same SPI bus, on an ESP32-S3-DevKitC-1 (N16R8: 16 MB flash,
 * 8 MB octal PSRAM).
 *
 * Interface note: this is the SPI variant of the common red TFT board, NOT the
 * 8-bit parallel (i80) variant. The display lines are VCC, GND, CS, RESET,
 * D/C, SDI(MOSI), SCK, LED, and SDO(MISO). The touch lines are T_CLK, T_CS,
 * T_DIN, T_DO, and T_IRQ. The ESP32-S3 drives both through one hardware SPI
 * peripheral (SPI2): the SCK/MOSI/MISO lines are shared, and the display and
 * touch controller are selected by their own chip-select pins. The touch
 * controller is slow (a few MHz) while the panel runs tens of MHz; esp_lcd and
 * the touch driver each set their own per-transaction clock on the shared bus,
 * so sharing costs the display nothing.
 *
 * Level note: drive the board's VCC from 3V3 (not 5V) so every line is
 * ESP32-S3 safe. Power: board 3V3 -> devkit 3V3, board GND -> devkit GND. The
 * board's LED (backlight) line has no pull and must be driven: tie it to 3V3
 * for an always-on backlight, or to BSP_PIN_LCD_BL for on/off control. Leaving
 * LED floating leaves the backlight off and the screen dark even when the
 * controller is receiving pixels correctly.
 *
 * Orientation: the UI renders in LANDSCAPE (320 wide x 240 tall). The panel is
 * natively 240x320 portrait; the swap/mirror flags below rotate it into
 * landscape, so the resolution exposed above the BSP is 320x240.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_types.h"

/* --- Display panel geometry (landscape) --- */
/* The ILI9341 is natively 240x320 portrait. The UI runs landscape, so the
 * logical resolution is the transpose: 320 wide x 240 tall. The swap-xy flag
 * below performs the rotation in the panel controller, so LVGL renders
 * directly into landscape with no per-frame software rotation. */
#define BSP_LCD_H_RES 320
#define BSP_LCD_V_RES 240

/* --- Display SPI bus --- */
/* The panel is driven over the ESP32-S3 hardware SPI2 peripheral through
 * esp_lcd. SCK is the clock, MOSI carries pixel and command bytes to the
 * panel, and MISO is shared with (and only actually used by) the touch
 * controller. The command and parameter widths are 8 bits, standard for the
 * ILI9341. */
#define BSP_LCD_SPI_HOST       SPI2_HOST

/* Pixel (SPI) clock. 20 MHz is a conservative rate for jumpered/breadboard
 * wiring, whose signal integrity degrades well below what a soldered board
 * tolerates. Above this, a too-fast clock produces torn or smeared pixels (a
 * signal-integrity failure, not a color or geometry bug); raise toward 40 MHz
 * once the panel is on a clean PCB and the image stays intact. If tearing
 * appears, lower the clock to confirm it is the cause, then walk it back up to
 * the ceiling the wiring supports.
 *
 * This clock is unrelated to color banding: solid fills breaking into vertical
 * color bands while text and shapes render correctly is a pixel byte-order
 * problem (see BSP_LCD_DATA_ENDIAN below), independent of clock rate. */
#define BSP_LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define BSP_LCD_CMD_BITS       8
#define BSP_LCD_PARAM_BITS     8

/* Shared SPI bus signals. These three lines are wired to both the display and
 * the touch controller; each device adds only its own chip-select. They are
 * contiguous on the devkit headers and clear of the octal-PSRAM pins
 * (35/36/37), the USB pins (19/20), and the strapping pins (0/3/45/46). */
#define BSP_PIN_SPI_SCK  GPIO_NUM_12
#define BSP_PIN_SPI_MOSI GPIO_NUM_11
#define BSP_PIN_SPI_MISO GPIO_NUM_13

/* Display control lines. CS selects the panel on the shared bus; DC is the
 * data/command select (the board's "D/C"); RST is driven by the panel driver
 * during init (the board's "RESET"). */
#define BSP_PIN_LCD_CS  GPIO_NUM_10
#define BSP_PIN_LCD_DC  GPIO_NUM_9
#define BSP_PIN_LCD_RST GPIO_NUM_14

/* Backlight. The board's LED pin. Set to a real GPIO to control the backlight,
 * or to GPIO_NUM_NC if LED is tied to the 3V3 rail (backlight hardwired on).
 * When set to NC, the backlight init is skipped and the brightness capability
 * reports unavailable.
 *
 * This pin must not be shared with any other peripheral: bsp_backlight_init
 * binds it to an LEDC PWM channel, which would clobber the setup of anything
 * else assigned the same GPIO.
 *
 * The ON/OFF level macros describe the panel's active polarity. On this board
 * the LED line is active-high: a high level (and thus a high PWM duty) is full
 * brightness. The PWM path below treats 100% duty as the BSP_LCD_BL_ON_LEVEL
 * end of the range and 0% duty as off, so a board with an active-low backlight
 * is handled by inverting the duty in bsp_backlight_set_percent rather than by
 * rewiring callers. */
#define BSP_PIN_LCD_BL       GPIO_NUM_15
#define BSP_LCD_BL_ON_LEVEL  1
#define BSP_LCD_BL_OFF_LEVEL 0

/* Backlight PWM (LEDC) configuration. The backlight is dimmed by driving
 * BSP_PIN_LCD_BL with a hardware LEDC PWM channel rather than a plain on/off
 * GPIO level, so brightness is continuously adjustable. These resources are
 * kept distinct from the camera's LEDC_TIMER_0 / LEDC_CHANNEL_0 (used for the
 * camera XCLK in camera.c) so the two PWM users never collide on the same
 * timer or channel.
 *
 *   - BSP_BL_LEDC_MODE: the speed mode. The ESP32-S3 has only the low-speed
 *     mode group, so LEDC_LOW_SPEED_MODE is the correct (and only) choice.
 *   - BSP_BL_LEDC_TIMER / BSP_BL_LEDC_CHANNEL: the timer and channel reserved
 *     for the backlight. Timer 1 and channel 1 avoid the camera's timer/channel
 *     0.
 *   - BSP_BL_LEDC_FREQ_HZ: the PWM frequency. 5 kHz is well above the flicker
 *     threshold and is comfortably reachable at the resolution below.
 *   - BSP_BL_LEDC_RES_BITS: duty resolution. 10 bits (0..1023) gives smooth
 *     dimming steps; at 5 kHz the LEDC source clock supports this resolution on
 *     the ESP32-S3. BSP_BL_LEDC_DUTY_MAX is the full-scale duty for this
 *     resolution and is derived from it so the two never drift apart. */
#define BSP_BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define BSP_BL_LEDC_TIMER     LEDC_TIMER_1
#define BSP_BL_LEDC_CHANNEL   LEDC_CHANNEL_1
#define BSP_BL_LEDC_FREQ_HZ   (5 * 1000)
#define BSP_BL_LEDC_RES_BITS  LEDC_TIMER_10_BIT
#define BSP_BL_LEDC_DUTY_MAX  ((1 << 10) - 1)

/* Panel orientation and color flags applied during init.
 *
 * The combination below rotates the native 240x320 portrait panel into a
 * 320x240 landscape image. If the image looks wrong these are the first knobs
 * to change:
 *   - BSP_LCD_SWAP_XY: true transposes the panel so its long axis becomes the
 *     horizontal axis. This is what makes the display landscape. Leaving it
 *     false renders portrait.
 *   - BSP_LCD_MIRROR_X / BSP_LCD_MIRROR_Y: flip the image to match the physical
 *     mounting. Adjust these (not swap-xy) if the landscape image is upside
 *     down or left/right reversed.
 *   - BSP_LCD_INVERT_COLOR: many of these red ILI9341 boards require this true
 *     despite the general "ILI9341 wants false" rule; a photo-negative image is
 *     the symptom of it being set wrong.
 *   - BSP_LCD_DATA_ENDIAN: the RGB565 byte order transmitted to the panel, set
 *     on the panel driver's data_endian field (the SPI panel-IO config has no
 *     byte-swap flag). This is the knob for the failure mode where text and
 *     shapes render correctly but solid color fills break into vertical color
 *     bands: that is a 16-bit byte-order mismatch (the high/low byte of each
 *     pixel swapped), not a timing fault, and is unchanged by lowering the
 *     clock. On this esp_lcd/ILI9341 version the field has no observable effect,
 *     so the swap is instead performed in software in the LVGL flush path (see
 *     bsp.c, lvgl_flush_cb); only one layer must perform the swap. */
#define BSP_LCD_SWAP_XY          true
#define BSP_LCD_MIRROR_X         false
#define BSP_LCD_MIRROR_Y         false
#define BSP_LCD_INVERT_COLOR     false
#define BSP_LCD_DATA_ENDIAN      LCD_RGB_DATA_ENDIAN_LITTLE

/* --- Touch (XPT2046 / HR2046, SPI, resistive) --- */
/* The touch controller shares SCK/MOSI/MISO with the display on SPI2 and adds
 * its own chip select (the board's "T_CS"). T_IRQ (the board's pen-interrupt
 * line) is wired but optional: the driver polls over SPI each LVGL pass, so
 * touch works without an interrupt, and the pin is reserved here for a future
 * interrupt-driven read. The XPT2046 maxes out around a few MHz, far below the
 * panel clock; its clock is set on its own transactions.
 *
 * Board silkscreen note: the touch data-output pin is printed "T_DO" on some
 * board revisions and "T_OUT" on others; both name the same signal, the touch
 * controller's data-out line (its MISO), which shares BSP_PIN_SPI_MISO. The
 * display's own SDO(MISO) is not required by this firmware (the draw path is
 * write-only) and may be left unconnected; only the touch T_DO/T_OUT line
 * needs to reach the shared MISO pin. */
#define BSP_PIN_TOUCH_CS  GPIO_NUM_8
#define BSP_PIN_TOUCH_IRQ GPIO_NUM_7
#define BSP_TOUCH_SPI_CLK_HZ (2 * 1000 * 1000)

/* Touch orientation flags handed to the XPT2046 driver. These track the
 * display's BSP_LCD_* flags one-to-one: the touch glass is the same physical
 * surface as the panel and mounted the same way, so the orientation transform
 * that lands the image right-side up is the orientation transform that lands a
 * touch under the finger. Keeping them aliased to the display flags means a
 * future re-mounting that changes the display orientation moves touch with it,
 * with no second place to edit.
 *
 * IMPORTANT: these flags are NOT where axis INVERSION is corrected. In the
 * esp_lcd_touch base component, the read path calls process_coordinates FIRST
 * (on the raw ADC values), then applies the flags in the fixed order
 * mirror_x -> mirror_y -> swap_xy, with mirror_x computing (x_max - x) and
 * mirror_y computing (y_max - y). Here x_max/y_max are the POST-swap landscape
 * resolution (H_RES, V_RES), while process_coordinates emits values in the
 * PRE-swap native frame (native X spans V_RES, native Y spans H_RES). Using a
 * touch mirror flag to invert an axis would therefore subtract from the
 * wrong-axis maximum and offset the point, not merely flip it. Inversion is
 * instead corrected in the calibration bounds below, where the flip happens
 * against the correct per-axis native span. The mirror flags stay matched to
 * the display; inversion lives in the raw bounds. */
#define BSP_TOUCH_SWAP_XY   BSP_LCD_SWAP_XY
#define BSP_TOUCH_MIRROR_X  BSP_LCD_MIRROR_X
#define BSP_TOUCH_MIRROR_Y  BSP_LCD_MIRROR_Y

/* Resistive-touch calibration. Unlike a capacitive controller, the XPT2046
 * returns raw 12-bit ADC readings, not pixel coordinates. These bounds map the
 * raw range onto the panel in touch_process_coordinates, and they are
 * panel-and-mounting specific: the magnitudes below are typical starting points
 * and MUST be tuned on the actual unit.
 *
 * These bounds carry BOTH the scale/offset AND the DIRECTION of each axis.
 * touch_process_coordinates maps a raw reading as (raw - MIN) * (res - 1) /
 * (MAX - MIN). When MIN < MAX the mapping ascends (raw low -> pixel 0); when
 * MIN > MAX the span goes negative and the mapping descends (raw low -> pixel
 * max), which inverts that axis. The function's arithmetic and clamp handle a
 * negative span, so inverting an axis is simply a matter of ordering MIN above
 * MAX.
 *
 * Inversion is corrected HERE, in the bounds, rather than via BSP_TOUCH_MIRROR_*
 * flags, because the esp_lcd_touch base applies its mirror flags after this
 * callback against the POST-swap x_max/y_max, which do not match the PRE-swap
 * per-axis spans this callback maps against (native X spans V_RES, native Y
 * spans H_RES) -- so a mirror flag would offset the point, not just flip it.
 * Flipping the bounds inverts against the correct native span with no offset
 * error.
 *
 * Both axes are flipped here (MIN ordered above MAX) to correct a touch that
 * registers inverted on both axes (a top-left press landing at bottom-right).
 * Tuning procedure on a fresh unit: log raw x/y while touching the four
 * corners; for an axis whose touch tracks the right direction, set its
 * lower-numbered bound as MIN; for an axis that runs backwards, set its
 * lower-numbered bound as MAX (order them high-to-low). The magnitudes (here
 * ~300 and ~3900) are the smallest and largest raw values seen on that axis
 * regardless of which is MIN vs MAX. */
#define BSP_TOUCH_RAW_X_MIN 3900
#define BSP_TOUCH_RAW_X_MAX 300
#define BSP_TOUCH_RAW_Y_MIN 3900
#define BSP_TOUCH_RAW_Y_MAX 300

/* --- LVGL draw buffer sizing --- */
/* Draw buffers live in DMA-capable internal RAM. Sizing one buffer to a
 * fraction of the framebuffer (here, 1/10 of the screen in pixels) keeps
 * internal-RAM pressure low while giving the flush path enough to work with.
 * Two buffers are allocated for double-buffered, flicker-free flushing.
 * Sizing is in pixels and follows the landscape geometry above. */
#define BSP_LCD_DRAW_BUF_LINES (BSP_LCD_V_RES / 10)
#define BSP_LCD_DRAW_BUF_PIXELS (BSP_LCD_H_RES * BSP_LCD_DRAW_BUF_LINES)

/* Largest single SPI transfer, in bytes: one full draw buffer of RGB565
 * pixels. The SPI bus max_transfer_sz is sized from this. */
#define BSP_LCD_MAX_TRANSFER_BYTES (BSP_LCD_DRAW_BUF_PIXELS * 2)

/* --- Camera (DVP parallel interface) --- */
/* Target module: OV7670 (no FIFO), the blue 18-pin dual-row board. Driven over
 * the parallel DVP bus by the esp32-camera driver. The GPIO assignments below
 * are chosen for the ESP32-S3-DevKitC-1 (N16R8) to avoid every pin already
 * claimed elsewhere in this config and every pin that is unsafe to repurpose:
 *
 *   - Display SPI bus and control: 9, 10, 11, 12, 13, 14, 15.
 *   - Touch CS / IRQ: 7, 8.
 *   - Octal PSRAM (N16R8): 35, 36, 37 -- reserved by the PSRAM controller and
 *     must never be used for anything else.
 *   - Native USB-Serial-JTAG: 19, 20 -- left free so USB flashing and logging
 *     keep working through camera bring-up.
 *   - Strapping pins: 0, 3, 46 -- avoided so boot mode and flash voltage are
 *     not disturbed by the camera's signal levels at reset.
 *
 * The data bus is mapped to the high-numbered GPIOs so the lower contiguous
 * block stays available for the control and clock lines. D0 sits on GPIO 21 to
 * keep the eight data lines as a near-contiguous group across the two devkit
 * headers.
 *
 * Note on D7 (GPIO 45): 45 is a strapping pin, normally avoided. It is used
 * here only as a camera data line, which is sampled by the sensor's PCLK well
 * after reset and boot strapping have completed, so its boot-time level does
 * not affect this board. If a different board is sensitive to the level on 45
 * at reset, move D7 to another free high GPIO.
 *
 * RESET and PWDN are tied in hardware rather than driven by a GPIO. CRITICAL
 * polarity: RESET is active-low and must be held HIGH (to 3.3V) to keep the
 * sensor out of reset; PWDN must be held LOW (to GND) to keep the sensor
 * powered up. Swapping these two -- PWDN to 3.3V, RESET to GND -- holds the
 * sensor simultaneously powered down AND in reset, so it never acknowledges on
 * the SCCB control bus and the driver reports "Detected camera not supported"
 * even when every data, clock, and power line is correct. The check for that
 * symptom is to measure PWDN (must read ~0 V) and RESET (must read ~3.3 V) at
 * the module. Both macros stay GPIO_NUM_NC; camera.c treats NC on these two as
 * "fixed regulator, leave the GPIO unconfigured" and proceeds. The pin-presence
 * guard in camera.c checks the clock, sync, and data lines (not RESET/PWDN), so
 * leaving these two NC does not make the camera report itself absent.
 *
 * BSP_CAM_XCLK_FREQ_HZ is the master clock fed to the sensor (the module's MCLK
 * pin), set to 10 MHz rather than the OV-series 20 MHz default. The no-FIFO
 * OV7670 has no frame buffer to absorb DVP timing slop, and over jumper wires a
 * 20 MHz pixel clock returns data faster than the loosely-routed D0-D7 lines
 * can settle between PCLK edges. The symptom of too-fast a clock on this wiring
 * is a viewfinder full of structured horizontal-streak color noise (frame
 * geometry and sync intact, but the per-pixel bytes corrupted) -- distinct from
 * a sensor that will not probe at all. 10 MHz gives the data lines time to
 * settle; it can be raised toward 20 MHz on a clean PCB with short traces. If
 * 10 MHz is still noisy, shorten the D0-D7 and PCLK jumpers or drop to 8 MHz.
 * The viewfinder targets RGB565 so frames blit to an LVGL canvas with no decode
 * step.
 *
 * Module-pin to GPIO map (silkscreen label -> ESP32-S3 GPIO):
 *   3.3V -> 3V3        GND  -> GND
 *   SDA  -> 4          SCL  -> 5
 *   VS   -> 6          HS   -> 16
 *   PCLK -> 17         MCLK -> 18
 *   D0   -> 21         D1   -> 38
 *   D2   -> 39         D3   -> 40
 *   D4   -> 41         D5   -> 42
 *   D6   -> 47         D7   -> 45
 *   RST  -> 3.3V (tie high)   PWDN -> GND (tie low) */
#define BSP_CAM_XCLK_FREQ_HZ (10 * 1000 * 1000)

#define BSP_PIN_CAM_PWDN  GPIO_NUM_NC
#define BSP_PIN_CAM_RESET GPIO_NUM_NC
#define BSP_PIN_CAM_XCLK  GPIO_NUM_18
#define BSP_PIN_CAM_SCCB_SDA GPIO_NUM_4
#define BSP_PIN_CAM_SCCB_SCL GPIO_NUM_5
#define BSP_PIN_CAM_VSYNC GPIO_NUM_6
#define BSP_PIN_CAM_HREF  GPIO_NUM_16
#define BSP_PIN_CAM_PCLK  GPIO_NUM_17
#define BSP_PIN_CAM_D0    GPIO_NUM_21
#define BSP_PIN_CAM_D1    GPIO_NUM_38
#define BSP_PIN_CAM_D2    GPIO_NUM_39
#define BSP_PIN_CAM_D3    GPIO_NUM_40
#define BSP_PIN_CAM_D4    GPIO_NUM_41
#define BSP_PIN_CAM_D5    GPIO_NUM_42
#define BSP_PIN_CAM_D6    GPIO_NUM_47
#define BSP_PIN_CAM_D7    GPIO_NUM_45