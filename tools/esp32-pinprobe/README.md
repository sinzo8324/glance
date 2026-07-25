# esp32-pinprobe — GeekMagic SmallTV Pro hardware diagnostic

A throwaway ESP32 firmware that verifies the **SmallTV Pro** pinout on *your*
physical unit before we commit pin numbers to the real Glance port. It:

1. drives the TFT with the community-reported candidate pins → the panel shows
   color bars + text if the wiring/driver is right (black = wrong pins or an
   alternate board revision);
2. scans **all 10** ESP32 capacitive-touch channels → touching the top of the
   case flips exactly one to `TOUCH`, revealing the button GPIO;
3. reports everything three ways — on the panel, over **Serial**, and at
   **http://pinprobe.local/** — so you can read it even with no UART attached;
4. serves its **own OTA updater** at `/update`, so you can re-flash it (or jump
   to real firmware) over WiFi without ever opening the case.

## Candidate pins under test (from community reverse-engineering)

| signal | GPIO | confidence |
|--------|------|-----------|
| TFT SCLK | 18 | high (3 sources) |
| TFT MOSI | 23 | high |
| TFT DC   | 2  | high |
| TFT RST  | 4  | high |
| TFT BL   | 25 (active LOW) | high |
| TFT CS   | **-1 (tied to GND)** or 3 | **unresolved — this probe decides it** |
| touch    | 32 (expected) | high, but confirmed by scan |

Panel: ST7789V, 240×240, colors inverted, SPI mode 3, 20 MHz.

## Flashing (no disassembly)

**First flash** — through the stock web console:

```bash
pio run -e pinprobe                       # builds .pio/build/pinprobe/firmware.bin
```

Open the device's stock web UI (it's at `192.168.0.228` right now) →
**System → Firmware Update** → upload that `firmware.bin`.

> ⚠️ Back up / keep the official stock `.bin` first (the ones in
> `smalltv-pro/firmware/`). If anything goes wrong you restore it the same way.
> If the stock updater rejects the image, you'll need a one-time UART flash
> (`pio run -e pinprobe -t upload`, GPIO0→GND on power-up) — after that, all
> further flashes go over the probe's own `/update`.

**Every flash after that** — over WiFi, no UART:
`http://pinprobe.local/update` → upload the new `firmware.bin`.

## Reading the result

On first boot the probe starts a `PinProbe-Setup` WiFi AP — join it and pick
your 2.4 GHz network (same flow as the main firmware). Then:

- **Screen shows color bars + text** → TFT pins/driver confirmed. 🎉
- **Screen black** → wrong TFT pins. Most likely the CS pin: edit
  `platformio.ini`, swap `-DTFT_CS=-1` for `-DTFT_CS=3`, re-flash via `/update`.
  Still black after that → you may have the alternate SparkleIoT XH-32S board
  revision (different pinout) — tell me and we'll probe GPIOs directly.
- **Touch the top of the case** → the row that turns green (on screen or at
  `pinprobe.local`) is your button GPIO. Expect `T9 / GPIO32`.

## Next step

Report back: (a) did the screen light up, (b) with CS=-1 or CS=3, (c) which
GPIO turned green on touch. Those three answers lock the Pro pin definitions
and we scaffold the real `[env:esp32-pro]` build in the main repo.
