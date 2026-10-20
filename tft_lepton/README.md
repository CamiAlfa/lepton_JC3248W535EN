# tft_lepton

ESP-IDF project for a thermal camera built on the **Guition JC3248W535C**
(ESP32-S3-WROOM-1 N16R8, 3.5" 320×480 IPS, AXS15231B QSPI controller, capacitive
touch) with a **FLIR Lepton 3** module.

**Status:** working end to end — live thermal video (Iron palette, 2x
upscaled) rendered on the display, at the Lepton's native ~8.7fps, with touch
active too.

Lepton support now uses
[**kampi/esp32-lepton**](https://components.espressif.com/components/kampi/esp32-lepton)
v0.0.3 instead of hand-rolled CCI/VoSPI code from earlier bring-up (removed —
see git history if you need to compare notes: it did get as far as real,
correctly-decoded thermal frames, just unreliably).

**Vendored, not fetched as a managed dependency.** The published package
doesn't build on modern ESP-IDF as-is: its `CMakeLists.txt` uses the pre-v4
`register_component()`/`COMPONENT_PRIV_REQUIRES` style, which doesn't
actually propagate requirements to the component's own sources on IDF v6 —
`src/VoSPI/vospi.cpp` failed on `driver/gpio.h: No such file or directory`
even though `driver` was declared. Separately, `driver`/`freertos` were
declared *private* when the component's own public headers need them
publicly. Fixed copy lives at
[`components/esp32-lepton/`](components/esp32-lepton/CMakeLists.txt) (sources
unchanged from upstream 0.0.3, only the build file rewritten) — see that
file's header comment for the full explanation. `main/idf_component.yml`
still pulls `espressif/esp-dsp` as a managed dependency (needed for CRC
checking; upstream's manifest doesn't declare that one either).

## Requirements

- ESP-IDF **v5.5.3+** (required by kampi/esp32-lepton; this project is
  built/tested on v6.0.1), with the `esp32s3` toolchain installed.
- The board's USB port (native USB-CDC / JTAG on the ESP32-S3).
- Note: `kampi/esp32-lepton` is **GPL-3.0** licensed — fine for this bring-up,
  but worth knowing before this ships in anything.

## Build & flash

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor        # replace COM5 with your port
```

Expected: `Lepton_Init ok`, part/serial number, `Starting capture...`, then a
`frames=... timeouts=... sync_errors=... driver_frame_counter=...` line every
3s, plus a raw pixel-value dump for the first 5 frames.

## Lepton 3 wiring

| Signal | GPIO | Notes |
|--------|------|-------|
| CCI SDA | 17 | I2C_NUM_1, device addr `0x2A` — set in [`main/main.cpp`](main/main.cpp) |
| CCI SCL | 18 | " |
| VoSPI CS | 7 | SPI3_HOST — set via **sdkconfig**, not code (see below) |
| VoSPI SCK | 15 | " |
| VoSPI MISO | 46 | " |
| VoSPI MOSI | — | unused — Lepton VoSPI never reads MOSI, don't wire/configure it |
| Power switch | 6 | MOSFET load switch on the sensor's supply rail (hardware addition, not part of the original board) — `1` = powered, `0` = cut. Set in code (`PIN_LEPTON_POWER`, `main/main.cpp`), not sdkconfig. See "Lepton power-down" below. |

VoSPI pins/host are `CONFIG_LEPTON_VOSPI_*` options
([`sdkconfig.defaults`](sdkconfig.defaults)), configurable via `idf.py
menuconfig` → *Component config* → *ESP32-Lepton*. CCI (I2C) pins are set in
code in `main.cpp` since the component takes an I2C bus config object rather
than reading Kconfig for it.

**Power:** give the Lepton its own supply, not the ESP32 board's 3.3V rail —
sharing it browned out imaging (CCI kept ACKing on the shared rail since it's
low-current, but VoSPI never produced real data) even though the rail looked
fine to the ESP32 itself. Confirmed during earlier hand-rolled bring-up;
almost certainly still applies here.

## Integrating kampi/esp32-lepton

This is a **C++** component — hence `main.cpp`, not `main.c` — and it doesn't
own an I2C driver: `main.cpp` supplies 5 callback functions
(`I2CM_Init/Deinit/Write/Read/WriteRead`) that the library calls back into,
implemented as thin wrappers around ESP-IDF's `driver/i2c_master.h`. This is
exactly the kind of glue most likely to have a first-try signature/behavior
mismatch — each callback logs its result, so a wiring problem there is
visible immediately rather than manifesting as a mysterious `Lepton_Init`
failure.

Frame buffers, telemetry parsing, VoSPI resync, and the capture task are all
handled internally by the component — `Lepton_StartCapture()` hands frames to
your app via a `QueueHandle_t` of `Lepton_FrameBuffer_t` (`Width`, `Height`,
`ImageBuffer` — 16-bit raw counts, `TelemetryBuffer`). `Lepton_GetSyncErrors`
/ `Lepton_GetFrameCounter` give the component's own internal diagnostics,
logged every 3s in `main.cpp`.

**If `Lepton_Init` fails:** check the `I2CM_*` log lines first (wrong
port/pins would show up there), then double-check the `CONFIG_LEPTON_VOSPI_*`
sdkconfig values against the wiring table above.

**If frames never arrive** (`timeouts` climbing, no `frame #N` lines): same
underlying VoSPI link as before, so the prior-bring-up lessons below still
apply — check power first, then wiring, before assuming a code problem.

Two real integration bugs hit during bring-up, both fixed in `main.cpp` now,
worth knowing if you're touching this code:

- **`Lepton_t Device` must be `static`** (or otherwise zero-initialized).
  `Lepton_Init()` never touches `Internal.CapHandle`; a plain stack-local
  `Lepton_t` leaves it as garbage, and if that garbage is non-NULL,
  `Lepton_StartCapture()` sees "already capturing" and fails with
  `LEPTON_ERR_BUSY` (`0xB008`) on the very first call. The component's own
  examples use a `static`/global `Lepton_t` for exactly this reason
  (undocumented).
- **The frame queue must have length 1.** `Lepton_CaptureTask()` pushes with
  `xQueueOverwrite()`, which FreeRTOS only permits on length-1 queues —
  anything else asserts (`xQueueGenericSend queue.c:938`) the moment a frame
  arrives. It's overwrite-latest semantics, not a real multi-slot queue, so
  there's no "use 2 for double buffering" option here.

A third bug, found later while wiring up the Calibrate button (below) and
fixed in the vendored `components/esp32-lepton/src/CCI/cci.cpp` itself
(sources otherwise unchanged from upstream 0.0.3 — see that file's own
comment):

- **`CCI_RunFFC()` always failed with `LEPTON_ERR_INVALID_ARG` (`0xB002`)
  before ever touching the I2C bus**, because it routed through `CCI_Set()`
  with `Length = 0` — and `CCI_Set()`'s very first check explicitly rejects
  `Length == 0`. `SYS_RUN_FFC` is a zero-payload "action" command (there's
  no data to write, just the command register), same as `SYS_RUN_PING`,
  which `CCI_WaitForBoot()` already handles correctly via a different,
  direct path (wait not-busy, write `CCI_REG_COMMAND` directly, wait
  busy-complete) that never goes through `CCI_Set()` at all. `CCI_RunFFC()`
  now follows that same direct-write pattern instead.

### Lepton power-down (new, added for the Shutdown button)

`CCI_PowerDownCamera()`/`Lepton_PowerDown()` (`CCI_CMD_OEM_RUN_POWER_DOWN`)
are additions, not part of upstream 0.0.3 — the vendored component didn't
expose an OEM power-down command at all. Needed because the Lepton has its
own power supply (see the wiring notes below): freeing CCI/VoSPI's GPIOs on
the ESP32 side (`Lepton_Deinit()`) never actually reduced the sensor's own
current draw, it just stopped the ESP32 from talking to it.

The command ID isn't in FLIR's public datasheet excerpts and was **not**
guessed — sending an arbitrary CCI value to real hardware seemed like a bad
place to be wrong, so it was cross-checked against
[`TheCacophonyProject/periph`'s Lepton CCI implementation](https://pkg.go.dev/github.com/TheCacophonyProject/periph/devices/lepton/cci)
(an independent open-source reading of the FLIR Software IDD), which
defines `oemPowerDown = 0x4800` sent via `cmd|2` for RUN-type commands (wire
value `0x4802`). That was verified against this codebase's own numbering
convention using the one already-working, already-vendored data point
available: `CCI_CMD_OEM_RUN_REBOOT = 0x4842` here is exactly periph's
`oemCameraReboot = 0x4840` with the same `|2` applied — so the same
transform on `0x4800` gives `0x4802`, expressed here as
`CCI_CMD_OEM_RUN_POWER_DOWN = 0x4000 + CCI_CMD_SDK_MODULE_OEM + 0x02`.

Implementation follows `CCI_RunFFC()`'s fixed pattern (direct
`CCI_WriteRegister()`, not `CCI_Set()`, for this zero-payload RUN command)
with one deliberate difference from `CCI_RebootCamera()`: no trailing
`CCI_WaitBusy()`. Reboot recovers and resumes responding after ~6s;
power-down's entire point is for the camera to stop responding until it's
rebooted or power-cycled, so waiting for a "not busy" that may never come
would just burn the default 5s `CCI_WaitBusy()` timeout for nothing.

**Update: a MOSFET load switch was added on the Lepton's supply rail**,
gated by `PIN_LEPTON_POWER` (`GPIO6`, `1` = powered, `0` = cut) —
`lepton_power_set()` in `main.cpp`. `perform_shutdown()` still sends
`Lepton_PowerDown()` first regardless (a graceful CCI shutdown before the
rail actually drops, not instead of it), then cuts the rail outright after
`Lepton_Deinit()` — no longer the software-only floor described above.
`lepton_power_set(true)` runs as early as possible in `app_main()` so the
sensor gets the most of the existing "wait for Lepton to boot" delay as
actual power-on time.

**This pin's level has to survive deep sleep the same way the backlight's
does** (`gpio_hold_en(PIN_LEPTON_POWER)`, alongside the same
`gpio_deep_sleep_hold_en()` global switch `bsp_display_shutdown()` already
enables) — and, importantly, **that hold persists into the *next* boot
too**. Per `gpio_hold_dis()`'s own ESP-IDF doc comment, a held pin stays
latched at its held level after wake/reset until the new boot explicitly
calls `gpio_hold_dis()` — a plain `gpio_set_level()` on it is silently
ineffective until then. Missing this would have meant the Lepton (and,
independently, the backlight — see below) could never be powered back on
after the very first Shutdown, stuck low forever. `lepton_power_set()`
calls `gpio_hold_dis()` before every level change for exactly this reason,
and the same fix was retrofitted into `bsp_display.c`'s `backlight_init()`
for the backlight pin, which had the identical latent bug already sitting
in the code from the earlier deep-sleep work — found while reasoning through
this one, not something that had actually failed yet.

**A second, separate latent bug found the same way, affecting the touch and
CCI I2C pins specifically**: `rtc_gpio_isolate()` (used on those pins in
`perform_shutdown()` — see "Further deep-sleep power reduction" below)
disconnects a pin from the digital IOMUX at the RTC/LP_IO mux level
entirely, and undoing that needs `rtc_gpio_deinit()` specifically — a
different mechanism from the hold/`gpio_hold_dis()` case above. Checked
`esp_driver_i2c/i2c_common.c` directly: the standard (non-LP) I2C pin setup
path this project's `i2c_new_master_bus()` calls go through
(`s_hp_i2c_pins_config()`) never calls `gpio_reset_pin()` or
`rtc_gpio_deinit()` on its own. Left unaddressed, touch and CCI I2C would
have silently stopped working on the very first boot after any Shutdown.
Fixed by calling `gpio_reset_pin()` (which calls `rtc_gpio_deinit()`
internally for any RTC-capable pin, confirmed against
`esp_driver_gpio/src/gpio.c`) on all four pins at the very top of
`app_main()`, before touch or CCI I2C ever get set up. (At the time this
was written, the SD card's bit-banged driver was isolated here too — its
own MISO pin didn't need a matching fix since that driver already called
`gpio_reset_pin()` on it as part of its own normal init. Moot now: the SD
card is native SDMMC, never isolated at all — see "SD card" below for why.)

### Panel deep sleep (`bsp_display_shutdown()`, `bsp_display.c`)

Turning the backlight off still left the *panel itself* drawing up to
~30mA. Cause: `esp_lcd_panel_disp_on_off(panel, false)` only sends DCS
Display-Off (`0x28`) — that blanks the output, but the controller's own
GRAM/oscillators stay powered. Actually dropping that needs the deeper
Sleep-In state (`0x10`).

`esp_lcd_panel_disp_sleep()` exists generically in this ESP-IDF version for
exactly this, but `esp_lcd_axs15231b` doesn't implement the `disp_sleep`
vfunc it needs — calling it returns `ESP_ERR_NOT_SUPPORTED` (verified
against `esp_lcd_panel_ops.c`: it checks `panel->disp_sleep != NULL` before
calling through, and grepping the component's own source turns up no
`disp_sleep` implementation anywhere). So `bsp_display_shutdown()` sends
`0x10` directly via `esp_lcd_panel_io_tx_param()` on the same panel IO
handle the component's own init table already uses for every other command.
Confirmed this panel actually follows standard DCS command/timing semantics
(not a given for every QSPI "DCS-like" controller) from that same init
table: `axs15231b_jc3248w535_init.h` sends `{0x11 (SLPOUT), ..., 120ms
delay}` during bring-up, so `0x10` (SLPIN) is its documented counterpart,
and the same 120ms settle time is applied symmetrically here.

**First attempt at this sent a bare `0x10` and measurably changed nothing.**
Root cause, found by reading `esp_lcd_axs15231b.c` directly: in QSPI mode
the component's own `tx_param()` helper (private, not exposed in the
public header) wraps every command as a 32-bit frame —
`(0x02 << 24) | ((cmd & 0xFF) << 8)`, `0x02` being its
`LCD_OPCODE_WRITE_CMD` — before ever calling `esp_lcd_panel_io_tx_param()`.
That's what `lcd_cmd_bits = 32` in `AXS15231B_PANEL_IO_QSPI_CONFIG` is
actually for. A bare `0x10` doesn't match that framing, so the panel had no
reason to treat it as SLPIN — every *other* command in this driver (the
init table, `disp_on_off`, etc.) goes through that same private helper and
was always framed correctly; only this direct bypass call had the bug.
`bsp_display_shutdown()` now replicates that exact frame by hand before
calling `esp_lcd_panel_io_tx_param()`, since the helper itself isn't public.

