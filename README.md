# Christmas Star

Firmware and docs for a USB-powered LED Christmas star and tree lights, built around an Adafruit QT Py ESP32-C3.

The star checks this repository once a day for updates and installs them on its own. This repo must stay **public**, named **RRVogel/ChristmasStar**, with the `main` branch and the file layout below. If the name, branch, or `manifest.json` path changes, installed stars cannot find updates.

A printed owner's guide is in [`docs/ChristmasStar_Guide.pdf`](docs/ChristmasStar_Guide.pdf). The steps to publish a new firmware build are in [`UPDATE.md`](UPDATE.md). Wiring, compile flags, and bench tests are in [`firmware/StarController/WIRING_AND_SETUP.md`](firmware/StarController/WIRING_AND_SETUP.md).

## Hardware

- **Board:** Adafruit QT Py ESP32-C3, with a NeoPixel Driver BFF (5645) on A3 and an I2C QT Rotary Encoder (4991) on STEMMA QT. An Adafruit SPH0645 I2S mic (3421) is used for the music modes.
- **Lights:** 49 GRB strand pixels (a 50-pixel string with one pixel cut off), then 11 APA106 8 mm star LEDs in RGB order (`STAR_RGB_SWAP` is on). The star stays bright in every mode.
- **Electrical order:** pixel 0 is at the control box, pixels 0–48 are the tree, pixels 49–59 are the star, pixel 59 is the tip.
- **APA106 pinout:** looking at the round side with the flat edge on the right: 1 DIN, 2 VDD, 3 GND, 4 DOUT (DOUT is next to the flat edge). Leg length is not a reliable guide.
- **Power:** USB 5 V. Use a charger rated **2 A or more**.

## Current firmware

- **FW_VERSION:** `1` (this integer is what the star compares against the manifest)
- **BUILD_TAG:** `2026-11-24e power-toggle` (shown on the serial boot line and used to label the build)
- Confirmed working on the hardware.

The published app binary is `firmware/StarController/ota/StarController.ino.bin` (1,487,152 bytes, SHA-256 `25b36566861d1d1c646d84e7e25b5caa7b211e3e3e77c43c0a8f56b94a5c3fbe`).

## Modes (knob click order)

The little light in the knob shows which mode is on. Turn the knob for brightness, except in the music shows (sensitivity) and Color chaser (speed). The star itself stays bright.

1. **White pulse** — warm white, the tree and star breathe (knob: warm white)
2. **Star white, tree color** — bright white star, tree slowly changes color (default; knob: white)
3. **Classic** — red, green and warm white tree, warm white star (knob: red)
4. **Rainbow** — rainbow colors flow up the tree into the star (knob: purple)
5. **Twinkle** — soft warm glow with sparkles (knob: blue)
6. **Sound rainbow** — music show: the tree fills with color and the star changes color with the beat (knob: green; turn for sensitivity)
7. **Star white, rainbow twinkle** — bright white star, rainbow twinkles on the tree (knob: light blue-green)
8. **White twinkle** — bright white star, soft white twinkles on the tree (knob: pale cool white)
9. **Sound white star** — music show: pulsing white star, color-changing tree (knob: orange; turn for sensitivity)
10. **Color chaser** — a burst of color climbs the tree and star at full brightness (knob: pink; turn for speed)
11. **Off** — lights off; one more push starts again at White pulse. Off is only on the knob, not on the phone mode list.

**Power on/off:** hold the knob about 1 second and let go, or use the power button at the top of the phone page. That is separate from Off and remembers the last mode.

## Knob and phone

- **Turn:** brightness (or sensitivity / chaser speed, as above)
- **Click:** next mode
- **Hold ~1 second, then let go:** power on/off
- **Hold ~10 seconds:** open or close the Wi-Fi setup network (knob blinks blue)
- **Phone page:** `http://christmasstar.local` on the home Wi-Fi (Safari on iPhone). The top button toggles power.

## Wi-Fi setup

The lights work without Wi-Fi. Setup is only needed for the phone page and for automatic updates.

1. Hold the knob about 10 seconds until it blinks blue (this also happens the first time, if no network is saved).
2. On the phone, join the Wi-Fi network **Christmas Star Setup**, password **starlight**.
3. A setup page should appear. If it does not, open a browser to `http://192.168.4.1`. If the phone warns that there is no internet, stay connected.
4. Choose the home network from the scanned list, enter its password, and save.
5. The knob flashes green when the star has joined. Put the phone back on the home Wi-Fi, then open `http://christmasstar.local`.

To forget a saved network, join the setup network again, open the page, tap Info, then Erase.

## Over-the-air updates

About a minute after the star joins Wi-Fi, and then about once a day, it fetches:

`https://raw.githubusercontent.com/RRVogel/ChristmasStar/main/manifest.json`

If that file's `version` is higher than `FW_VERSION` on the star, it downloads the `.bin` at the manifest `url`, flashes it, and reboots. The knob glows purple while it updates — do not unplug it then.

Only `version` (integer) and `url` (a direct http/https link) are read. The star follows redirects, so a GitHub Release asset URL also works. There are no sha or size fields.

See [UPDATE.md](UPDATE.md) before publishing a new build. Always test on a star in hand first. A build that boots but breaks Wi-Fi cannot be fixed over the air.

## Compile

Board: **Adafruit QT Py ESP32-C3**. USB CDC On Boot enabled. Partition scheme: **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**. Arduino-ESP32 core 3.x (this build used 3.3.12). Libraries: Adafruit NeoPixel, Adafruit seesaw, WiFiManager (tzapu) 2.0.17 or later.

```bash
cd firmware/StarController
arduino-cli compile --fqbn esp32:esp32:adafruit_qtpy_esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs --output-dir ota .
```

Do not use the default 1.2 MB partition scheme; this firmware does not fit there.

## Repository layout

```
manifest.json                         # what the star fetches
firmware/StarController/              # Arduino sketch and wiring notes
firmware/StarController/ota/          # published .bin files
docs/ChristmasStar_Guide.pdf          # printed owner's guide
docs/guide.html
README.md
UPDATE.md
```

## License

MIT, see LICENSE. Remixes welcome.
