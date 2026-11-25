# Star Controller: Wiring and Setup

## Parts
- Adafruit QT Py ESP32-C3
- NeoPixel Driver BFF (5645), data on A3 (default jumper)
- I2C QT Rotary Encoder with NeoPixel (4991), address 0x36
- Adafruit SPH0645 I2S MEMS mic breakout (3421)
- 4560 strand, 49 pixels (tree; one pixel was cut off), then 11x 1734 8 mm NeoPixels / APA106 (star)

## Chain order
Box -> strand (pixels 0-48, 49 pixels) -> star spikes (pixels 49-59).
Pixel 0 is at the box (electrically first). Pixel 59 is the star tip.
With the tree assembled, the star sits on top and the string unwraps down the tree, so the control box ends up under the tree.

## Wiring
**In the box**
- BFF JST PH (+, -, Sig) to the strand's input plug. The strand plug is probably a 3-pin JST SM, so use an adapter cable or a solder splice in the box. Check the strand's wire colors with clip leads first. Adafruit says the order can change between batches.
- 1000 uF cap across 5V and GND at the strand input. Stripe (-) to GND.
- Encoder: STEMMA QT cable to the QT Py's STEMMA QT port.
- Mic (SPH0645 I2S): **3V** (not 5V), GND to GND, **SEL to GND** (left channel), **BCLK to A0**, **DOUT to A1**, **LRCL/WS to A2**. If SEL is left floating or tied high (right channel), set `MIC_I2S_RIGHT` to `true` in the sketch.
- The SPH0645 breakout is about 0.65 x 0.5 in (16.5 x 12.7 mm). The box already has a horizontal mount for it under the lid sound holes, port side toward the lid, so it should fit as designed.

**Feed to the star**
- Run a 24-26 AWG 5V/GND pair from the box up to the star hub.
- At the hub, tie the strand's top-end V+/GND into that pair, and power all 11 spikes from it. That keeps current off the thin 28-30 AWG hub jumpers.

**Data at the star**
- Strand data out (top end) to spike 1 DIN (leg 1).
- Each spike's DOUT (leg 4) to the next spike's DIN (leg 1), through all 11.
- APA106-F5 pinout, counting from the **round side** with the flat edge on the right: 1 DIN, 2 VDD, 3 GND, 4 DOUT (DOUT is next to the flat edge). Leg length is not a reliable guide. (An earlier note that said "count from the flat side" was wrong and reversed power.)
- A 330 ohm resistor on the strand-to-spike-1 data wire is optional. If you use it, heat-shrink it just below the hub.
- 0.1 uF cap across VDD/GND at spike 1 is optional.

## Pins (QT Py ESP32-C3)
| Function | Pin | GPIO |
|---|---|---|
| NeoPixel data (via BFF) | A3 | 0 |
| Mic BCLK | A0 | 4 |
| Mic DOUT | A1 | 3 |
| Mic LRCL / WS | A2 | 1 |
| Mic SEL | GND | left channel |
| I2C SDA / SCL (STEMMA QT) | SDA / SCL | 5 / 6 |

## Software
- Arduino IDE, Boards Manager: **esp32 by Espressif** 3.x (this sketch was compiled on 3.3.12).
- Board: **Adafruit QT Py ESP32-C3**. Leave "USB CDC On Boot" Enabled so Serial works over USB.
- Partition Scheme: **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**. That layout has two OTA slots, which pull-updates need. The default 1.2MB slots are too small once Wi-Fi and the TLS certificate bundle are included.
- Libraries: **Adafruit NeoPixel**, **Adafruit seesaw Library** (it pulls in Adafruit BusIO), and **WiFiManager** by tzapu (2.0.17 or later). Wi-Fi, HTTPUpdate, ArduinoOTA, and **ESP_I2S** come with the esp32 core.
- Serial Monitor: 115200.
- If uploading fails: hold BOOT, tap RESET, let go of BOOT, then upload.

## Test mode (do this first)
1. Set `#define TEST_MODE 1` at the top and upload.
2. For 3 seconds: pixel 0 (at the box) is **red**, pixel 49 (first star LED) is **red**, pixel 59 (star tip) is **blue**.
   - Pixel 49 shows green: set `STAR_RGB_SWAP` to `false`.
   - Pixel 0 not red: the strand isn't GRB. Tell me what color it showed.
3. Then one white pixel walks through all 60, box to star. Serial prints which pixel is lit. Check it goes in order and nothing is skipped.
4. Serial also shows "seesaw found", the I2S mic startup line (sample count, DC, live/idle), and the estimated mA. Every 2 s it prints mic env / floor / level. Set `#define MIC_DEBUG 1` if you want those prints in normal mode too. Talk at the mic or play music: level should rise. If it stays at zero with a live mic, try `MIC_I2S_RIGHT true` (if SEL is not grounded) or `MIC_I2S_MSB true`.
5. Set `TEST_MODE` back to `0` and upload.