**Measured result: panel current dropped from ~30mA to ~20mA (steady-state,
measured well into actual deep sleep, not the brief window before it) —
this is the software floor for this panel, not a partial fix left to
chase further.** Reasoned through and confirmed rather than guessed at
further:

- The remaining ~20mA can't be the QSPI/SPI2 bus's software state (an
  un-freed `spi_bus_initialize()` from bring-up, say) — ESP32-S3
  automatically power-gates unused peripherals once deep sleep actually
  engages, regardless of whether the driver object was explicitly freed
  first. Confirmed the current was measured in that steady-state window,
  not the transient one right before `esp_deep_sleep_start()`, which ruled
  this out as something worth spending more code on.
- This board's display module has a fixed, always-on power rail shared
  with the ESP32 — no separate LDO-enable/load-switch line the ESP32 could
  drive to cut its VCI/VDD entirely, confirmed the same way the Lepton's
  supply was (see above). DCS Sleep-In reduces the *controller's* internal
  oscillator/DDVDD draw, but doesn't necessarily reach every analog bias
  generator on the panel FPC (VGH/VGL boost converters, VCOM generation) —
  those are likely where the remaining ~20mA lives, and nothing reachable
  by a QSPI command can power them down on this hardware. Same conclusion
  as the Lepton: the only way to eliminate this entirely would be new
  hardware (an external MOSFET/load switch spliced into the panel's own
  VCI line), not a software change.

