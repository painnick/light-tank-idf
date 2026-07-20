# Design: ESP32-C3 Super Mini → ESP32-C6 Super Mini Migration

**Date:** 2026-07-17  
**Status:** Implemented — final PCB schematic updated 2026-07-20
**Project:** panzer2-idf / light-tank-idf

## Summary

Migrate the RC tank firmware from **ESP32-C3 Super Mini** to **ESP32-C6 Super Mini** only. Redesign pin defaults for a new C6 Super Mini PCB. Expose all GPIO assignments via **Kconfig** so PCB revisions can change pins without editing application logic.

## Goals

1. Build and run on `idf.py set-target esp32c6`.
2. Default pin map is PCB-routing-friendly for C6 Super Mini and avoids boot/USB hazards.
3. Every hardware pin is configurable under `idf.py menuconfig` → **RC Tank Hardware Pins**.
4. Shared app behavior (motors, turret, LEDs, DFPlayer, Bluepad32 BLE gamepad) stays the same.

## Non-goals

- Dual C3/C6 board support or runtime board detection.
- Editing EasyEDA schematic/PCB CAD files (document the pin map for the next PCB rev only).
- Changing gamepad, motor ramp, sound track, or control-loop behavior.
- Upgrading or refactoring Bluepad32/BTstack beyond what C6 build requires.

## Current state (C3)

| Function | GPIO | Notes |
|----------|------|--------|
| Left track IN1 / IN2 | 4 / 3 | DRV8833 Motor-IN-B1/B2 |
| Right track IN1 / IN2 | 0 / 5 | DRV8833 Motor-IN-A1/A2 |
| Cannon LED | 1 | |
| MG / HeadLight LED | 6 | Schematic notes boot glitch on GPIO6 (C3) |
| Turret servo | 7 | LEDC 50 Hz |
| DFPlayer TX / RX | 10 / 9 | UART1; avoid C3 UART0 20/21 |

Pins are hard-coded in `main/main.c` and `main/dfplayer.c`. Target is `CONFIG_IDF_TARGET="esp32c3"`.

## Hardware constraints (C6 Super Mini)

Avoid as application I/O:

| GPIO | Reason |
|------|--------|
| 8 | Onboard WS2812 + strapping |
| 9 | BOOT button + strapping |
| 12, 13 | USB-Serial/JTAG |
| 15 | Strapping (sometimes board LED) |
| 4, 5 | Strapping; motor glitch risk at boot |
| 16, 17 | Default UART0; leave free for console fallback |

Safe preferred I/O: **0, 1, 2, 3, 6, 7, 14, 18, 19, 20, 21, 22, 23**.

C6 Super Mini is taller and has a different pinout than C3 Super Mini; a **new PCB** is required (confirmed).

## Default pin map (final C6 Super Mini PCB)

Net names match `pcb/Schematic_Light-Tank_2026-07-20.png` and `main/Kconfig.projbuild`.

| Net / function | Kconfig symbol | Default GPIO | PCB rationale |
|----------------|----------------|--------------|---------------|
| Motor-IN-A1 (Right IN1) | `PIN_RIGHT_IN1` | **3** | Left-side motor cluster routed to DRV8833 IN1 |
| Motor-IN-A2 (Right IN2) | `PIN_RIGHT_IN2` | **2** | Left-side motor cluster routed to DRV8833 IN2 |
| Motor-IN-B1 (Left IN1) | `PIN_LEFT_IN1` | **1** | Left-side motor cluster routed to DRV8833 IN3 |
| Motor-IN-B2 (Left IN2) | `PIN_LEFT_IN2` | **0** | Left-side motor cluster routed to DRV8833 IN4 |
| DRV8833 nSLEEP | `PIN_MOTOR_NSLEEP` | **6** | Driver enable with boot-time pull-down |
| HeadLight | `PIN_HEADLIGHT` | **20** | MOSFET Q1 gate for headlight connector |
| CannonLED | `PIN_CANNON_LED` | **19** | Cannon LED connector |
| GatlingLED | `PIN_MG_LED` | **18** | Gatling LED connector |
| TurretServo | `PIN_TURRET_SERVO` | **21** | SG90 PWM signal on turret connector |
| MP3-RX / DFPlayer TX | `PIN_DFPLAYER_TX` | **22** | TX-only command path to DFPlayer RX |
| DFPlayer RX (optional) | `PIN_DFPLAYER_RX` | **-1** | Not wired on the final PCB |