## Normal use
- Turn: brightness (saved).
- Click: next mode. The star flashes that mode's color once. Clicking when it's powered off turns it on.
- Hold about 1 second, then let go: power on/off. It switches when you let go, so a longer hold can mean something else. The lights fade briefly on or off instead of jumping.
- Hold about 10 seconds: open (or close) the Wi-Fi setup network. The lights keep running.
- Everything is on the encoder knob. The little light on the knob is the status indicator (see the friend's section below for the colors).

Click order (Off is last, and is not listed on the phone page): White pulse; Star white, tree color (first-boot default); Classic; Rainbow; Twinkle; Sound rainbow (knob sets sensitivity); Star white, rainbow twinkle; White twinkle; Sound white star (knob sets sensitivity); Color chaser (max brightness, knob sets speed); Off. Off clicks back to White pulse. Hold about 1 second and let go to power on/off; the phone page's top power button does the same.

It remembers mode, brightness, power, max current, star trim, and sensitivity. They're saved 3 s after the last change.

## Setup mode (set it for her brick)
1. Hold the knob button down while plugging in USB. Let go.
2. Turn to pick max current. Tree pixels at the box show it:
   - 1 lit = 1000 mA (knob LED blue)
   - 2 lit = 1500 mA (orange)
   - 3 lit = 1800 mA (red)
   Pick by the "Output" rating on the brick: 1 A brick = 1000, 2 A = 1500 or 1800. Never above 1800, because the BFF plug is good for about 2 A.
3. Click. Now the star and the top 10 tree pixels light warm white. Turn to set star brightness (10-100%, default 70%) until the star matches the tree.
4. Click to save and exit. A long press also saves and exits.

## Tuning constants (top of the sketch)
`TREE_COUNT`, `STAR_COUNT`, `STAR_RGB_SWAP`, `ENCODER_REVERSE` (flip if turning clockwise dims it), `DEFAULT_STAR_TRIM`, `MA_PER_CHANNEL_FULL` / `MA_IDLE_PER_PIXEL` (current estimate). Mic pins and channel: `MIC_BCLK_PIN`, `MIC_WS_PIN`, `MIC_DATA_PIN`, `MIC_I2S_RIGHT`, `MIC_I2S_MSB`.

The star zone starts at `STAR_FIRST`, which is derived from `TREE_COUNT` (49 on the full chain, so the star begins at pixel 49). For a single 8 mm LED bench test, set `TREE_COUNT` to 0 and `STAR_COUNT` to 1. Pixel 0 is then a star LED, and `STAR_RGB_SWAP` applies to it. Put them back to 49 and 11 for the full chain.

`FW_VERSION` and `UPDATE_MANIFEST_URL` are for pull updates. `ENABLE_ARDUINO_OTA` is off unless you want passworded LAN uploads while you have it on the bench (`LAN_OTA_PASSWORD`).

## Connect the star to her Wi-Fi (for her iPhone)

You can paste these steps to her. The lights work even if she never does this; this is only so the star can receive updates later.

The knob still does everything: turn it for brightness, click for the next mode, hold about a second and let go to turn it on or off, and hold about 10 seconds for Wi-Fi setup. When you click a new mode, the star itself flashes that mode's color once. When you turn it on or off, the lights fade briefly instead of jumping.

You can also use your iPhone once the star is on your home Wi-Fi.

1. Open **Safari** (it has to be Safari, not Chrome or another app).
2. In the address bar, type **christmasstar.local** and go there. iPhones understand names that end in .local, so you should not need the number. The page will also show the star's IP address at the bottom once it loads.
3. If the page does not open, the name may not have resolved. Hold the knob for about 10 seconds until it blinks blue, join **Christmas Star Setup** with password **starlight**, open the page that appears (or go to **http://192.168.4.1**), and tap **Info**. Look for the star's home-network IP address — not 192.168.4.1, which is only the setup network. Then go back to your home Wi-Fi, open Safari, and type **http://** followed by that address.
4. When the page is open, tap the **Share** button (the square with the arrow pointing up), then **Add to Home Screen**, then **Add**. The star icon will sit on your Home Screen like an app.

What the page does:

- The **power button** at the top toggles the lights on or off, the same as holding the knob for a second.
- **Brightness** is a slider. Turn the knob and this slider will catch up in a couple of seconds, and the other way around.
- The big buttons pick a lighting mode (Off is only on the knob, with one extra press after Color chaser). Power off is not a mode — it remembers the mode you were on.
- **Sound sensitivity** appears in Sound rainbow and Sound white star. **Chaser speed** appears in Color chaser.
- The small text at the bottom shows the firmware version, the Wi-Fi signal, and the IP address.

The current limit and star brightness trim stay on the knob only, in setup mode, so they cannot be changed by accident from the phone.

The little light on the knob tells you what the star is doing:

- A steady, dim color means it is on. That color matches the lighting mode. The light is off when the star is turned off.
- A blinking blue light means the star is waiting for you to join its setup network and pick your home Wi-Fi.
- A quick flash of green means it has joined your Wi-Fi. You can put your phone back on your home network.
- A purple light means it is updating itself. Please don't unplug it. The lights will fade, then it will restart on its own.
- A brief red blink means it lost your Wi-Fi, or an update didn't work. The lights will keep working either way.

The first time she plugs it in, or if you (or she) hold the knob for about 10 seconds, the star opens its own Wi-Fi network for about 10 minutes. The knob light blinks blue while that network is on.

1. On the iPhone, open **Settings**, then **Wi-Fi**.
2. Tap **Christmas Star Setup**. When it asks for a password, type **starlight** (all lowercase, one word).
3. A login page should pop up on its own. If it does not, open Safari and go to **http://192.168.4.1**
4. Tap her home Wi-Fi name, type the home Wi-Fi password, and save it.
5. The star leaves the setup network and joins her home Wi-Fi. The knob light flashes green, then goes back to the usual mode color. Her iPhone will also leave the setup network; she should rejoin her home Wi-Fi if it does not do that by itself.

To forget a saved network (for example if she changes her home password): hold the knob for 10 seconds to open the setup network again, join **Christmas Star Setup** with password **starlight**, open the page, tap **Info**, then **Erase**. Then set it up again from step 4.

If no one joins the setup network, it closes after about 10 minutes and the star goes back to using any network it already knew, or it just keeps lighting with no Wi-Fi.

## Phone control (for you)

Once the star is on her home Wi-Fi, it serves a control page at **http://christmasstar.local** (mDNS name `christmasstar`). iPhones resolve `.local` in Safari. The IP is printed on the Serial Monitor at 115200, shown at the bottom of the page, and listed on the Wi-Fi setup portal's **Info** page if she has to look it up.

The page and `GET /api/state` plus `POST /api/set` run in the net task on the core WebServer, so a slow phone cannot stall the animation. Changes are queued and applied on the same path as the knob (NVS debounce, star flash, power fade, current cap). Hidden setup items (max current, star trim) are not in the API. The body is capped at 256 bytes, writes are rate-limited, and mDNS plus the server restart after a Wi-Fi drop. The knob keeps working if the server misbehaves.

If you change the hostname, change `HOSTNAME` at the top of the sketch and the friend steps together. The first published build is `FW_VERSION` 1, `BUILD_TAG` "2026-11-24e power-toggle".

## Publishing an update (for you)

The full checklist is in `UPDATE.md` at the repo root. Short version:

She does not have to do anything. About a minute after the star is on her Wi-Fi, and then about once a day, it reads `manifest.json` and updates itself if that file's `version` is higher than `FW_VERSION` on the star.

The firmware always fetches:

`https://raw.githubusercontent.com/RRVogel/ChristmasStar/main/manifest.json`

The repo must stay **public**, on the **main** branch, with that file at the root. A private repo, a rename, or a different branch will break updates.

1. In `StarController.ino`, bump `FW_VERSION` (for example from 1 to 2). Also update `BUILD_TAG` so the serial boot line is easy to recognize. Do not change behavior you have not tested.
2. Flash the new build to your own star over USB first and use it. A bad build that boots but breaks Wi-Fi cannot be fixed over the air.
3. Compile with the same board and the **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)** partition scheme (two OTA slots). Do not use the default 1.2MB scheme; this firmware does not fit there.

```bash
cd firmware/StarController
arduino-cli compile --fqbn esp32:esp32:adafruit_qtpy_esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs --output-dir ota .
```

4. Put the app binary `ota/StarController.ino.bin` in the repo (same path, or a versioned name). GitHub Release assets also work; the star follows redirects. Do not use Git LFS.
5. Edit the root `manifest.json`. Only two fields are read: `version` (integer, must be higher than the firmware already on the star) and `url` (a direct http/https download of the `.bin`). There are no sha/size fields. Example with a file in the repo:

```json
{"version": 2, "url": "https://raw.githubusercontent.com/RRVogel/ChristmasStar/main/firmware/StarController/ota/StarController-fw2.bin"}
```

A Release asset also works, for example `https://github.com/RRVogel/ChristmasStar/releases/download/v2/StarController.ino.bin`.

6. Commit and push the manifest and the bin (or publish the Release, then push the manifest). The next time the star checks, the knob light goes purple, the lights fade, it flashes the new image, and it reboots. If the new firmware crashes in the first minute, this board's Arduino core will roll it back to the previous one. If it keeps running but Wi-Fi or OTA is broken, it cannot be updated again until it is plugged into a computer.

HTTPS uses the esp32 core's built-in certificate bundle, which checks that GitHub is really GitHub. If that check fails (for example if the clock has not set yet), the star retries once without checking the certificate so the update can still go through. The tradeoff: that fallback could accept a fake file on a hostile network. The bundle path is the one you want; the fallback is only a safety net. A 404 is treated as "no update yet" and is retried in 24 hours, not insecurely.