### Unused audio amps (NS4168, MAX98357A) — desoldered, not a software fix

This board also carries two audio amplifier ICs this project never uses
(no audio anywhere in this codebase). Checked before writing any GPIO code:
their shutdown/enable pins are hardwired on this board, not connected to
any ESP32 GPIO — same "fixed, no software lever" situation as the Lepton's
and panel's power rails above, confirmed rather than guessed at (guessing
GPIO numbers for chip-enable lines with no schematic in hand was exactly
the kind of unverified hardware assumption worth avoiding here). With no
control pin to drive, the only fix was physical: **both chips were
desoldered from the board.** Total system current after that:
**~40mA.**

### Further deep-sleep power reduction (`perform_shutdown()` in `main.cpp`)

Three more things it does before `esp_deep_sleep_start()`, beyond stopping
the Lepton and putting the panel to sleep:

- **Holds the backlight pin low through the actual sleep period, not just
  at the moment of calling it.** Per ESP-IDF's own `gpio_hold_en()` docs, a
  GPIO's driven level is *not* guaranteed to persist once the digital
  domain powers down for deep sleep — without an explicit hold, the
  backlight pin could float (or revert to some default state) for the
  entire sleep period regardless of what `bsp_display_shutdown()` had just
  set it to, silently leaving the backlight circuit undefined — possibly
  still lit — for exactly the period this feature exists to save power
  during. `gpio_hold_en(PIN_NUM_LCD_BL)` latches that specific pin's level;
  `gpio_deep_sleep_hold_en()` is the separate global switch that makes any
  held pin's hold actually take effect across deep sleep. Both are new in
  `bsp_display_shutdown()`.
