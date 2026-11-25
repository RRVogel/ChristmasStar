# Publishing a firmware update

The star already in England (or on a tree here) will install whatever this repository advertises. Follow these steps in order. Do not skip the USB test on a star you can reach.

## How the star decides to update

- Firmware constant `FW_VERSION` is a number (currently **4**). The human label `BUILD_TAG` (currently `2026-11-24h rainbow knob`) is only for the serial boot line and docs; it does not trigger an update.
- About 60 seconds after the star joins Wi-Fi, and then about once every 24 hours, it GETs:

  `https://raw.githubusercontent.com/RRVogel/ChristmasStar/main/manifest.json`

  That path is hard-coded. The repo must stay **public**, named **RRVogel/ChristmasStar**, with this file at the root of the **main** branch.
- The manifest is JSON with two fields the firmware actually reads:

  ```json
  {
    "version": 5,
    "url": "https://raw.githubusercontent.com/RRVogel/ChristmasStar/main/firmware/StarController/ota/StarController-fw5.bin"
  }
  ```

  `version` must be an integer greater than `FW_VERSION` on the star. `url` must be a direct `http://` or `https://` download of the app `.bin`. Extra fields are ignored. There are **no sha or size fields**; the star does not check a checksum from the manifest.
- Comparison is simply: if `manifest.version <= FW_VERSION`, log "up to date" and wait 24 hours. If it is higher, download and flash `url`, then reboot.
- The download follows redirects, so a GitHub Release asset such as `https://github.com/RRVogel/ChristmasStar/releases/download/v2/StarController.ino.bin` is fine. A raw file in this repo (as used for v1) is also fine and needs no Release. Do **not** use Git LFS; raw links to LFS files will not give the star a binary.
- A 404/403 is treated as "no manifest yet" and retried in 24 hours, quietly. Other errors are retried in 1 hour. Rebooting the star, or it rejoining Wi-Fi, starts a new check about 60 seconds later — useful when testing.
- If the new image crashes or watchdogs in the first ~60 seconds, this board's Arduino core rolls back to the previous image. If it keeps running but Wi-Fi or OTA is broken, **it cannot be fixed over the air**. Someone would have to plug it into a computer, or mail it back.

## Checklist

1. **Change the version in the sketch, and nothing else you have not tested.**
   - In `firmware/StarController/StarController.ino`, increment `FW_VERSION` (for example 4 → 5).
   - Set `BUILD_TAG` to a new label, for example `2026-11-24i …`, so the serial boot line is easy to recognize.
   - Leave `UPDATE_MANIFEST_URL` pointing at `https://raw.githubusercontent.com/RRVogel/ChristmasStar/main/manifest.json` unless the repo itself has moved.

2. **Compile with the same board and partition scheme.**

   ```bash
   cd firmware/StarController
   arduino-cli compile --fqbn esp32:esp32:adafruit_qtpy_esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs --output-dir ota .
   ```

   Board: Adafruit QT Py ESP32-C3, USB CDC On Boot enabled, partition scheme **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**. The default 1.2 MB scheme is too small. The file you want is `ota/StarController.ino.bin` (the app image). Do not publish the `.merged.bin`, bootloader, or partitions files.

3. **Flash that exact `.bin` to your own star over USB and use it.**
   Confirm it boots, modes and the knob still work, Wi-Fi reconnects, `christmasstar.local` still loads, and the serial line reads the new `FW_VERSION` and `BUILD_TAG`. Only after that is it safe to advertise the build.

4. **Add the binary to the repo (or to a GitHub Release).**
   - Preferred, matching earlier builds: copy it to a versioned name so the previous file remains, for example `firmware/StarController/ota/StarController-fw5.bin`.
   - Or attach `StarController.ino.bin` to a GitHub Release (for example tag `v2`).
   - Record the file size and SHA-256 for your own notes (`sha256sum`). The star will not read them.

5. **Point `manifest.json` at the new file and bump `version`.**
   Edit the root `manifest.json` last, in the same commit (or immediately after the Release asset is up). The `url` must already work when the star fetches it — a broken URL with a higher version will make it retry a failed flash every hour.

   Example for a file in the repo:

   ```json
   {
     "version": 5,
     "url": "https://raw.githubusercontent.com/RRVogel/ChristmasStar/main/firmware/StarController/ota/StarController-fw5.bin"
   }
   ```

   Example for a Release asset:

   ```json
   {
     "version": 5,
     "url": "https://github.com/RRVogel/ChristmasStar/releases/download/v5/StarController.ino.bin"
   }
   ```

6. **Commit and push source, binary, and manifest** (or publish the Release, then push the manifest). Also update this file's "Current published build" section. Push to **main**. Do not rename the repo or move `manifest.json`.

7. **Watch one update on a star that is still on the previous version**, if you have one. Reboot it (or unplug and plug it back in) and wait about a minute. The knob goes purple, the lights fade, it reboots, and serial should show the new `FW_VERSION` / `BUILD_TAG`. After about 60 seconds you should see the "app marked valid" line.

## Current published build

| Field | Value |
|---|---|
| `FW_VERSION` | 4 |
| `BUILD_TAG` | 2026-11-24h rainbow knob |
| Binary | `firmware/StarController/ota/StarController-fw4.bin` |
| Size | 1,487,840 bytes |
| SHA-256 | `626f82086fa6ca3191203e96c834630bfaeb75f6ce36cf7f2bd5fe8d5d8f0c1c` |
| Manifest URL the star polls | `https://raw.githubusercontent.com/RRVogel/ChristmasStar/main/manifest.json` |
| Manifest `version` | 4 (so a star already on this build sees no update) |

This build makes the Rainbow-mode knob light cycle through rainbow colors, so a steady purple means only that the star is updating. The v3 current-limit migration (`curv < 3`) is unchanged and will not run again. Older binaries remain: v1 `StarController.ino.bin`, v2 `StarController-fw2.bin`, v3 `StarController-fw3.bin`. The next published version should be **5**.

## What not to do

- Do not push a higher manifest `version` before the `.bin` URL is live and has been USB-tested.
- Do not make the repository private, rename it, switch the default branch away from `main`, or move `manifest.json`.
- Do not enable `ENABLE_ARDUINO_OTA` in a published build; it is off, and the unused LAN password in the sketch is only a placeholder.
- Do not put Wi-Fi passwords, tokens, or other secrets in this repository.
