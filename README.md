# Kotodama speaker firmware

Firmware for the Spotpear ESP32-S3 1.28" BOX: ESP32-S3 N16R8, a GC9A01 240×240 round display, CST816D touch and an
ES8311 codec. It is built with PlatformIO on the Arduino framework, using LVGL 9 and LovyanGFX. It speaks the
protocol in [../docs/PROTOCOL.md](../docs/PROTOCOL.md).

## What it does (v0.6)

1. **Wi-Fi.** With no saved network, it opens the access point `Kotodama-XXXX`. Join it, and a page at
   `http://192.168.4.1` lets you pick a network. The page has an *Advanced* field for the server URL, which defaults
   to `https://kotodama.mylife.ninja`. Networks pushed from Kotodama are added to the saved list and never removed.
2. **Find the server.** It polls `/api/v1/ping` until Kotodama answers, and sets its clock from the reply.
3. **Adoption.** It shows a 3-digit code. You enter the code in Kotodama, and the device saves its token in NVS.
4. **Online.** It loads its config, downloads the character animations (KANI, see `app/kani.py`) at the size the layout needs, and draws the layout.
   It then holds a WebSocket to Kotodama:
   - It applies config pushes live.
   - It sends status every 30 s and log lines every 2 s.
   - It handles the commands `reboot`, `identify`, `refresh_config`, `set_volume`, `play_state` and
     `update_firmware` (see *Updates* below).
   - The screen dims after `sleep_after_s` without a touch, and the character switches to its sleeping animation.
5. **Talking.** Tap the middle to talk: a chime, then it streams what it hears to Kotodama until you tap again or stop speaking for
   `audio.silence_s` (default 5 s, at most 30 s). Kotodama answers with speech, which the speaker plays while the character
   talks; a tap stops it. Drag around the edge
   to change the volume; the speaker's own volume wins over the config unless the volume in the config changes.
6. **BOOT button.**
   - Short press: mutes or unmutes the mic (remembered across restarts). The character shows its muted animation.
   - Hold 5 s: opens Wi-Fi setup on the next boot, keeping saved networks.
   - Hold 10 s: factory reset, forgetting Wi-Fi, the token and the server URL.

Not yet implemented: wake word, configurable sounds.

## Build and flash

```
python -m venv .venv
.venv\Scripts\pip install platformio
.venv\Scripts\pio run                 # build
.venv\Scripts\pio run -t erase        # first time only: wipes XiaoZhi (back it up first!)
.venv\Scripts\pio run -t upload       # flash over USB (COM4, see platformio.ini)
.venv\Scripts\pio device monitor      # serial log
```

## Updates

After the first USB flash, update over Wi-Fi: upload `.pio/build/kotodama-speaker/firmware.bin` on Kotodama's
*Firmware* page and press *Install*. The speaker downloads it, checks the SHA-256, writes the spare app slot and
restarts; the previous version stays in the other slot. Bump `FW_VERSION` in `platformio.ini` for every build
you upload (Kotodama reads it from the image).

**Releases.** Published releases live in the public repo [ninjalife/kotodama-firmware](https://gitlab.com/ninjalife/kotodama-firmware)
as `latest.json` plus `v<version>/firmware.bin`. After building a new `FW_VERSION`:

```
python ../tools/publish_firmware.py <kotodama-firmware clone> --push
```

Each speaker asks Kotodama for updates once when it starts; if `latest.json` names a newer version it shows
*Update available* with *Install* and *Dismiss* (a dismissed version isn't offered again; a newer one is).

To restore the stock XiaoZhi firmware, write the full backup back to address 0 with esptool.

## Notes

- `src/ca_certs.h` holds the Let's Encrypt roots (ISRG Root X1 and X2) used to verify the server.
- If the picture comes out rotated or mirrored, change `DISPLAY_ROTATION` in `src/board.cpp`. Values 4-7 are the
  mirrored variants.
- Battery percentage assumes a 1:2 divider on GPIO1. Charging is read from GPIO41, assumed active-low. Both still
  need checking against real readings.