- **Stops WiFi cleanly** (`wifi_setup_stop()`, new — `esp_wifi_stop()`
  guarded by an internal "was it ever started" flag so it's a no-op when
  `wifi.json` was never found) rather than just relying on deep sleep's
  hard power cutoff.
- **Isolates I2C pins with internal pull-ups still enabled** — CCI SDA/SCL,
  touch SDA/SCL, and the SD bit-bang driver's MISO pin were all configured
  with `enable_internal_pullup = true` (or `gpio_set_pull_mode(...,
  GPIO_PULLUP_ONLY)` for MISO) at setup time. Checked directly against
  `esp_driver_i2c/i2c_common.c`'s `i2c_common_deinit_pins()`: tearing down
  an I2C bus (`i2c_del_master_bus()`, called by `Lepton_Deinit()` and
  whatever `bsp_touch.c` set up) disables the pins' signal routing but never
  touches their pull configuration, so an enabled internal pull-up stays
  configured and keeps drawing a small leakage current through deep sleep
  regardless. `rtc_gpio_isolate()` — the ESP-IDF-documented fix, explicitly
  "to reduce ... current consumption" per its own doc comment — disconnects
  a pin's input/output buffers and pull resistors entirely. Safe to call
  regardless of whether each pin's own driver was cleanly deinitialized
  first, since waking from this deep sleep is always a full reset (no wake
  source is configured — see the Shutdown button above), so every GPIO gets
  freshly reconfigured from scratch on the next boot either way.

### Notes carried over from prior (discarded) hand-rolled bring-up

Still relevant since the physical link (wiring, power, the sensor itself)
didn't change, even though the driver code did:

- **The Lepton's VoSPI state can get stuck** and needs a power cycle (of the
  *Lepton*, not just the ESP32) to recover — the exact same pins/clock
  produced both garbage and clean data across different sessions, with no
  wiring change in between. If data looks bad, power-cycle the Lepton before
  suspecting the wiring or the code. A CCI soft-reboot command was also being
  tested as a substitute for a physical power cycle — worth retrying with
  this component if the stuck-state issue resurfaces
  (`Lepton_RunCCI`/OEM reboot — check `include/CCI/cci.h` in the component for
  the exact call).
- **16MHz broke capture that worked cleanly at 10MHz** on this wiring
  (jumper leads). The component defaults to **20MHz** — if `Lepton_Init`
  succeeds but frames don't lock, try lowering `clock_speed_hz` in the VoSPI
  section of `LEPTON_DEFAULT_CONF` (in the component's
  `include/Definitions/lepton_config.h`, or override `Config.VoSPI.Interface`
  after copying the default) before assuming a deeper problem.