**PCB recommendation:** Add a weak pull-down (~10k) on DRV8833 nSLEEP so tracks stay off while GPIOs are high-Z at reset. Pull-downs on IN1–IN4 are optional when nSLEEP is held low.

**Expansion / spare GPIOs:** EXT2 uses GPIO23 and EXT3 uses GPIO7. GPIO14 remains unassigned. Avoid using strapping, USB, and flash pins listed above.

## Configuration approach

### Kconfig (`main/Kconfig.projbuild`)

Menu: **RC Tank Hardware Pins**

- One `int` option per pin with `range` (GPIO 0–30, and `-1` allowed for DFPlayer RX).
- Defaults = table above.
- Help text names the schematic net and notes “avoid 8/9/12/13/15”.

Application code uses `CONFIG_PIN_*` only (no scattered magic numbers).

### Defaults files

| File | Role |
|------|------|
| `sdkconfig.defaults` | Target-agnostic: FreeRTOS, Bluepad32 platform, Wi-Fi off, flash size if shared, etc. Remove obsolete ESP32 classic `BTDM_*` entries that do not apply cleanly to C6. |
| `sdkconfig.defaults.esp32c6` | C6-only: BLE controller options, CPU frequency, USB console expectations as needed, any C6-specific BT stack flags. |

After migration, developers run:

```bat
idf.py set-target esp32c6
idf.py menuconfig   # optional pin tweaks
idf.py build
```

Regenerate `sdkconfig` via `set-target` rather than hand-editing C3 leftovers.

## Code changes

### `main/main.c`

- Replace `#define PIN_*` with `CONFIG_PIN_*` (or thin wrappers).
- Update comments from C3 to C6 Super Mini.
- Keep LEDC channel/timer layout (channels 0–4, timers 0–1).
- Keep task model: `tank_ctrl` + `bt_main` on core 0 (C6 is single high-performance core for app purposes like C3).

### `main/dfplayer.c`

- TX/RX from `CONFIG_PIN_DFPLAYER_TX` / `CONFIG_PIN_DFPLAYER_RX`.
- If RX is `-1`, configure UART with RX disabled / no pin (TX-only command path still works).
- Use `UART_SCLK_DEFAULT` instead of `UART_SCLK_APB` for C6-safe clock source selection.

### `main/Kconfig.projbuild`

- New file as described.

### Documentation

- `README.md`: MCU, final pin table, build target `esp32c6`, BLE-only note, and current PCB schematic.
- `pcb/Schematic_Light-Tank_2026-07-20.png`: final C6 Super Mini PCB schematic. The C3-era M3 Stuart image was removed.

## Peripheral / stack notes

- **Bluetooth:** Continue Bluepad32 + BTstack controller-only BLE. C6 supports BLE 5.x; gamepad path remains Simple HOG.
- **Wi-Fi:** Stay disabled.
- **LEDC / GPIO / UART:** Standard ESP-IDF drivers; no new components.
- **Build verify:** `idf.py set-target esp32c6` then `idf.py build` must succeed.

## Testing / acceptance

1. Clean configure for `esp32c6` with defaults builds without error.
2. Changing a pin in menuconfig and rebuilding changes only that assignment (spot-check generated `sdkconfig.h` / binary map if needed).
3. On hardware (when available): motors, servo, LEDs, DFPlayer, BLE gamepad connect/control.
4. Boot: no sustained motor drive before `app_main` GPIO init (hardware pull-downs + safe pin choice).

## Implementation outline (for plan)

1. Add `main/Kconfig.projbuild` with defaults.
2. Wire `main.c` / `dfplayer.c` to `CONFIG_PIN_*` and UART clock fix.
3. Split/update `sdkconfig.defaults` + add `sdkconfig.defaults.esp32c6`.
4. `idf.py set-target esp32c6` and fix any C6 config/compile issues.
5. Update `README.md`.
6. Verify build.

## Decisions log

| Decision | Choice |
|----------|--------|
| PCB | New C6 Super Mini layout (not drop-in on C3 PCB) |
| Pin config | Kconfig (menuconfig) |
| C3 support | Dropped (C6 only) |
| Structure | Kconfig pins + `sdkconfig.defaults.esp32c6` |
| Default pin cluster | Motors 3/2/1/0, nSLEEP 6, LEDs 20/19/18, servo 21, DFPlayer TX 22, RX -1 |
