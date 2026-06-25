# PROME Wiring Diagram

Pin assignments for the ESP32-S3-DevKitC-1 (N16R8). All values are taken from
`components/bsp/include/bsp_config.h` — that file is the single source of truth,
so if a pin changes there, update this table to match.

**Power:** drive the board's VCC from **3V3, not 5V**, so every line stays
ESP32-S3 safe.

## ESP32-S3 to LCD (ILI9341, 4-wire SPI + XPT2046 touch)

The display and the XPT2046 touch controller share the SPI bus (SCK/MOSI/MISO)
and are distinguished by separate chip selects.

| LCD board pin | Signal               | ESP32-S3 GPIO         |
| ------------- | -------------------- | --------------------- |
| VCC           | Power                | 3V3                   |
| GND           | Ground               | GND                   |
| SCK           | SPI clock (shared)   | GPIO 12               |
| SDI (MOSI)    | SPI data to panel    | GPIO 11               |
| SDO (MISO)    | SPI data (touch use) | GPIO 13               |
| CS            | Display chip select  | GPIO 10               |
| D/C           | Data/command select  | GPIO 9                |
| RESET         | Display reset        | GPIO 14               |
| LED           | Backlight (PWM)      | GPIO 15               |
| T_CS          | Touch chip select    | GPIO 8                |
| T_IRQ         | Touch pen interrupt  | GPIO 7                |
| T_CLK         | Touch clock          | GPIO 12 (shared SCK)  |
| T_DIN         | Touch data in        | GPIO 11 (shared MOSI) |
| T_DO / T_OUT  | Touch data out       | GPIO 13 (shared MISO) |

Notes:

- The display's own SDO (MISO) is not required by the firmware (the draw path is
  write-only) and may be left unconnected; only the touch T_DO/T_OUT line needs
  to reach the shared MISO pin.
- The LED (backlight) line has no pull and must be driven. Tie it to 3V3 for an
  always-on backlight, or to GPIO 15 for software-controlled dimming. Leaving it
  floating leaves the screen dark even when pixels are arriving correctly.
- The touch data-output pin is silkscreened **T_DO** on some board revisions and
  **T_OUT** on others; both name the same signal.

## ESP32-S3 to Camera (OV7670, parallel DVP)

RESET and PWDN are tied to fixed rails in hardware, **not** driven by a GPIO.
Polarity is critical: **RESET must be held HIGH (3.3V)** to keep the sensor out
of reset, and **PWDN must be held LOW (GND)** to keep it powered up. Swapping
these two holds the sensor simultaneously powered down and in reset, so it never
acknowledges on SCCB and the driver reports "Detected camera not supported".

| Camera module pin | Signal             | ESP32-S3 GPIO    |
| ----------------- | ------------------ | ---------------- |
| 3.3V              | Power              | 3V3              |
| GND               | Ground             | GND              |
| SDA               | SCCB data          | GPIO 4           |
| SCL               | SCCB clock         | GPIO 5           |
| VS                | VSYNC              | GPIO 6           |
| HS                | HREF               | GPIO 16          |
| PCLK              | Pixel clock        | GPIO 17          |
| MCLK (XCLK)       | Master clock       | GPIO 18          |
| D0                | Data bit 0         | GPIO 21          |
| D1                | Data bit 1         | GPIO 38          |
| D2                | Data bit 2         | GPIO 39          |
| D3                | Data bit 3         | GPIO 40          |
| D4                | Data bit 4         | GPIO 41          |
| D5                | Data bit 5         | GPIO 42          |
| D6                | Data bit 6         | GPIO 47          |
| D7                | Data bit 7         | GPIO 45          |
| RST               | Reset (active-low) | Tie HIGH to 3.3V |
| PWDN              | Power down         | Tie LOW to GND   |

> **Keep every camera jumper cable under 10 cm.** The no-FIFO OV7670 has no
> frame buffer to absorb DVP timing slop, so over longer jumper wires the
> D0–D7 and PCLK lines cannot settle between clock edges and the camera feed
> arrives corrupted — typically as structured horizontal-streak color noise
> while the frame geometry and sync stay intact. Short wiring (or a PCB) fixes
> it. If corruption persists at 10 cm, shorten the leads further or lower
> `BSP_CAM_XCLK_FREQ_HZ` from 10 MHz toward 8 MHz.

### Verification checks

- Measure **PWDN ≈ 0 V** and **RESET ≈ 3.3 V** at the module if the sensor
  won't probe.
- Solid color fills breaking into vertical color bands (while text renders fine)
  is a pixel byte-order problem, not a wiring fault.
- Torn or smeared pixels on the display point to too high an SPI clock for the
  wiring — lower `BSP_LCD_PIXEL_CLOCK_HZ` to confirm.