- Best confirmed result with the old code: 10MHz, steady (non-stalling) lock
  at ~2.3fps for 55+ seconds once the Lepton was in a clean state.

## Rendering to the display

`bsp_display.c`/`bsp_touch.c` (QSPI/AXS15231B panel + LVGL + capacitive
touch — see their own file comments for that hardware's bring-up notes:
board-specific init table, `full_refresh` requirement, RASET quirk, etc.) are
back in the build. `main.cpp`'s `start_display()` sets up a full-screen LVGL
canvas, image anchored top-left; the main loop's frame-receive branch calls
the library's own `Lepton_Raw14ToRGB()` (palette LUT + auto min/max contrast
— no need to reimplement normalization) into a PSRAM RGB888 scratch buffer,
then `render_thermal_frame()` upscales 2x and blits it into the canvas via
`lv_color_to_u16()` per pixel (native LVGL packing, avoids hand-rolling
RGB565 bit math).

**Right-side control strip** (x=320..480, the empty margin next to the
320x240 image). The status label lives at the bottom-left of the *screen*
instead (under the image, which only occupies y=0..240 of the 320-tall
canvas) — used only for transient messages: "Waiting for Lepton...", Save
progress/result, errors. No longer shows a running `frames`/`sync_errors`
counter -- still logged over serial every 3s if you need it, just not on
screen:

| Button | Does |
|--------|------|
| Freeze / Resume | `s_frozen` gates the render call in the main loop; frames still get pulled off the queue (keeps counters/watchdog honest) but the canvas stops updating, so it holds the last frame. |
| Save | Writes the currently-displayed frame as a BMP to the SD card (`/sdcard/lepton_NNNN.bmp`), with a live "Saving... NN%" progress readout in the status label. The counter is seeded at boot by scanning existing `lepton_NNNN.bmp` files (`init_save_counter()` in `main.cpp`), so numbering continues across reboots instead of restarting at 1 and overwriting old saves. See "SD card" below for how it avoids pausing the video feed. |
| Full | Rescales the image to cover the entire 480x320 canvas and hides every other UI element (buttons, status label). A single tap anywhere on the canvas exits back to normal mode (`canvas_click_cb()` in `main.cpp`). 160x120's 4:3 aspect doesn't evenly divide into the panel's 3:2, so full-screen stretches slightly rather than letterboxing — `render_thermal_frame()` uses destination-pixel-driven nearest-neighbor sampling (source pixel = `dst * srcSize / dstSize`) so the same code handles both the exact-2x normal case and full-screen's non-integer scale. |
| Palette | Cycles through every `LEPTON_PALETTE_*` value from `include/lepton_palette.h` (`Iron → Rainbow → Whitehot → Blackhot → Amber → Arctic → Lava →` back to `Iron`, `s_palette_index` wrapping via `% LEPTON_PALETTE_COUNT`), passed to `Lepton_Raw14ToRGB()`. **Whitehot (grayscale) is the default.** Unlike Freeze's action-not-state label convention, the button shows the palette that's now active (`kPaletteNames[s_palette_index]`) rather than "next", since "next of 7" doesn't read as clearly as a two-state toggle does. |
| Calib | Requests a Flat-Field Correction (`Lepton_RunCCI()`, which the vendored component maps to `CCI_CMD_SYS_RUN_FFC`) — the sensor closes its internal shutter and recalibrates against it. This is the standard fix for image retention/"persistent image" (a ghost of a previous scene burned into the readout) that Lepton sensors are prone to. Pure CCI (I2C) traffic, so unlike Save it never touches VoSPI/SPI3 and needs no pause-and-restart dance — just a brief real-world pause in the image while the shutter cycles. Also available from the web control page's Calibrate button (`POST /calibrate`); both routes funnel through the same request flag (`s_calibrate_requested`) so the CCI call only ever happens from `app_main`'s own task, since the vendored CCI interface isn't internally thread-safe against concurrent callers. |
| Shutdown | **Long-press only**, not a tap — bound to `LV_EVENT_LONG_PRESSED` instead of `LV_EVENT_CLICKED` (`make_button()` grew an optional event-code parameter for this one button) since this is the one control here you can't undo without physical access. `perform_shutdown()` stops Lepton capture (`Lepton_StopCapture()`), tells the sensor itself to power down over CCI (`Lepton_PowerDown()` — see "Lepton power-down" below), frees the ESP32-side CCI/VoSPI resources (`Lepton_Deinit()`), then cuts the sensor's power rail outright via a MOSFET load switch (`lepton_power_set(false)`, `PIN_LEPTON_POWER`/`GPIO6` — see "Lepton power-down" below), puts the panel into its own DCS Sleep-In state and turns off the backlight (`bsp_display_shutdown()`, new in `bsp_display.c/h` — see "Panel deep sleep" below), does a few more things to cut deep-sleep current further (see "Further deep-sleep power reduction" below), and calls `esp_deep_sleep_start()`, which never returns — on wake it's a fresh boot (`app_main` from the top), not a resumed call. **No wake source is configured**: the touch controller's INT pin isn't wired (`bsp_touch.c` uses `int_gpio_num = -1`), and there's no dedicated hardware button on this board, so the only way back is the board's EN/reset button or a power cycle — the same as switching the device off and on. Also on the web control page (`POST /shutdown`, `app_request_shutdown()`), gated behind a JS `confirm()` dialog instead of a long-press, same reasoning. |

### SD card

**Current: native 4-bit SDMMC**, after the board was physically rewired for
true SDMMC lines (hardware addition, not part of the original board — the
original GPIO table only routed `CMD`/`CLK`/one data line, SPI mode only).
Pins: `CLK`=12, `CMD`=11, `D0`=13, `D1`=42, `D2`=41, `D3`=10 (`D1`/`D2`
swapped relative to the originally-reported wiring — see below) — set in
[`main/sd_card.cpp`](main/sd_card.cpp). ESP32-S3's SDMMC peripheral routes
all of these through the GPIO matrix (unlike the classic ESP32, which has
two fixed-pin slots in the IOMUX) — confirmed directly against
`driver/sdmmc_default_configs.h` rather than assumed: its own ESP32-S3
`SDMMC_SLOT_CONFIG_DEFAULT()` uses non-contiguous pins (14, 15, 2, 4, 12,
13), which only makes sense if arbitrary GPIOs are supported, so this
board's custom pinout needed nothing beyond setting the `sdmmc_slot_config_t`
fields directly. No `SDMMC_SLOT_FLAG_INTERNAL_PULLUP` — the microSD
breakout board already has its own pull-ups on `CMD`/`D0`-`D3`.

This replaced two earlier approaches entirely (both removed outright, kept
here only as history — the problem they were solving, no free SPI host for
the SD card, doesn't exist anymore now that SD has its own dedicated
peripheral):

- **First pass**: time-share SPI3 with the Lepton's VoSPI, since ESP32-S3
  only has two general-purpose SPI controllers and both were already
  claimed (display QSPI on SPI2, Lepton VoSPI on SPI3) on different pins
  than the SD card, so no bus could be shared and no third controller
  existed to give the card its own. Fully tore down the Lepton
  (`Lepton_Deinit()`) before every save, mounted the SD card on the
  now-free SPI3, then brought the Lepton back with a fresh `Lepton_Init()`
  + `Lepton_StartCapture()`. Worked, but paused the video feed ~2-3s per
  save.
- **Second pass**: bit-banged (plain GPIO) SPI on CS=10/MOSI=11/CLK=12/
  MISO=13, a hand-rolled SD SPI-mode block driver (CMD0/CMD8/ACMD41 init
  handshake, CMD17/CMD24 single-block read/write) wired into FatFS as a
  custom diskio backend. No SPI host needed at all, so no more Lepton
  pause — but deliberately slow (~125-140kHz) and required real protocol
  debugging to get working (an SD-spec clock-speed ceiling during card
  identification, a missing busy/ready wait before every command, and
  — the actual root cause of the hardest bug in that effort — a card that
  silently gated its data phase on the command CRC even though CRC
  checking is supposed to default off in SPI mode, needing a real computed
  CRC7 instead of the usual dummy stop-bit-only byte).

The native SDMMC driver needs none of that hand-rolled protocol work —
ESP-IDF's own driver handles the SD command set, CRCs, and timing
correctly, at real SDMMC speeds instead of a deliberately slow bit-banged
clock. `GET_SECTOR_COUNT`/`f_mkfs` support comes along with the stock
driver too, though this project still never calls `f_mkfs` (the card is
expected to already be FAT-formatted).

**`SDMMC_HOST_DEFAULT()`'s 20MHz didn't work on this wiring** — mount
failed with `sdmmc_card_init failed (0x108)` /
`ESP_ERR_INVALID_RESPONSE`, logged as `"got corrupted data after
increasing clock frequency"`. First suspect was signal skew: ESP32-S3's
SDMMC peripheral routes every pin (`CLK`/`CMD`/`D0`-`D3`) through the GPIO
matrix here (see above), which adds skew the classic ESP32's fixed-IOMUX
SDMMC pins don't have, shrinking the timing margin at higher clock rates —
the same class of issue as the Lepton's VoSPI breaking at 16MHz but
working at 10MHz on this project's jumper-wire connections (see "Notes
carried over from prior... bring-up" below). Capping
`host.max_freq_khz = SDMMC_FREQ_PROBING` (400kHz) for every transfer, not
just card identification, was tried first on that theory — but the mount
**still failed the same way**, which ruled clock speed out: read
`sdmmc_init_host_frequency()` directly and confirmed it only raises the
clock when `card->max_freq_khz > SDMMC_FREQ_PROBING`, so with the cap in
place no clock raise was happening at all. The error message's "after
increasing clock frequency" framing turned out to be misleading here —
`sdmmc_check_scr()` just compares two reads of the same register and
blames a frequency change as the *usual* cause, but low-speed reads were
failing just as badly, pointing at a wiring fault on one of the data
lines instead of a speed/margin issue. Dropping to `slot_config.width = 1`
(removing `D1`/`D2`/`D3` from the transaction entirely) mounted reliably,
isolating the fault to one of those three pins. **Root cause: `D1` and
`D2` were physically swapped on the board** — fixed on the hardware side,
with `sd_card.cpp`'s pin macros updated to match (`D1`=42, `D2`=41, not
41/42), and `slot_config.width` back to `4`. With the real root cause
fixed, the `host.max_freq_khz = SDMMC_FREQ_PROBING` cap (400kHz) no
longer served any purpose — it was only ever working around the
(mis-diagnosed) margin theory — and was removed, so the host now runs at
`SDMMC_HOST_DEFAULT()`'s own 20MHz.

**Removing that cap alone didn't fix the ~3s-per-save time** — confirmed
on real hardware that saves were just as slow at 20MHz, which pointed
away from the bus clock entirely. The actual cause was in `main.cpp`'s
`write_bmp()`: it wrote the file as the 54-byte header plus 120 separate
~480-byte row `fwrite()` calls (one per BMP scanline, each re-flipped to
bottom-up BGR on the fly). Each of those 120 small writes was paying
fixed per-call overhead through the FatFS/SDMMC stack, dwarfing the
actual bus transfer time at any clock speed. Fixed by building the whole
~57KB file (header + pixels) into one buffer first — reusing
`app_build_frame_bmp()`, the same encoder the web server's `/save`
download already used — and writing it with a single `fwrite()` call, so
the SD stack moves it as a handful of large sector-sized transfers
instead of 120 tiny ones.

### WiFi

Optional, off by default. If `/sdcard/wifi.json` exists and mounts, it's read
right after the SD card mounts (`wifi_setup_start()` in `main.cpp`) and looks
like:

```json
{
  "ssid": "wifinetworkname",
  "password": "opensesame"
}
```

If found and valid, `wifi_setup.cpp` brings up WiFi station mode and connects
in the background (event-driven — connect on `WIFI_EVENT_STA_START`,
reconnect on `WIFI_EVENT_STA_DISCONNECTED`). Once an IP is assigned
(`IP_EVENT_STA_GOT_IP`), the main loop's existing 3s status tick in
`main.cpp` picks it up via `wifi_setup_get_ip()` and shows it in the
bottom-right corner of the screen (`s_wifi_label`) — shown once and left
alone after that, so a later disconnect/reconnect with a different IP won't
update it.

If the file is missing, unreadable, or malformed (bad JSON, missing/non-string
`ssid`/`password`), WiFi is simply never started — no error shown on screen,
just an `ESP_LOGI`/`ESP_LOGE` line over serial. The auth mode threshold is
deliberately left unset: per `esp_wifi_types_generic.h`, an unset threshold
with a password set defaults to WPA2, matching what the JSON's shape
(ssid+password, nothing else) implies — no separate "network type" field
needed.

JSON parsing uses `espressif/cjson` (managed dependency, MIT licensed) — not
bundled in-tree on this ESP-IDF version (moved out to idf-extra-components
at some point upstream).

### Web control page

Once WiFi connects, the IP shown on screen (or `http://<ip>/`) serves a
browser control page (`web_server.cpp`): a live video feed plus Freeze/Palette
buttons that drive the same `s_frozen`/`s_palette_index` state as the physical
touchscreen buttons (via `app_state.h`'s accessors — either source updates
both the screen and the browser), a Start/Stop Transmission button (see
below), a Calibrate button (see the Calib row in the touchscreen table
above — same request flag, same `POST /calibrate` endpoint, result shows on
the device's own screen either way), a "Save to PC" link that downloads
the current frame as a BMP straight to the browser's Downloads folder
(separate from, and unrelated to, the SD card Save button), and a Shutdown
button (see the Shutdown row above — same request flag, same deep-sleep
behavior, gated behind a JS `confirm()` dialog instead of a long-press).

The feed isn't real MJPEG (no JPEG encoder on-device) — it's the same
multipart/x-mixed-replace mechanism MJPEG uses, just with `image/bmp` parts
instead, at 160x120 (the Lepton's native resolution, same as the SD-card
BMPs) capped around 4fps client-side. Browsers display multipart BMP parts
in an `<img>` tag exactly like MJPEG; no library needed for it.

**Transmission is off by default and is a deliberate either/or with the
device's own screen**, not something that runs alongside it: the `<img>`
connects to `/stream` on page load same as before, but the connection just
idles (sends nothing) until the web page's Start Transmission button
enables it (`s_web_streaming` in `main.cpp`, toggled via `POST /transmit`).
While enabled, `render_thermal_frame()` skips the canvas blit + LVGL
invalidate + panel flush entirely (`Lepton_Raw14ToRGB()` still runs, so the
web feed itself stays live) — the on-screen status label shows "Streaming to
web..." in place of the frozen last frame so it's clear why the screen
stopped updating. See "sync errors" below for why driving both the LCD and
the web feed at once turned out not to be free.

**Two separate `httpd_start()` instances, control on :80 and the stream on
:81** — not an accident, and not simplifiable to one. `esp_http_server`
processes one request at a time on a single `select()`-loop task per
instance (confirmed against `esp_http_server/src/httpd_main.c`), and
`/stream`'s handler deliberately never returns while a client stays
connected. On a single shared server that would block every other
request — `/status`, `/freeze`, `/gray`, `/save`, even reloading `/` — for
as long as a browser tab keeps the feed open. This is the same fix the
official ESP32 CameraWebServer example uses; the control page's `<img>` src
is pointed at `:81` via one line of JS since a relative URL can't cross
ports.

`s_rgb_buf` (the RGB888 scratch buffer `render_thermal_frame()` writes and
blits from) used to only ever be touched from the one task that owns the
Lepton and display; the web feed's `app_build_frame_bmp()` added a second,
concurrent reader on the HTTP server's task. `s_rgb_mutex` guards it, held
for the *entire* render (Raw14ToRGB conversion + canvas blit) on the writer
side and the entire BMP encode on the reader side, so neither can observe a
half-updated frame.

**Connecting a browser measurably increases Lepton VoSPI sync errors**
(`Lepton-Capture: Sync error occurred`, logged by the vendored component).
The capture task is pinned to core 1 at a high priority
(`CONFIG_LEPTON_CAPTURE_TASK_CORE=1`, `_PRIORITY=16`) specifically so
FreeRTOS priority scheduling protects its VoSPI timing -- but ESP32-S3's
flash/PSRAM bus is shared across both cores, so heavy activity on core 0
(WiFi, TCP/IP, frequent PSRAM malloc/free -- all of which streaming adds)
can stall core 1 regardless of priority or core pinning, since that stall
happens at the bus-arbitration level below FreeRTOS's scheduler entirely.
Mitigations in `web_server.cpp`, aimed at reducing how often and how hard
that shared bus gets hit while a client is connected, not a root fix for the
underlying contention:
- Both `httpd_start()` instances are pinned to core 0 (`core_id = 0`),
  alongside WiFi's own task and `app_main`'s (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`,
  `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0`) -- keeps every WiFi-adjacent task off
  core 1 entirely, rather than just hoping priority scheduling sorts it out
  if they ever landed there (`core_id` defaults to `tskNO_AFFINITY`,
  i.e. either core, if left unset).
- The stream handler mallocs its BMP buffer **once per connection**
  (`app_build_frame_bmp_into()`) instead of once per frame
  (`app_build_frame_bmp()`, still used for the low-frequency `/save`
  download) -- cuts out repeated PSRAM allocator churn during the exact
  window sync errors were seen to track with.
- The stream rate was dropped from an initial ~6-7fps to ~4fps
  (`vTaskDelay(pdMS_TO_TICKS(250))`), trading feed smoothness for fewer
  encode+send cycles competing for the bus.

**Tested and confirmed**: sync errors track specifically with an open
`/stream` connection actively transmitting, not with WiFi being
connected/idle (ruling out RF interference on the Lepton's SPI wiring as the
cause, and confirming it's genuinely the streaming work's bus/network load).
A half-resolution stream was tried at this point and dropped again in favor
of a different approach (full resolution restored) that removes a
comparable amount of load from a different angle: the LCD-vs-web-feed
either/or described above. Driving the panel (canvas blit + QSPI flush,
every render) was itself a source of CPU0/bus activity competing with the
capture task, on top of the streaming work — stopping *both* when
transmission is off, and giving up the LCD (not the stream) as the tradeoff
when it's on, turned out to matter more than shrinking the stream's own
payload. If sync errors are still an issue while transmitting, dropping the
frame rate further (`vTaskDelay(pdMS_TO_TICKS(250))` in
`stream_get_handler()`) is the next thing to try.

## Layout

```
main/
  main.cpp             app_main: I2C HAL glue, Lepton bring-up, display render loop
  bsp_display.c/h      QSPI/AXS15231B panel + LVGL bring-up
  bsp_touch.c/h        Capacitive touch indev
  sd_card.cpp/h        native 4-bit SDMMC mount (see "SD card" above)
  wifi_setup.cpp/h     optional WiFi from /sdcard/wifi.json (see "WiFi" above)
  web_server.cpp/h     browser control page + video feed (see "Web control page" above)
  app_state.h          cross-task accessors main.cpp exposes to web_server.cpp
  idf_component.yml    managed component dependencies (esp-dsp, cjson, display/touch)
components/
  esp32-lepton/        vendored + fixed kampi/esp32-lepton 0.0.3 (see above)
partitions.csv         factory app 3 MB + 1 MB spiffs
sdkconfig.defaults     ESP32-S3 N16R8 flash/PSRAM config + Lepton VoSPI pins
```

## Next steps

- CCI-driven features the component exposes: AGC, emissivity, spotmeter/ROI.
- `PartNumber` `500-0726-01` (this module) is **non-radiometric** Lepton 3.0
  — `Lepton_GetPixelTemperature()` always returns `LEPTON_ERR_NOT_SUPPORTED`
  on it (confirmed via `Lepton_Init`'s own part-number check); only the 3.5
  (`500-0771-01`) is radiometric. No temperature-readout feature is planned
  unless the hardware changes.
